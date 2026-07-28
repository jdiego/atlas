#include "atlas/async/async_connection.hpp"
#include "async/detail/connect_poll_state.hpp"
#include "async/detail/descriptor_observer.hpp"
#include "async/detail/result_status.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"
#include "pg/detail/result_handle_adopter.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <libpq-fe.h>

#include <memory>
#include <string>
#include <utility>

namespace atlas {

namespace {

struct cancel_connection_deleter {
    void operator()(PGcancelConn *cancel) const noexcept {
        if (cancel != nullptr) {
            PQcancelFinish(cancel);
        }
    }
};

using cancel_connection_handle = std::unique_ptr<PGcancelConn, cancel_connection_deleter>;

// Drives the PQconnectPoll handshake to completion, keeping `fd` in sync with
// PQsocket(): libpq can swap the socket underneath us during SSL/GSS
// negotiation, including replacing it with a new socket using the same number.
[[nodiscard]] pg_awaitable<void> poll_until_connected(PGconn *conn, detail::descriptor_observer &fd) {
    using descriptor = asio::posix::stream_descriptor;

    auto status = detail::initial_connect_poll_status;
    for (;;) {
        const auto action = detail::connect_poll_action_for(status);

        if (action == detail::connect_poll_action::connected) {
            co_return pg_expected<void>{};
        }
        if (action == detail::connect_poll_action::failed) {
            co_return std::unexpected(pg::error{PQerrorMessage(conn), pg::errc::connection_failure});
        }
        if (action != detail::connect_poll_action::poll_again) {
            const int current_fd = PQsocket(conn);
            if (current_fd < 0) {
                co_return std::unexpected(
                    pg::error{"connection socket closed during handshake", pg::errc::connection_failure});
            }
            if (auto assign_error = fd.mirror(current_fd); assign_error) {
                co_return std::unexpected(pg::error{assign_error.message(), pg::errc::connection_failure});
            }

            const auto wait_type =
                (action == detail::connect_poll_action::wait_read) ? descriptor::wait_read : descriptor::wait_write;

            auto ec = co_await fd.wait(wait_type);
            if (ec) {
                co_return std::unexpected(pg::error{ec.message(), pg::errc::connection_failure});
            }
        }

        status = PQconnectPoll(conn);
    }
}

} // namespace

// ── Private constructor ────────────────────────────────────────────────────

async_connection::async_connection(PGconn *raw, executor_type ex, std::chrono::milliseconds cleanup_budget)
    : pg_conn_{raw}, conn_fd_{ex, PQsocket(raw)}, executor_{std::move(ex)}, cleanup_budget_{cleanup_budget} {
}

// ── Move operations ────────────────────────────────────────────────────────

async_connection::async_connection(async_connection &&other) noexcept
    : pg_conn_{std::exchange(other.pg_conn_, nullptr)}, conn_fd_{std::move(other.conn_fd_)},
      executor_{std::move(other.executor_)}, cleanup_budget_{other.cleanup_budget_},
      query_in_progress_{std::exchange(other.query_in_progress_, false)} {
}

async_connection &async_connection::operator=(async_connection &&other) noexcept {
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
    if (this == &other)
        return *this;
    this->cleanup();
    this->pg_conn_ = std::exchange(other.pg_conn_, nullptr);
    this->conn_fd_ = std::move(other.conn_fd_);
    this->executor_ = std::move(other.executor_);
    this->cleanup_budget_ = other.cleanup_budget_;
    this->query_in_progress_ = std::exchange(other.query_in_progress_, false);
    return *this;
}

// ── Destructor ─────────────────────────────────────────────────────────────

async_connection::~async_connection() {
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

void async_connection::cleanup() noexcept {
    if (this->pg_conn_ != nullptr) {
        // Detach before PQfinish: both close the same fd, and letting the
        // descriptor close it after libpq did would hit an unrelated fd.
        detail::detach_descriptor(this->conn_fd_);
        PQfinish(this->pg_conn_);
        this->pg_conn_ = nullptr;
    }
    this->query_in_progress_ = false;
}

// ── Static factory ─────────────────────────────────────────────────────────
// Non-blockingly establishes a connection: PQconnectStart followed by
// PQconnectPoll driven off the socket until it reports OK or FAILED.
// On every exit path libpq owns the socket and the descriptor has let go of it.
pg_awaitable<async_connection> async_connection::connect(executor_type ex, std::string_view connstr,
                                                         std::chrono::milliseconds cleanup_budget) {
    // string_view carries no NUL-terminator guarantee; libpq needs a C string.
    const std::string connstr_str{connstr};

    PGconn *connection = PQconnectStart(connstr_str.c_str());
    if (connection == nullptr) {
        co_return std::unexpected(pg::error{"PQconnectStart returned null", pg::errc::connection_failure});
    }
    if (PQstatus(connection) == CONNECTION_BAD) {
        std::string msg = PQerrorMessage(connection);
        PQfinish(connection);
        co_return std::unexpected(pg::error{std::move(msg), pg::errc::connection_failure});
    }

    const int initial_fd = PQsocket(connection);
    if (initial_fd < 0) {
        std::string msg = PQerrorMessage(connection);
        PQfinish(connection);
        co_return std::unexpected(pg::error{std::move(msg), pg::errc::connection_failure});
    }

    detail::descriptor_observer connection_fd{ex};

    pg_expected<void> polled;
    try {
        polled = co_await poll_until_connected(connection, connection_fd);
    } catch (...) {
        // Cancellation unwinds through here; never let the descriptor close a
        // socket that libpq is about to close itself.
        connection_fd.detach();
        PQfinish(connection);
        throw;
    }

    connection_fd.detach();

    if (!polled) {
        PQfinish(connection);
        co_return std::unexpected(polled.error());
    }
    if (PQsetnonblocking(connection, 1) != 0) {
        std::string msg = PQerrorMessage(connection);
        PQfinish(connection);
        co_return std::unexpected(pg::error{std::move(msg), pg::errc::connection_failure});
    }

    co_return async_connection{connection, ex, cleanup_budget};
}

// ── Non-blocking query send ────────────────────────────────────────────────

// Enqueues a parameterised query. The query may still be sitting in libpq's
// output buffer when this returns; receive() flushes before it waits for input.
pg_expected<void> async_connection::send_query(std::string_view sql, std::span<const char *const> params) {
    if (pg_conn_ == nullptr) {
        return std::unexpected(pg::error{"send_query on moved-from connection", pg::errc::invalid_state});
    }
    if (PQstatus(pg_conn_) != CONNECTION_OK) {
        return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::connection_failure});
    }
    if (query_in_progress_) {
        return std::unexpected(pg::error{"another command is already in progress", pg::errc::invalid_state});
    }

