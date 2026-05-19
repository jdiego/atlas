#pragma once

#include "atlas/pg/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <expected>
#include <variant>

namespace atlas {

namespace asio = boost::asio;

// Races `op` against a steady_timer of `duration`.
// On timeout: returns pg::error{pg::errc::query_canceled, "operation timed out"}.
// On success: cancels the timer and returns the operation result unchanged.
template<typename T, typename Duration>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration,
             asio::awaitable<std::expected<T, pg::error>> op)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Races an Asio coroutine against a deadline timer; whichever completes
     *   first wins, and the other is cancelled.
     *
     * Step 1 — co_await asio::this_coro::executor to obtain the current executor.
     * Step 2 — Construct asio::steady_timer timer{ex}; call timer.expires_after(duration).
     * Step 3 — Build a timer-awaitable lambda:
     *             auto wait = [&]() -> asio::awaitable<void> {
     *                 co_await timer.async_wait(asio::use_awaitable);
     *             };
     * Step 4 — Use awaitable_operators::|| to race op vs. wait():
     *             using namespace asio::experimental::awaitable_operators;
     *             auto result = co_await (std::move(op) || wait());
     *           result is std::variant<std::expected<T,pg::error>, std::monostate>.
     * Step 5 — If result.index() == 0: timer lost, op won → co_return std::get<0>(result).
     * Step 6 — If result.index() == 1: op lost, timer won →
     *             co_return std::unexpected(pg::error{pg::errc::query_canceled,
     *                                                 "operation timed out"});
     *
     * Key types involved:
     *   - asio::steady_timer: Boost.Asio timer (boost/asio/steady_timer.hpp)
     *   - asio::experimental::awaitable_operators::||: races two awaitables
     *   - std::variant: holds the winning result
     *
     * Preconditions:
     *   - op has not yet been co_awaited (it is an unevaluated awaitable).
     *   - duration > 0.
     *   - The coroutine runs on a live io_context executor.
     *
     * Postconditions:
     *   - Exactly one of op or the timer completes; the other receives a
     *     cancellation signal (operation_aborted).
     *   - On op success: the timer is cancelled before co_return.
     *   - On timeout: pg::errc::query_canceled is returned.
     *
     * Pitfalls:
     *   - asio::error::operation_aborted on timer.async_wait means the timer was
     *     cancelled by op completing first — this is normal, not an error.
     *   - The || operator cancels the losing branch automatically; do not manually
     *     call timer.cancel() after the race.
     *   - awaitable_operators::|| is in the "experimental" namespace; include
     *     <boost/asio/experimental/awaitable_operators.hpp>.
     *
     * Hint:
     *   The || result type is std::variant<A, B> where A and B are the value types
     *   of the two awaitables. Check result.index() to determine the winner.
     */
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer{ex};
    timer.expires_after(duration);

    auto wait_op = [&]() -> asio::awaitable<void> {
        auto ec = co_await timer.async_wait(asio::use_awaitable);
        (void)ec;
    };

    using namespace asio::experimental::awaitable_operators;
    auto result = co_await (std::move(op) || wait_op());

    if (result.index() == 0) {
        co_return std::get<0>(std::move(result));
    }
    co_return std::unexpected(pg::error{pg::errc::query_canceled, "operation timed out"});
}

// Overload accepting an external cancellation slot for cooperative abort.
// Installs the slot so callers can cancel the entire timed operation from outside.
template<typename T, typename Duration>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_timeout(Duration duration,
             asio::awaitable<std::expected<T, pg::error>> op,
             asio::cancellation_slot slot)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Like the two-argument overload but also installs an external cancellation
     *   slot so callers can abort the entire operation (including the timer).
     *
     * Step 1 — co_await asio::this_coro::executor to get ex.
     * Step 2 — Construct asio::steady_timer timer{ex}; timer.expires_after(duration).
     * Step 3 — Connect slot to a handler that calls timer.cancel() on activation:
     *             slot.assign([&timer](asio::cancellation_type) { timer.cancel(); });
     * Step 4 — Build timer wait coroutine (same as two-arg overload).
     * Step 5 — Race using awaitable_operators::||.
     * Step 6 — If timer won (index == 1): return pg_errc::query_canceled.
     *           If op won (index == 0): cancel timer, return op result.
     *
     * Key types involved:
     *   - asio::cancellation_slot: receives an external cancel signal
     *   - asio::cancellation_type: the kind of cancellation (total/partial/terminal)
     *
     * Preconditions:
     *   - slot is connected to a live cancellation_signal.
     *
     * Postconditions:
     *   - External slot.emit() or io_context shutdown both result in query_canceled.
     *
     * Pitfalls:
     *   - A slot can only be assigned one handler; calling assign() twice replaces
     *     the previous handler. Assign before starting the race.
     *   - Do not hold a reference to timer in the slot lambda if timer is destroyed
     *     before the slot is deassigned.
     *
     * Hint:
     *   slot.assign([&timer](asio::cancellation_type) { timer.cancel(); });
     *   Then proceed identically to the two-argument overload.
     */
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer{ex};
    timer.expires_after(duration);
    slot.assign([&timer](asio::cancellation_type) { timer.cancel(); });

    auto wait_op = [&]() -> asio::awaitable<void> {
        auto ec = co_await timer.async_wait(asio::use_awaitable);
        (void)ec;
    };

    using namespace asio::experimental::awaitable_operators;
    auto result = co_await (std::move(op) || wait_op());

    if (result.index() == 0) {
        co_return std::get<0>(std::move(result));
    }
    co_return std::unexpected(pg::error{pg::errc::query_canceled, "operation timed out"});
}

} // namespace atlas
