#include "atlas/async/async_connection.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"
#include "pg/detail/result_handle_adopter.hpp"
#include <boost/asio/use_awaitable.hpp>
#include <libpq-fe.h>

#include <string>
#include <utility>

namespace atlas {

namespace {

[[nodiscard]] auto is_success_status(ExecStatusType status) noexcept -> bool
{
    switch (status) {
    case PGRES_EMPTY_QUERY:
    case PGRES_COMMAND_OK:
    case PGRES_TUPLES_OK:
    case PGRES_COPY_OUT:
    case PGRES_COPY_IN:
    case PGRES_COPY_BOTH:
    case PGRES_SINGLE_TUPLE:
    case PGRES_PIPELINE_SYNC:
    case PGRES_PIPELINE_ABORTED:
    case PGRES_TUPLES_CHUNK:
        return true;
    default:
        return false;
    }
}

} // namespace

// ── Private constructor ────────────────────────────────────────────────────

async_connection::async_connection(PGconn* raw, executor_type ex)
    : pg_conn_{raw}
    , conn_fd_{ex, PQsocket(raw)}
    , executor_{std::move(ex)}
{}

// ── Move operations ────────────────────────────────────────────────────────

async_connection::async_connection(async_connection&& other) noexcept
    : pg_conn_{std::exchange(other.pg_conn_, nullptr)}
    , conn_fd_{std::move(other.conn_fd_)}
    , executor_{std::move(other.executor_)}
{}

async_connection& async_connection::operator=(async_connection&& other) noexcept
{
    /*
     * Move-assigns another async_connection, cleaning up the current one first.
     *
     * Key types involved:
     *   - asio::posix::stream_descriptor: move-only; owns the fd unless released
     *
     * Preconditions:
     *   - No coroutine is suspended on conn_fd_ (caller must cancel first).
     *
     * Postconditions:
     *   - this->pg_conn_ == former other.pg_conn_; other.pg_conn_ == nullptr.
     *   - Old pg_conn_ (if any) is PQfinish'd after conn_fd_.release().
     *
     * Pitfalls:
     *   - Must call conn_fd_.release() BEFORE PQfinish. PQfinish closes the socket fd
     *     that conn_fd_ still holds. Closing while conn_fd_ owns it causes double-close (UB).
     *
     */
    if (this == &other) return *this;
    this->cleanup();
    this->pg_conn_      = std::exchange(other.pg_conn_, nullptr);
    this->conn_fd_      = std::move(other.conn_fd_);
    this->executor_     = std::move(other.executor_);
    return *this;
}

// ── Destructor ─────────────────────────────────────────────────────────────

async_connection::~async_connection()
{
    /*
     * Cleanly destroys the connection by releasing the Asio fd handle
     * before letting libpq close the socket.
     *
     * Preconditions:
     *   - No coroutine is currently suspended waiting on conn_fd_.
     *
     * Postconditions:
     *   - pg_conn_ resources are freed; socket fd is closed exactly once (by PQfinish).
     *
     * Pitfalls:
     *   - Skipping conn_fd_.release() before PQfinish causes double-close: Asio's
     *     stream_descriptor destructor calls close(fd) after PQfinish already did.
     *
     */
    this->cleanup();

}

void async_connection::cleanup() noexcept
{
    if (this->pg_conn_ != nullptr) 
    {
        //boost::system::error_code ignored_ec;
        //this->conn_fd_.release(ignored_ec);
        this->conn_fd_.release();
        PQfinish(this->pg_conn_);
        this->pg_conn_ = nullptr;
    }
}

// ── Static factory ─────────────────────────────────────────────────────────
pg_awaitable<async_connection> async_connection::connect(executor_type ex, std::string_view connstr)
{
    /*
     * Non-blockingly establishes a PostgreSQL connection by polling libpq
     * until PQconnectPoll returns PGRES_POLLING_OK or PGRES_POLLING_FAILED.
     *
     * Key types involved:
     *   - PostgresPollingStatusType: PQconnectPoll return type (libpq-fe.h)
     *   - asio::posix::stream_descriptor: wraps the socket fd for Asio I/O
     *   - PQsocket(conn): returns the current socket fd (may change during poll)
     *
     * Preconditions:
     *   - ex is bound to a running io_context.
     *   - connstr is a valid libpq connection string.
     *
     * Postconditions:
     *   - On success: PQstatus(conn) == CONNECTION_OK.
     *   - On failure: PGconn* is PQfinish'd; no resource leak.
     *
     * Pitfalls:
     *   - PQsocket() CAN change between poll steps on some SSL negotiation paths.
     *     Always re-read and re-assign sd after each async_wait.
     *   - string_view is not null-terminated; always copy to std::string first.
     *   - The sd constructed here is a temporary; the async_connection constructor
     *     creates its own sd from the fd.
     *
     * Hint:
     *   Use asio::posix::stream_descriptor::wait_read and wait_write type aliases.
     */
    std::string connstr_str(connstr);
    auto connection = PQconnectStart(connstr_str.c_str());
    if (!connection) {
        co_return  std::unexpected(pg::error{"PQconnectStart returned null", pg::errc::connection_failure});
    }
    if (PQstatus(connection) == CONNECTION_BAD) {
        std::string msg = PQerrorMessage(connection);
        PQfinish(connection);
        co_return std::unexpected(pg::error{std::move(msg), pg::errc::connection_failure});
    }
    asio::posix::stream_descriptor connection_fd{ex, PQsocket(connection)};
    co_return co_await [&]() -> pg_awaitable<async_connection> {
        while (true) {
            PostgresPollingStatusType status = PQconnectPoll(connection);
            switch (status) {
                case PGRES_POLLING_READING:
                    co_await connection_fd.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);
                    break;
                case PGRES_POLLING_WRITING:
                    co_await connection_fd.async_wait(asio::posix::stream_descriptor::wait_write, asio::use_awaitable);
                    break;
                case PGRES_POLLING_OK:
                    goto connected;
                case PGRES_POLLING_FAILED:
                default: {
                    std::string msg = PQerrorMessage(connection);
                    PQfinish(connection);
                    co_return std::unexpected(pg::error{std::move(msg), pg::errc::connection_failure});
                }
            }
            // Re-read socket; PQsocket may change after a poll step.
            connection_fd.assign(PQsocket(connection));
        }
    connected:
        // Release the temporary stream_descriptor before passing to the constructor.
        connection_fd.release();
        co_return async_connection{connection, ex};
    }();
}