    // string_view carries no NUL-terminator guarantee; passing sql.data()
    // straight to libpq would read past the end of the view.
    const std::string sql_str{sql};

    const int rc = PQsendQueryParams(pg_conn_, sql_str.c_str(), static_cast<int>(params.size()),
                                     nullptr,       // paramTypes: let server infer
                                     params.data(), // paramValues: null-terminated strings
                                     nullptr,       // paramLengths: text mode, not needed
                                     nullptr,       // paramFormats: text mode (0)
                                     0);            // resultFormat: text

    if (rc == 0) {
        if (PQstatus(pg_conn_) != CONNECTION_OK) {
            return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::connection_failure});
        }
        if (PQisBusy(pg_conn_) != 0) {
            return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::invalid_state});
        }
        return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::unknown});
    }
    query_in_progress_ = true;
    return {}; // success
}

// ── Output flush ───────────────────────────────────────────────────────────

// Drives libpq's output buffer empty. On a non-blocking connection
// PQsendQueryParams can return before the whole query reaches the socket; a
// query left half-sent would never produce a reply to wait for.
pg_awaitable<void> async_connection::flush() {
    for (;;) {
        const int rc = PQflush(pg_conn_);
        if (rc == 0) {
            co_return pg_expected<void>{};
        }
        if (rc < 0) {
            co_return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::connection_failure});
        }

        auto ready = co_await detail::wait_read_or_write(conn_fd_);
        if (!ready) {
            co_return std::unexpected(pg::error{ready.error().message(), pg::errc::connection_failure});
        }
        if (*ready == detail::descriptor_readiness::read && PQconsumeInput(pg_conn_) == 0) {
            co_return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::connection_failure});
        }
    }
}

