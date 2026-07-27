#pragma once

#include "atlas/async/async_connection.hpp"
#include "atlas/async/pool_config.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace atlas {

namespace asio = boost::asio;

class transaction;
class pool;

namespace detail {

// Shared pool state: the connection vector, the free list, the waiter queue and
// the strand that serialises them. Held by the pool, by every outstanding lease
// and by any detached work the pool spawns, so none of them can outlive it.
struct pool_state;

} // namespace detail

// RAII connection lease — returns the connection to the pool on destruction.
class pool_connection {
public:
    pool_connection(const pool_connection &) = delete;
    pool_connection &operator=(const pool_connection &) = delete;
    pool_connection(pool_connection &&) noexcept;
    pool_connection &operator=(pool_connection &&) noexcept;

    // Returns the connection to the pool if this lease still holds one.
    ~pool_connection();

    [[nodiscard]] std::expected<void, pg::error> send_query(std::string_view sql, std::span<const char *const> params);

    [[nodiscard]] asio::awaitable<std::expected<std::optional<pg::result>, pg::error>> receive();

    [[nodiscard]] asio::awaitable<std::expected<pg::result, pg::error>> execute(std::string_view sql,
                                                                                std::span<const char *const> params);

    // Asks the server to abort the query currently in flight. Cancelling the
    // awaitable alone leaves the query running server-side.
    [[nodiscard]] pg_awaitable<void> request_cancel();

    void invalidate() noexcept;

    [[nodiscard]] bool is_alive() const noexcept;
    [[nodiscard]] bool transaction_aborted() const noexcept;

private:
    friend class pool;
    pool_connection(async_connection *conn, std::shared_ptr<detail::pool_state> owner) noexcept;

    async_connection *conn_ = nullptr;
    std::shared_ptr<detail::pool_state> owner_;
};

// Async connection pool with strand serialisation and backpressure.
class pool {
public:
    explicit pool(executor_type ex, pool_config cfg);
    ~pool();

    pool(const pool &) = delete;
    pool &operator=(const pool &) = delete;

    // Acquires a free connection. Suspends the coroutine if none are available
    // until one is released. Respects pool_config::timeout.
    [[nodiscard]] asio::awaitable<std::expected<pool_connection, pg::error>> acquire();

    // Acquires a connection, sends BEGIN, and returns a transaction.
    [[nodiscard]] asio::awaitable<std::expected<transaction, pg::error>> begin();

    // Convenience: acquire + execute + release.
    [[nodiscard]] asio::awaitable<std::expected<pg::result, pg::error>> execute(std::string_view sql,
                                                                                std::span<const char *const> params);

    [[nodiscard]] std::size_t size() const noexcept;      // total connections
    [[nodiscard]] std::size_t available() const noexcept; // free connections

private:
    std::shared_ptr<detail::pool_state> state_;
};

} // namespace atlas
