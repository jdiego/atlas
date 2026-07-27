#pragma once

#include "atlas/async/pool.hpp"
#include "atlas/pg/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>

namespace atlas {

namespace asio = boost::asio;

namespace detail {

[[nodiscard]] constexpr auto retry_delay(std::size_t attempt, std::chrono::milliseconds base_delay,
                                         std::chrono::milliseconds max_delay) noexcept -> std::chrono::milliseconds {
    if (base_delay.count() <= 0 || max_delay.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    if (base_delay >= max_delay) {
        return max_delay;
    }
    if (attempt >= std::numeric_limits<unsigned long long>::digits) {
        return max_delay;
    }

    const auto factor = 1ULL << attempt;
    const auto base = static_cast<unsigned long long>(base_delay.count());
    const auto cap = static_cast<unsigned long long>(max_delay.count());
    if (base > cap / factor) {
        return max_delay;
    }

    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(base * factor)};
}

} // namespace detail

// Operation must be callable with pool_connection& and return the matching
// awaitable type.
template <typename Op, typename T>
concept retryable_operation = requires(Op op, pool_connection &conn) {
    { op(conn) } -> std::same_as<asio::awaitable<std::expected<T, pg::error>>>;
};

// Retries op on transient PostgreSQL errors (serialization_failure,
// deadlock_detected, connection_failure), backing off exponentially between
// attempts: base_delay * 2^attempt, capped at max_delay. Non-retryable errors
// are propagated immediately, and a failure to acquire is never retried.
//
// Each attempt takes its own lease and gives it back before the backoff timer
// runs, so a retrying caller does not hold pool capacity while it waits.
// Because the pool may well hand the same connection back, a retryable failure
// is followed by a best-effort ROLLBACK: a serialization failure or deadlock
// leaves the server-side transaction aborted, and every later statement on that
// connection would fail until it is cleared.
template <typename T, typename Operation>
    requires retryable_operation<Operation, T>
[[nodiscard]] asio::awaitable<std::expected<T, pg::error>>
with_retry(pool &db, std::size_t max_attempts, Operation &&op,
           std::chrono::milliseconds base_delay = std::chrono::milliseconds{10},
           std::chrono::milliseconds max_delay = std::chrono::milliseconds{500}) {
    if (max_attempts == 0) {
        co_return std::unexpected(pg::error{"max_attempts must be at least 1", pg::errc::invalid_argument});
    }

    auto ex = co_await asio::this_coro::executor;

    // T need not be default-constructible, so the last error is held in an
    // optional rather than a pre-seeded expected.
    std::optional<std::expected<T, pg::error>> last_result;

    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        {
            // Scoped: the lease destructor returns the connection here, before
            // the backoff below rather than after it.
            auto conn_res = co_await db.acquire();
            if (!conn_res) {
                co_return std::unexpected(conn_res.error());
            }

            auto res = co_await op(*conn_res);

            if (res.has_value() || !res.error().is_retryable()) {
                co_return res;
            }

            // Only an explicit transaction in PQTRANS_INERROR needs cleanup.
            // Implicit transactions have already ended, and issuing ROLLBACK
            // there would generate a PostgreSQL warning.
            if (conn_res->transaction_aborted()) {
                static_cast<void>(co_await conn_res->execute("ROLLBACK", std::span<const char *const>{}));
            }

            last_result = std::move(res);
        }

        if (attempt + 1 == max_attempts) {
            break;
        }

        const auto delay = detail::retry_delay(attempt, base_delay, max_delay);

        asio::steady_timer timer{ex};
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
    }

    co_return std::move(*last_result);
}

} // namespace atlas