// ── Result receive loop ────────────────────────────────────────────────────

// Returns the next result, or nullopt once the stream is exhausted. Callers
// must keep calling until nullopt: leaving the terminating sentinel unread
// leaves the connection unusable for the next query.
pg_awaitable<std::optional<pg::result>> async_connection::receive() {
    if (pg_conn_ == nullptr) {
        co_return std::unexpected(pg::error{"receive on moved-from connection", pg::errc::invalid_state});
    }

    if (auto flushed = co_await flush(); !flushed) {
        co_return std::unexpected(flushed.error());
    }

    for (;;) {
        // Consume what libpq has already buffered before touching the socket.
        // A result and its terminating sentinel usually arrive in the same read,
        // so waiting for readability first would block until the *next* query
        // produced traffic — that is, forever.
        if (PQisBusy(pg_conn_) == 0) {
            PGresult *raw = PQgetResult(pg_conn_);
            if (raw == nullptr) {
                query_in_progress_ = false;
                co_return std::optional<pg::result>{}; // end of results
            }

            auto wrapped = wrap_result(raw);
            if (!wrapped) {
                co_return std::unexpected(wrapped.error());
            }
            co_return std::optional<pg::result>{std::move(*wrapped)};
        }

        auto [ec] = co_await conn_fd_.async_wait(asio::posix::stream_descriptor::wait_read,
                                                 asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return std::unexpected(pg::error{ec.message(), pg::errc::connection_failure});
        }
        if (PQconsumeInput(pg_conn_) == 0) {
            co_return std::unexpected(pg::error{PQerrorMessage(pg_conn_), pg::errc::connection_failure});
        }
    }
}

// ── Convenience execute ────────────────────────────────────────────────────

// Sends a query and consumes the whole result stream, returning the last
// non-null result. Multi-statement queries produce several results; use
// send_query + receive() directly if every one of them matters.
pg_awaitable<pg::result> async_connection::execute(std::string_view sql, std::span<const char *const> params) {
    auto query = send_query(sql, params);
    if (!query) {
        co_return std::unexpected(query.error());
    }

    std::optional<pg::result> last;
    std::optional<pg::error> failure;

    for (;;) {
        auto result = co_await receive();
        if (!result) {
            if (failure) {
                // Two failures in a row: the stream is not going to terminate
                // cleanly, so stop rather than spin.
                break;
            }
            // A server-side error still leaves the terminating sentinel unread.
            // Keep draining so the connection is reusable, then report the
            // original error.
            failure = result.error();
            continue;
        }
        if (!result->has_value()) {
            break; // end of stream
        }
        last = std::move(**result);
    }

    if (failure) {
        co_return std::unexpected(std::move(*failure));
    }
    if (!last) {
        co_return std::unexpected(pg::error{"no result returned", pg::errc::unknown});
    }
    co_return std::move(*last);
}

// ── Status helpers ─────────────────────────────────────────────────────────

bool async_connection::is_alive() const noexcept {
    /*
     * Returns true if the underlying PGconn is in CONNECTION_OK state.
     *
     */
    return pg_conn_ != nullptr && PQstatus(pg_conn_) == CONNECTION_OK;
}