// ── Non-blocking query send ────────────────────────────────────────────────

pg_expected<void> async_connection::send_query(std::string_view sql, std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Enqueues a parameterised SQL query on the non-blocking connection.
     *
     * Step 1 — If pg_conn_ == nullptr:
     *             return std::unexpected(pg::error{pg::errc::invalid_state,
     *                                             "send_query on moved-from connection"});
     * Step 2 — int rc = PQsendQueryParams(
     *                       pg_conn_,
     *                       sql.data(),
     *                       static_cast<int>(params.size()),
     *                       nullptr,       // paramTypes: let server infer
     *                       params.data(), // paramValues: null-terminated strings
     *                       nullptr,       // paramLengths: text mode, not needed
     *                       nullptr,       // paramFormats: text mode (0)
     *                       0);            // resultFormat: text
     * Step 3 — If rc == 0:
     *             return std::unexpected(pg::error{pg::errc::unknown,
     *                                             PQerrorMessage(pg_conn_)});
     * Step 4 — return {} (success).
     *
     * Key types involved:
     *   - PQsendQueryParams: libpq non-blocking parameterised query submission
     *   - std::span<const char* const>: caller-owned array of parameter C-strings
     *
     * Preconditions:
     *   - pg_conn_ != nullptr; PQstatus(pg_conn_) == CONNECTION_OK.
     *   - No other query is currently in flight on this connection.
     *
     * Postconditions:
     *   - On success: query is enqueued; call receive() to retrieve the result.
     *   - On failure: connection state may be degraded; check is_alive().
     *
     * Pitfalls:
     *   - sql.data() is used directly; it must be null-terminated.
     *     If string_view does not guarantee null termination, copy to std::string.
     *   - PQsendQueryParams does not flush; the io_context flushes asynchronously.
     *
     * Hint:
     *   Pass 0 for resultFormat to request text-format results (not binary).
     */
    if (pg_conn_ == nullptr) {
        return std::unexpected(pg::error{"send_query on moved-from connection", pg::errc::invalid_state});
    }
    int rc = PQsendQueryParams(
        pg_conn_,
        sql.data(),
        static_cast<int>(params.size()),
        nullptr,       // paramTypes: let server infer
        params.data(), // paramValues: null-terminated strings
        nullptr,       // paramLengths: text mode, not needed
        nullptr,       // paramFormats: text mode (0)
        0);            // resultFormat: text

    if (rc == 0) {
        return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::unknown});

    }
    return {}; // success
}

