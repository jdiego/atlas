#pragma once

#include "atlas/async/pool_config.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <expected>
#include <optional>
#include <span>
#include <string_view>

// Forward-declare the libpq opaque types to avoid pulling in libpq-fe.h in
// every translation unit that includes this header.
struct pg_conn;
using PGconn   = pg_conn;
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
    [[nodiscard]] static pg_awaitable<async_connection> connect(executor_type executor, std::string_view connstr);

    async_connection(const async_connection&)            = delete;
    async_connection& operator=(const async_connection&) = delete;
    async_connection(async_connection&&) noexcept;
    async_connection& operator=(async_connection&&) noexcept;

    // Releases the stream_descriptor fd before calling PQfinish.
    ~async_connection();

    // Enqueues a parameterised query without blocking using PQsendQueryParams.
    // Returns immediately after enqueuing; call receive() for the result.
    [[nodiscard]] pg_expected<void> send_query(std::string_view sql, std::span<const char* const> params);

    // Waits for the next result.
    // Loop: async_wait(wait_read) → PQconsumeInput → PQisBusy → PQgetResult.
    // Returns nullopt when PQgetResult returns nullptr (end of result stream).
    [[nodiscard]] pg_awaitable<std::optional<pg::result>> receive();

    // Convenience: send_query + loop receive() until nullopt.
    // Returns the last non-null result or an error.
    [[nodiscard]] pg_awaitable<pg::result> execute(std::string_view sql, std::span<const char* const> params);

    [[nodiscard]] bool is_alive()    const noexcept;
    [[nodiscard]] int  socket_fd()   const noexcept;
    [[nodiscard]] int  backend_pid() const noexcept;

    // Initiates server-side query cancellation via PQgetCancel + PQcancel.
    // Must be called before sd_.cancel() when aborting an in-flight query.
    [[nodiscard]] pg_expected<void> request_cancel() noexcept;

private:
    explicit async_connection(PGconn* raw, executor_type executor);

    PGconn*                          pg_conn_ = nullptr;
    asio::posix::stream_descriptor   conn_fd_;
    executor_type                    executor_;

    // Takes ownership of raw and maps its status to expected<result, error>.
    // Handles all ExecStatusType values; always calls PQclear on error paths.
    [[nodiscard]] pg_expected<pg::result> wrap_result(PGresult* raw);
    void cleanup() noexcept;
};

} // namespace atlas
