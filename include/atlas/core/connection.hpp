#pragma once

#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <libpq-fe.h>

namespace atlas {

class connection {
public:
    ~connection() noexcept {
        if (conn_) PQfinish(conn_);
    }

    connection(const connection&)            = delete;
    connection& operator=(const connection&) = delete;

    connection(connection&& other) noexcept
        : conn_(std::exchange(other.conn_, nullptr)) {}

    connection& operator=(connection&& other) noexcept {
        if (this != &other) {
            if (conn_) PQfinish(conn_);
            conn_ = std::exchange(other.conn_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] static auto connect(std::string_view connstr)
        -> std::expected<connection, pg_error>
    {
        PGconn* raw = PQconnectdb(connstr.data());
        if (!raw || PQstatus(raw) != CONNECTION_OK) {
            pg_error err{
                .message  = raw ? PQerrorMessage(raw) : "allocation failure",
                .sqlstate = {},
                .code     = pg_errc::connection_failure,
            };
            if (raw) PQfinish(raw);
            return std::unexpected(std::move(err));
        }
        return connection{raw};
    }

    [[nodiscard]] auto exec(std::string_view sql)
        -> std::expected<result, pg_error>
    {
        return wrap_result(PQexec(conn_, sql.data()));
    }

    [[nodiscard]] auto exec_params(
        std::string_view             sql,
        std::span<const char* const> params)
        -> std::expected<result, pg_error>
    {
        PGresult* raw = PQexecParams(
            conn_,
            sql.data(),
            static_cast<int>(params.size()),
            nullptr,   // paramTypes  — infer from context
            params.data(),
            nullptr,   // paramLengths — text mode: unused
            nullptr,   // paramFormats — text mode: unused
            0          // resultFormat — 0 = text
        );
        return wrap_result(raw);
    }

    [[nodiscard]] auto is_alive() const noexcept -> bool {
        return conn_ && PQstatus(conn_) == CONNECTION_OK;
    }

    [[nodiscard]] auto reset() -> std::expected<void, pg_error> {
        PQreset(conn_);
        if (PQstatus(conn_) != CONNECTION_OK) {
            return std::unexpected(pg_error{
                .message  = PQerrorMessage(conn_),
                .sqlstate = {},
                .code     = pg_errc::connection_failure,
            });
        }
        return {};
    }

    [[nodiscard]] auto backend_pid() const noexcept -> int {
        return PQbackendPID(conn_);
    }

    [[nodiscard]] auto socket_fd() const noexcept -> int {
        return PQsocket(conn_);
    }

private:
    explicit connection(PGconn* conn) noexcept : conn_(conn) {}

    [[nodiscard]] auto wrap_result(PGresult* raw)
        -> std::expected<result, pg_error>
    {
        if (!raw) {
            return std::unexpected(pg_error{
                .message  = PQerrorMessage(conn_),
                .sqlstate = {},
                .code     = pg_errc::unknown,
            });
        }

        const ExecStatusType st = PQresultStatus(raw);
        if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK) {
            return result{raw};
        }

        std::string sqlstate;
        if (const char* s = PQresultErrorField(raw, PG_DIAG_SQLSTATE); s)
            sqlstate = s;

        pg_error err{
            .message  = PQresultErrorMessage(raw),
            .sqlstate = sqlstate,
            .code     = sqlstate_to_errc(sqlstate),
        };
        PQclear(raw);
        return std::unexpected(std::move(err));
    }

    PGconn* conn_;
};

} // namespace atlas