// ── Result receive loop ────────────────────────────────────────────────────

pg_awaitable<std::optional<pg::result>> async_connection::receive()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Waits for one result from the server; returns nullopt at end of stream.
     *
     * Step 1 — Outer loop (until a complete result or end-of-stream):
     *   a. co_await conn_fd_.async_wait(
     *          asio::posix::stream_descriptor::wait_read,
     *          asio::use_awaitable);
     *   b. if (PQconsumeInput(pg_conn_) == 0):
     *          co_return std::unexpected(pg::error{pg::errc::unknown,
     *                                             PQerrorMessage(pg_conn_)});
     *   c. while (!PQisBusy(pg_conn_)) {
     *          PGresult* raw = PQgetResult(pg_conn_);
     *          if (raw == nullptr) co_return std::nullopt;  // end of results
     *          co_return wrap_result(raw);
     *      }
     *      // PQisBusy == true: loop back to async_wait for more data.
     *
     * Key types involved:
     *   - PQconsumeInput: reads available data into libpq's internal buffer
     *   - PQisBusy: returns 1 if a complete result is not yet buffered
     *   - PQgetResult: dequeues the next result; returns nullptr at end of stream
     *
     * Preconditions:
     *   - send_query() returned success.
     *   - No concurrent receive() calls on this connection.
     *
     * Postconditions:
     *   - Returns one result or nullopt. Caller must loop until nullopt to fully
     *     drain the result stream.
     *
     * Pitfalls:
     *   - Must drain all results (including error results) until PQgetResult returns
     *     nullptr; otherwise the connection enters an undefined state for next queries.
     *   - Do NOT busy-spin; always co_await async_wait before PQconsumeInput.
     *
     * Hint:
     *   Outer loop structure:
     *     while (true) {
     *       co_await conn_fd_.async_wait(wait_read, use_awaitable);
     *       PQconsumeInput(pg_conn_);
     *       while (!PQisBusy(pg_conn_)) { auto* r = PQgetResult(pg_conn_); ... }
     *     }
     */
    while (true) {
        co_await conn_fd_.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);
        if (PQconsumeInput(pg_conn_) == 0) {
            co_return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::unknown});
        }
        while (!PQisBusy(pg_conn_)) {
        auto* raw = PQgetResult(pg_conn_);
        if (raw == nullptr) {
            co_return std::nullopt; // end of results   
        }
        co_return wrap_result(raw);
        }
    }
}

// ── Convenience execute ────────────────────────────────────────────────────