bool async_connection::is_nonblocking() const noexcept {
    return pg_conn_ != nullptr && PQisnonblocking(pg_conn_) != 0;
}

bool async_connection::transaction_aborted() const noexcept {
    return pg_conn_ != nullptr && PQtransactionStatus(pg_conn_) == PQTRANS_INERROR;
}

int async_connection::socket_fd() const noexcept {
    /*
     * Returns the raw OS file descriptor of the server connection socket.
     */
    return pg_conn_ != nullptr ? PQsocket(pg_conn_) : -1;
}

int async_connection::backend_pid() const noexcept {
    /*
     * Returns the server-side process ID of the PostgreSQL backend.
     *
     */
    return pg_conn_ != nullptr ? PQbackendPID(pg_conn_) : 0;
}

// ── Cancellation ───────────────────────────────────────────────────────────

pg_awaitable<void> async_connection::request_cancel() {
    if (pg_conn_ == nullptr) {
        co_return std::unexpected(pg::error{"request_cancel on moved-from connection", pg::errc::invalid_state});
    }

    cancel_connection_handle cancel{PQcancelCreate(pg_conn_)};
    if (!cancel) {
        co_return std::unexpected(pg::error{"PQcancelCreate returned null", pg::errc::unknown});
    }
    if (PQcancelStart(cancel.get()) == 0) {
        co_return std::unexpected(pg::error{PQcancelErrorMessage(cancel.get()), pg::errc::connection_failure});
    }

    detail::descriptor_observer cancel_socket{executor_};
    auto wait_type = asio::posix::stream_descriptor::wait_write;

    for (;;) {
        const int socket = PQcancelSocket(cancel.get());
        if (socket < 0) {
            co_return std::unexpected(pg::error{PQcancelErrorMessage(cancel.get()), pg::errc::connection_failure});
        }

        if (auto assign_error = cancel_socket.mirror(socket); assign_error) {
            co_return std::unexpected(pg::error{assign_error.message(), pg::errc::connection_failure});
        }

        auto wait_error = co_await cancel_socket.wait(wait_type);
        if (wait_error) {
            co_return std::unexpected(pg::error{wait_error.message(), pg::errc::connection_failure});
        }

        const auto status = PQcancelPoll(cancel.get());
        if (status == PGRES_POLLING_OK) {
            co_return pg_expected<void>{};
        }
        if (status == PGRES_POLLING_FAILED) {
            co_return std::unexpected(pg::error{PQcancelErrorMessage(cancel.get()), pg::errc::connection_failure});
        }
        if (status == PGRES_POLLING_READING) {
            wait_type = asio::posix::stream_descriptor::wait_read;
        } else if (status == PGRES_POLLING_WRITING) {
            wait_type = asio::posix::stream_descriptor::wait_write;
        }
    }
}

void async_connection::invalidate() noexcept {
    cleanup();
}

std::chrono::milliseconds async_connection::cleanup_budget() const noexcept {
    return cleanup_budget_;
}

// ── Internal result wrapping ───────────────────────────────────────────────

pg_expected<pg::result> async_connection::wrap_result(PGresult *raw) {
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
    const auto disposition = detail::classify_async_result(status);
    if (disposition == detail::async_result_disposition::success) {
        return pg::detail::result_handle_adopter::make(std::move(result_handle));
    }
    if (disposition == detail::async_result_disposition::unsupported_copy) {
        result_handle.reset();
        cleanup();
        return std::unexpected(
            pg::error{"COPY protocol is not supported by async_connection", pg::errc::invalid_state});
    }

    const char *sqlstate = PQresultErrorField(result_handle.get(), PG_DIAG_SQLSTATE);
    std::string msg = PQresultErrorMessage(result_handle.get());
    pg::errc code = sqlstate ? pg::sqlstate_to_errc(sqlstate) : pg::errc::unknown;
    return std::unexpected(pg::error{std::move(msg), sqlstate ? std::string(sqlstate) : "", code});
}

} // namespace atlas
