#pragma once

#include "atlas/async/pool_config.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

// Forward-declare the libpq opaque types to avoid pulling in libpq-fe.h in
// every translation unit that includes this header.
struct pg_conn;
using PGconn = pg_conn;
struct pg_result;
using PGresult = pg_result;

namespace atlas {

namespace asio = boost::asio;

using executor_type = asio::any_io_executor;

template <typename T>
using pg_expected = std::expected<T, pg::error>;

template <typename T>
using pg_awaitable = asio::awaitable<pg_expected<T>>;

class async_connection {
public:
    // Non-blocking factory. Calls PQconnectStart then polls via
    // sd_.async_wait(wait_write/wait_read) + PQconnectPoll until done.
    // `cleanup_budget` bounds the post-timeout cancel-and-drain sequence; a
    // pool passes its own pool_config value down here.
    [[nodiscard]] static pg_awaitable<async_connection>
    connect(executor_type executor, std::string_view connstr,
            std::chrono::milliseconds cleanup_budget = default_cleanup_budget);

    async_connection(const async_connection &) = delete;
    async_connection &operator=(const async_connection &) = delete;
    async_connection(async_connection &&) noexcept;
    async_connection &operator=(async_connection &&) noexcept;

    // Releases the stream_descriptor fd before calling PQfinish.
    ~async_connection();

    // Enqueues a parameterised query without blocking using PQsendQueryParams.
    // Returns immediately after enqueuing; call receive() for the result.
    [[nodiscard]] pg_expected<void> send_query(std::string_view sql, std::span<const char *const> params);

    // Waits for the next result.
    // Loop: PQisBusy → PQgetResult, waiting on the socket only when libpq has
    // nothing buffered. Returns nullopt at end of the result stream; callers
    // must keep calling until then or the connection cannot be reused.
    [[nodiscard]] pg_awaitable<std::optional<pg::result>> receive();

    // Convenience: send_query + loop receive() until nullopt.
    // Returns the last non-null result or an error.
    [[nodiscard]] pg_awaitable<pg::result> execute(std::string_view sql, std::span<const char *const> params);

    [[nodiscard]] bool is_alive() const noexcept;
    [[nodiscard]] bool is_nonblocking() const noexcept;
    [[nodiscard]] bool transaction_aborted() const noexcept;
    [[nodiscard]] int socket_fd() const noexcept;
    [[nodiscard]] int backend_pid() const noexcept;

    // Initiates server-side query cancellation through libpq's non-blocking
    // PGcancelConn state machine.
    [[nodiscard]] pg_awaitable<void> request_cancel();

    // Makes the connection reject further work. A pool replaces invalidated
    // connections when their lease is returned.
    void invalidate() noexcept;

    // How long with_timeout may spend cancelling and draining this connection
    // before giving up on it. Fixed when the connection is opened.
    [[nodiscard]] std::chrono::milliseconds cleanup_budget() const noexcept;

private:
    explicit async_connection(PGconn *raw, executor_type executor, std::chrono::milliseconds cleanup_budget);

    PGconn *pg_conn_ = nullptr;
    asio::posix::stream_descriptor conn_fd_;
    executor_type executor_;
    std::chrono::milliseconds cleanup_budget_ = default_cleanup_budget;
    bool query_in_progress_ = false;

    // Takes ownership of raw and maps its status to expected<result, error>.
    // Handles all ExecStatusType values; always calls PQclear on error paths.
    [[nodiscard]] pg_expected<pg::result> wrap_result(PGresult *raw);

    // Empties libpq's output buffer while servicing read and write readiness.
    // receive() calls this first so a partially-sent query cannot deadlock.
    [[nodiscard]] pg_awaitable<void> flush();

    void cleanup() noexcept;
};

} // namespace atlas