pg_awaitable<pg::result> async_connection::execute(std::string_view sql, std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Sends a query and collects all results, returning the last non-null one.
     *
     * Step 1 — auto sq = send_query(sql, params);
     *           if (!sq) co_return std::unexpected(sq.error());
     * Step 2 — std::optional<pg::result> last;
     * Step 3 — Loop:
     *             auto res = co_await receive();
     *             if (!res) co_return std::unexpected(res.error());
     *             if (!res->has_value()) break;          // nullopt = end of stream
     *             last = std::move(**res);
     * Step 4 — If !last:
     *             co_return std::unexpected(pg::error{pg::errc::unknown,
     *                                                 "no result returned"});
     * Step 5 — co_return std::move(*last).
     *
     * Preconditions:
     *   - pg_conn_ != nullptr; in a ready state.
     *
     * Postconditions:
     *   - Result stream fully consumed (receive() returned nullopt).
     *   - Returns the last non-null result (typically the only one for a single
     *     statement query).
     *
     * Pitfalls:
     *   - Multi-statement queries produce multiple non-null results; only the last
     *     is returned. Use send_query + manual receive() loop for multi-result queries.
     *
     * Hint:
     *   A single-statement PostgreSQL query always produces exactly two PQgetResult
     *   calls: one real PGresult and one nullptr sentinel.
     */
    auto query = send_query(sql, params);
    if (!query) {
        co_return std::unexpected(query.error());
    }
    std::optional<pg::result> last;
    while (true) {
        auto result = co_await receive();
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (!result->has_value()) {
            break;
        }
        last = std::move(**result);
    }
    if (!last) {
        co_return std::unexpected(pg::error{"no result returned", pg::errc::unknown});
    }
    co_return std::move(*last);
}

// ── Status helpers ─────────────────────────────────────────────────────────

bool async_connection::is_alive() const noexcept
{
    /*
     * Returns true if the underlying PGconn is in CONNECTION_OK state.
     *
     */
    return pg_conn_ != nullptr && PQstatus(pg_conn_) == CONNECTION_OK;
}

int async_connection::socket_fd() const noexcept
{
    /*
     * Returns the raw OS file descriptor of the server connection socket.
     */
    return pg_conn_ != nullptr ? PQsocket(pg_conn_) : -1;
}

int async_connection::backend_pid() const noexcept
{
    /*
     * Returns the server-side process ID of the PostgreSQL backend.
     *
     */
    return pg_conn_ != nullptr ? PQbackendPID(pg_conn_) : 0;
}

// ── Cancellation ───────────────────────────────────────────────────────────

pg_expected<void> async_connection::request_cancel() noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Requests server-side cancellation of the current query via a cancel handle.
     *
     * Step 1 — If pg_conn_ == nullptr:
     *             return std::unexpected(pg::error{pg::errc::invalid_state,
     *                                             "request_cancel on moved-from connection"});
     * Step 2 — PGcancel* cancel = PQgetCancel(pg_conn_).
     *           If cancel == nullptr:
     *             return std::unexpected(pg::error{pg::errc::unknown,
     *                                             "PQgetCancel returned null"});
     * Step 3 — char errbuf[256] = {};
     *           int rc = PQcancel(cancel, errbuf, sizeof(errbuf));
     * Step 4 — PQfreeCancel(cancel).
     * Step 5 — If rc == 0:
     *             return std::unexpected(pg::error{pg::errc::unknown, errbuf});
     * Step 6 — return {}.
     *
     * Key types involved:
     *   - PGcancel*: opaque cancel handle from PQgetCancel (thread-safe to use)
     *   - PQcancel: sends the cancel request over a separate connection
     *   - PQfreeCancel: always frees the handle regardless of success
     *
     * Preconditions:
     *   - pg_conn_ != nullptr.
     *   - A query is in flight for cancellation to have effect.
     *
     * Postconditions:
     *   - On success: server has received the cancel signal; next receive() will
     *     return pg_errc::query_canceled.
     *   - PGcancel handle is always freed.
     *
     * Pitfalls:
     *   - PQrequestCancel is deprecated; use PQgetCancel + PQcancel instead.
     *   - noexcept: must never throw. Use errbuf for error text.
     *   - PQcancel is signal-safe and thread-safe; PQgetCancel/PQfreeCancel are not.
     *
     * Hint:
     *   Use a scope guard or RAII wrapper to guarantee PQfreeCancel is called even
     *   when rc == 0 (the cancel itself failed).
     */
    if (this->pg_conn_ == nullptr) {
        return std::unexpected(pg::error{"request_cancel on moved-from connection", pg::errc::invalid_state});
    }
    PGcancel* cancel = PQgetCancel(this->pg_conn_);
    if (cancel == nullptr) {
        return std::unexpected(pg::error{"PQgetCancel returned null", pg::errc::unknown});
    }
    char errbuf[256] = {};
    int rc = PQcancel(cancel, errbuf, sizeof(errbuf));
    PQfreeCancel(cancel);
    if (rc == 0) {
        return std::unexpected(pg::error{errbuf, pg::errc::unknown});
    }
    return {};
}

