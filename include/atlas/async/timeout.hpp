#pragma once

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

namespace detail {

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
template <typename T, typename Duration, cancellable_connection Connection>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration, Connection &conn, asio::awaitable<std::expected<T, pg::error>> op) {
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

    auto cancelled = co_await conn.request_cancel();
    if (!cancelled) {
        // The query may still be running. Waiting for it would violate the
        // deadline, while reusing the connection would expose its result to the
        // next operation.
        conn.invalidate();
    } else if (!co_await detail::drain_connection(conn)) {
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
