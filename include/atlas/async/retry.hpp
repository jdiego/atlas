#pragma once

#include "atlas/pg/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <expected>
namespace atlas {

namespace asio = boost::asio;

// Forward declaration — pool and pool_connection are defined in pool.hpp.
class pool;
class pool_connection;

// Concept: Operation must be callable with pool_connection& and return the
// correct awaitable type.
template <typename Op, typename T>
concept retryable_operation = requires(Op op, pool_connection &conn) {
    { op(conn) } -> std::same_as<asio::awaitable<std::expected<T, pg::error>>>;
};

// Retries op on transient PostgreSQL errors (serialization_failure, deadlock_detected).
// Acquires a fresh connection from db on each attempt.
// Uses exponential backoff: base_delay * 2^attempt, capped at max_delay.
// Non-retryable errors are propagated immediately without further attempts.
template <typename T, typename Operation>
    requires retryable_operation<Operation, T>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_retry(pool &db, std::size_t max_attempts, Operation &&op,
           std::chrono::milliseconds base_delay = std::chrono::milliseconds{10},
           std::chrono::milliseconds max_delay = std::chrono::milliseconds{500}) {
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Attempts op up to max_attempts times, retrying only when the error is
     *   retryable (pg::error::is_retryable() == true). Applies exponential
     *   backoff between attempts using a steady_timer.
     *
     * Step 1 — auto ex = co_await asio::this_coro::executor.
     * Step 2 — Declare std::expected<T, pg::error> last_result (will hold the last error).
     * Step 3 — for (std::size_t attempt = 0; attempt < max_attempts; ++attempt):
     *   a. auto conn_res = co_await db.acquire();
     *      if (!conn_res) { co_return std::unexpected(conn_res.error()); }
     *   b. auto res = co_await op(*conn_res);
     *      (pool_connection RAII destructor releases conn here when conn_res goes out of scope)
     *   c. if (res.has_value()) { co_return res; }          // success
     *   d. if (!res.error().is_retryable()) { co_return res; } // hard error
     *   e. last_result = res;                                // save for final return
     *   f. if (attempt + 1 == max_attempts) break;           // no more retries
     *   g. Compute delay:
     *        auto shift  = std::min(attempt, std::size_t{20}); // guard against overflow
     *        auto delay  = base_delay * (1u << shift);
     *        if (delay > max_delay) delay = max_delay;
     *   h. asio::steady_timer timer{ex};
     *      timer.expires_after(delay);
     *      co_await timer.async_wait(asio::use_awaitable);
     * Step 4 — co_return last_result (exhausted retries).
     *
     * Key types involved:
     *   - pool::acquire(): returns awaitable<expected<pool_connection, pg::error>>
     *   - pool_connection: RAII handle; destructor releases conn back to pool
     *   - pg::error::is_retryable(): true for 40001 and 40P01 errors
     *   - asio::steady_timer: implements the backoff sleep
     *
     * Preconditions:
     *   - max_attempts >= 1.
     *   - op is callable with pool_connection& and matches retryable_operation<Op, T>.
     *   - db is a live pool with at least one connection available.
     *
     * Postconditions:
     *   - On first success: returns the result; total attempts == 1 + retries.
     *   - On non-retryable error: returns immediately; total attempts == 1.
     *   - On exhausted retries: returns the last retryable error.
     *
     * Pitfalls:
     *   - A new connection is acquired per attempt because a serialization_failure
     *     may leave the previous connection's transaction in an aborted state.
     *   - 1u << attempt overflows for attempt >= 32; cap with min(attempt, 20).
     *   - base_delay * (1u << shift) may overflow chrono::milliseconds; cast to
     *     a wider type if base_delay is large.
     *   - Do not retry pg::errc::connection_failure blindly; is_retryable() covers it,
     *     but pool::acquire() may itself fail — propagate that immediately (step 3a).
     *
     * Hint:
     *   pool_connection conn_res goes out of scope at the end of the loop body,
     *   so its destructor returns the connection to the pool automatically.
     *   Do not call release() manually.
     */
    auto ex = co_await asio::this_coro::executor;
    std::expected<T, pg::error> last_result = std::unexpected(pg::error{pg::errc::unknown, "no attempts made"});

    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        auto conn_res = co_await db.acquire();
        if (!conn_res) {
            co_return std::unexpected(conn_res.error());
        }

        auto res = co_await op(*conn_res);

        if (res.has_value()) {
            co_return res;
        }
        if (!res.error().is_retryable()) {
            co_return res;
        }

        last_result = res;

        if (attempt + 1 == max_attempts) {
            break;
        }

        auto shift = std::min(attempt, std::size_t{20});
        auto delay = base_delay * static_cast<long long>(1u << shift);
        if (delay > max_delay) {
            delay = max_delay;
        }

        asio::steady_timer timer{ex};
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
    }

    co_return last_result;
}

} // namespace atlas
