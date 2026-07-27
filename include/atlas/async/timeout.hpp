#pragma once

#include "atlas/async/pool_config.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <utility>

namespace atlas {

namespace asio = boost::asio;

// A connection that can abort the query it currently has in flight and read the
// remainder of its result stream. Satisfied by async_connection and by
// pool_connection.
template <typename Connection>
concept cancellable_connection = requires(Connection &conn) {
    { conn.request_cancel() } -> std::same_as<asio::awaitable<std::expected<void, pg::error>>>;
    { conn.receive() } -> std::same_as<asio::awaitable<std::expected<std::optional<pg::result>, pg::error>>>;
    { conn.invalidate() } -> std::same_as<void>;
};

template <typename Connection>
concept cleanup_budget_provider = requires(const Connection &conn) {
    { conn.cleanup_budget() } -> std::same_as<std::chrono::milliseconds>;
};

namespace detail {

template <typename Connection>
[[nodiscard]] auto resolve_cleanup_budget(const Connection &conn,
                                          std::optional<std::chrono::milliseconds> override_budget)
    -> std::chrono::milliseconds {
    if (override_budget) {
        return *override_budget;
    }
    if constexpr (cleanup_budget_provider<Connection>) {
        return conn.cleanup_budget();
    }
    return default_cleanup_budget;
}

// Reads the result stream to its end and discards it, so the connection is
// reusable.
//
// A cancelled query reports an error result (SQLSTATE 57014) and only then the
// terminating sentinel, so stopping at the first error would leave the
// connection mid-stream — and a server-side error does not make PQstatus
// invalid, so nothing downstream would notice. Draining therefore continues
// past error results, and gives up only when two reads fail in a row, which
// means the transport itself is gone.
template <cancellable_connection Connection>
[[nodiscard]] asio::awaitable<bool> drain_connection(Connection &conn) {
    int consecutive_failures = 0;

    while (consecutive_failures < 2) {
        auto received = co_await conn.receive();
        if (!received) {
            ++consecutive_failures;
            continue;
        }
        if (!received->has_value()) {
            co_return true; // sentinel reached: the stream is fully consumed
        }
        consecutive_failures = 0;
    }

    co_return false;
}

// Cancels the query still running on `conn` and reads its result stream to the
// end, so the connection can be handed on. Returns false when the connection is
// not safe to reuse.
//
// The whole sequence runs against `budget`. Both steps talk to the server over
// the network — request_cancel() even opens a fresh connection to it — and the
// usual reason a query timed out in the first place is a server that stopped
// answering. Without a budget here, the cleanup could outlast the deadline
// with_timeout exists to enforce, by an unbounded margin.
template <cancellable_connection Connection>
[[nodiscard]] asio::awaitable<bool> reclaim_connection(Connection &conn, std::chrono::milliseconds budget) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer deadline{ex};
    deadline.expires_after(budget);

    auto reclaim = [&conn]() -> asio::awaitable<bool> {
        auto cancelled = co_await conn.request_cancel();
        if (!cancelled) {
            // The query may still be running, so its results would surface in
            // whatever the next caller does with this connection.
            co_return false;
        }
        co_return co_await drain_connection(conn);
    };

    auto wait_deadline = [&deadline]() -> asio::awaitable<void> {
        auto [ec] = co_await deadline.async_wait(asio::as_tuple(asio::use_awaitable));
        static_cast<void>(ec);
    };

    using namespace asio::experimental::awaitable_operators;
    auto outcome = co_await (reclaim() || wait_deadline());

    if (outcome.index() == 0) {
        co_return std::get<0>(outcome);
    }
    co_return false; // the budget ran out part-way through
}

// Detaches the handler installed on a cancellation slot. The handler holds a
// reference to a timer that dies with the enclosing coroutine frame, so leaving
// it installed would let a later emit() run over freed memory.
struct slot_guard {
    explicit slot_guard(asio::cancellation_slot s) : slot{std::move(s)} {
    }

    slot_guard(const slot_guard &) = delete;
    slot_guard &operator=(const slot_guard &) = delete;
    slot_guard(slot_guard &&) = delete;
    slot_guard &operator=(slot_guard &&) = delete;

    ~slot_guard() {
        slot.clear();
    }

    asio::cancellation_slot slot;
};

} // namespace detail

// Races `op` against a steady_timer of `duration`.
// On timeout: returns pg::error{pg::errc::query_canceled, "operation timed out"}.
// On success: cancels the timer and returns the operation result unchanged.
//
// This cancels the awaitable only. The query keeps running on the server and
// its results stay queued on the connection — prefer the overload taking a
// connection whenever the connection is reused afterwards.
template <typename T, typename Duration>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration, asio::awaitable<std::expected<T, pg::error>> op) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer{ex};
    timer.expires_after(duration);

    auto wait_op = [&timer]() -> asio::awaitable<void> {
        // operation_aborted here just means `op` won the race.
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        static_cast<void>(ec);
    };

    using namespace asio::experimental::awaitable_operators;
    auto result = co_await (std::move(op) || wait_op());

    if (result.index() == 0) {
        co_return std::get<0>(std::move(result));
    }
    co_return std::unexpected(pg::error{"operation timed out", pg::errc::query_canceled});
}

// Races `op` against `duration` and, on timeout, aborts the query server-side
// and drains the connection so the next user of it starts from a clean state.
//
// The connection is invalidated whenever that cleanup cannot be completed —
// including when it overruns its budget. A pooled connection is then replaced
// on release rather than handed on mid-stream.
//
// The cleanup budget comes from the per-call override, then from an optional
// connection provider, and finally from default_cleanup_budget.
template <typename T, typename Duration, cancellable_connection Connection>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration, Connection &conn, asio::awaitable<std::expected<T, pg::error>> op,
             std::optional<std::chrono::milliseconds> cleanup_budget = std::nullopt) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer{ex};
    timer.expires_after(duration);

    auto wait_op = [&timer]() -> asio::awaitable<void> {
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        static_cast<void>(ec);
    };

    using namespace asio::experimental::awaitable_operators;
    auto result = co_await (std::move(op) || wait_op());

    if (result.index() == 0) {
        co_return std::get<0>(std::move(result));
    }

    const auto effective_cleanup_budget = detail::resolve_cleanup_budget(conn, cleanup_budget);
    if (!co_await detail::reclaim_connection(conn, effective_cleanup_budget)) {
        conn.invalidate();
    }

    co_return std::unexpected(pg::error{"operation timed out", pg::errc::query_canceled});
}

// Overload accepting an external cancellation slot so callers can abort the
// whole timed operation from outside. The slot is cleared before returning.
template <typename T, typename Duration>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration, asio::awaitable<std::expected<T, pg::error>> op, asio::cancellation_slot slot) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer{ex};
    timer.expires_after(duration);

    detail::slot_guard guard{slot};
    if (slot.is_connected()) {
        slot.assign([&timer](asio::cancellation_type) { timer.cancel(); });
    }

    auto wait_op = [&timer]() -> asio::awaitable<void> {
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        static_cast<void>(ec);
    };

    using namespace asio::experimental::awaitable_operators;
    auto result = co_await (std::move(op) || wait_op());

    if (result.index() == 0) {
        co_return std::get<0>(std::move(result));
    }
    co_return std::unexpected(pg::error{"operation timed out", pg::errc::query_canceled});
}

} // namespace atlas
