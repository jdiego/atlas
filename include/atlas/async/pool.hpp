#pragma once

#include "atlas/async/async_connection.hpp"
#include "atlas/async/pool_config.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <queue>
#include <span>
#include <string_view>
#include <vector>

namespace atlas {

namespace asio = boost::asio;

class transaction;
class pool;

// RAII connection lease — returns the connection to the pool on destruction.
class pool_connection {
public:
    pool_connection(const pool_connection&)            = delete;
    pool_connection& operator=(const pool_connection&) = delete;
    pool_connection(pool_connection&&) noexcept;
    pool_connection& operator=(pool_connection&&) noexcept;

    // Calls pool::release() if conn_ is not null.
    ~pool_connection();

    [[nodiscard]] std::expected<void, pg::error>
    send_query(std::string_view sql, std::span<const char* const> params);

    [[nodiscard]] asio::awaitable<std::expected<std::optional<pg::result>, pg::error>>
    receive();

    [[nodiscard]] asio::awaitable<std::expected<pg::result, pg::error>>
    execute(std::string_view sql, std::span<const char* const> params);

    [[nodiscard]] bool is_alive() const noexcept;

private:
    friend class pool;
    pool_connection(async_connection* conn, pool* owner) noexcept;

    async_connection* conn_  = nullptr;
    pool*             owner_ = nullptr;
};

// Async connection pool with strand serialisation and backpressure.
class pool {
public:
    explicit pool(executor_type ex, pool_config cfg);
    ~pool();

    pool(const pool&)            = delete;
    pool& operator=(const pool&) = delete;

    // Acquires a free connection. Suspends the coroutine if none are available
    // until one is released. Respects pool_config::timeout.
    [[nodiscard]] asio::awaitable<std::expected<pool_connection, pg::error>>
    acquire();

    // Acquires a connection, sends BEGIN, and returns a transaction.
    [[nodiscard]] asio::awaitable<std::expected<transaction, pg::error>>
    begin();

    // Convenience: acquire + execute + release.
    [[nodiscard]] asio::awaitable<std::expected<pg::result, pg::error>>
    execute(std::string_view sql, std::span<const char* const> params);

    [[nodiscard]] std::size_t size()      const noexcept; // total connections
    [[nodiscard]] std::size_t available() const noexcept; // free connections

private:
    friend class pool_connection;

    // Returns a connection to the pool. If waiters are queued, hands off
    // directly. Must be called (via dispatch) from within the strand.
    void release(async_connection* conn);

    // Sequentially connects cfg_.max_size connections and fills free_.
    asio::awaitable<void> initialise();

    executor_type                                      ex_;
    asio::strand<executor_type>                        strand_;
    pool_config                                        cfg_;
    std::vector<async_connection>                      connections_;
    std::vector<async_connection*>                     free_;
    std::queue<std::function<bool(async_connection*)>> waiters_;
};

} // namespace atlas