// ── Internal result wrapping ───────────────────────────────────────────────

pg_expected<pg::result> async_connection::wrap_result(PGresult* raw)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Converts a raw PGresult* into expected<result, error>, taking ownership
     *   of the pointer and handling all ExecStatusType codes.
     *
     * Step 1 — If raw == nullptr:
     *             return std::unexpected(pg::error{pg::errc::unknown, "null result"});
     * Step 2 — ExecStatusType status = PQresultStatus(raw).
     * Step 3 — If status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK:
     *             Construct pg::result using the detail::result_handle_adopter pattern
     *             (or whatever internal construction mechanism Phase 1 exposes).
     *             Return the result wrapped in expected.
     * Step 4 — Otherwise (error status):
     *             const char* sqlstate = PQresultErrorField(raw, PG_DIAG_SQLSTATE);
     *             std::string msg      = PQresultErrorMessage(raw);
     *             PQclear(raw);
     *             pg::errc code = sqlstate
     *                 ? pg::sqlstate_to_errc(sqlstate)
     *                 : pg::errc::unknown;
     *             return std::unexpected(pg::error{code, std::move(msg), sqlstate ? sqlstate : ""});
     *
     * Key types involved:
     *   - ExecStatusType: PGRES_COMMAND_OK, PGRES_TUPLES_OK, PGRES_FATAL_ERROR, etc.
     *   - pg::sqlstate_to_errc: maps SQLSTATE string to pg::errc (atlas/pg/error.hpp)
     *   - pg::detail::result_handle_adopter: internal friend for result construction
     *
     * Preconditions:
     *   - Caller does NOT call PQclear(raw); this function takes ownership.
     *
     * Postconditions:
     *   - raw has been consumed: wrapped in result on success, PQclear'd on error.
     *
     * Pitfalls:
     *   - PGRES_EMPTY_QUERY means no query was sent (not necessarily a fatal error).
     *   - PGRES_NONFATAL_ERROR is a warning; treat as success or log and skip.
     *   - Must PQclear(raw) in all error paths to avoid memory leaks.
     *
     * Hint:
     *   See src/pg/connection.cpp make_result_error() for the Phase 1 pattern.
     *   pg::detail::result_handle_adopter is declared as a friend of pg::result.
     */
    if (raw == nullptr) {
        return std::unexpected(pg::error{"null result", pg::errc::unknown});
    }
    ExecStatusType status = PQresultStatus(raw);
    pg::detail::result_handle result_handle{raw};
    if (is_success_status(status)) {
        return pg::detail::result_handle_adopter::make(std::move(result_handle));
    }

    const char* sqlstate = PQresultErrorField(result_handle.get(), PG_DIAG_SQLSTATE);
    std::string msg = PQresultErrorMessage(result_handle.get());
    pg::errc code = sqlstate ? pg::sqlstate_to_errc(sqlstate) : pg::errc::unknown;
    return std::unexpected(pg::error{std::move(msg), sqlstate ? std::string(sqlstate) : "", code});
}

} // namespace atlas
