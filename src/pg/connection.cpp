#include "atlas/pg/connection.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace atlas::pg {

namespace {

struct prepared_params {
    std::vector<text_param> views {};
    std::vector<std::string> owned_values {};
    std::vector<const char*> raw_values {};
    int count {0};
};

[[nodiscard]] auto trim_message(std::string message) -> std::string {
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

[[nodiscard]] auto make_error(
    std::string message,
    errc code = errc::unknown,
    std::string sqlstate = {}) -> error {
    return error {
        .message = trim_message(std::move(message)),
        .sqlstate = std::move(sqlstate),
        .code = code,
    };
}

[[nodiscard]] auto make_handle_error(std::string_view label) -> error {
    return make_error(std::string(label) + " handle is empty", errc::invalid_state);
}

[[nodiscard]] auto make_connection_error(
    PGconn* handle,
    std::string_view fallback = "libpq connection failure") -> error {
    if (handle == nullptr) {
        return make_error(std::string(fallback), errc::connection_failure);
    }

    const auto* message = PQerrorMessage(handle);
    if (message == nullptr || message[0] == '\0') {
        return make_error(std::string(fallback), errc::connection_failure);
    }

    return make_error(std::string {message}, errc::connection_failure);
}

[[nodiscard]] auto make_result_error(PGresult* handle) -> error {
    std::string sqlstate {};
    if (handle != nullptr) {
        if (const auto* field = PQresultErrorField(handle, PG_DIAG_SQLSTATE); field != nullptr) {
            sqlstate = field;
        }
    }

    const auto code = sqlstate.empty() ? errc::unknown : sqlstate_to_errc(sqlstate);
    const auto* message = handle != nullptr ? PQresultErrorMessage(handle) : nullptr;
    if (message == nullptr || message[0] == '\0') {
        return make_error("libpq command failed", code, std::move(sqlstate));
    }

    return make_error(std::string {message}, code, std::move(sqlstate));
}

[[nodiscard]] auto copy_c_string_argument(std::string_view value, std::string_view label)
    -> std::expected<std::string, error> {
    if (value.find('\0') != std::string_view::npos) {
        return std::unexpected(make_error(
            std::string(label) + " contains an embedded NUL byte",
            errc::invalid_argument));
    }

    return std::string {value};
}

[[nodiscard]] auto checked_count(std::size_t size, std::string_view label)
    -> std::expected<int, error> {
    constexpr auto max_int = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (size > max_int) {
        return std::unexpected(make_error(
            std::string(label) + " exceeds libpq integer limits",
            errc::invalid_argument));
    }

    return static_cast<int>(size);
}

[[nodiscard]] auto prepare_params(std::span<const text_param> params)
    -> std::expected<prepared_params, error> {
    const auto count = checked_count(params.size(), "parameter count");
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }

    prepared_params prepared {};
    prepared.count = *count;
    prepared.views.reserve(params.size());
    prepared.owned_values.reserve(params.size());
    prepared.raw_values.reserve(params.size());

    for (const auto& param : params) {
        prepared.views.push_back(param);
        if (param.has_value()) {
            const auto copied = copy_c_string_argument(*param, "query parameter");
            if (!copied) {
                return std::unexpected(std::move(copied.error()));
            }
            prepared.owned_values.push_back(std::move(*copied));
        }
    }

    auto owned_index = std::size_t {0};
    for (const auto& param : prepared.views) {
        if (!param.has_value()) {
            prepared.raw_values.push_back(nullptr);
            continue;
        }

        prepared.raw_values.push_back(prepared.owned_values[owned_index].c_str());
        ++owned_index;
    }

    return prepared;
}

[[nodiscard]] auto is_success_status(ExecStatusType status) noexcept -> bool {
    switch (status) {
        case PGRES_EMPTY_QUERY:
        case PGRES_COMMAND_OK:
        case PGRES_TUPLES_OK:
        case PGRES_COPY_OUT:
        case PGRES_COPY_IN:
        case PGRES_COPY_BOTH:
        case PGRES_SINGLE_TUPLE:
            return true;
        default:
            break;
    }

#ifdef PGRES_TUPLES_CHUNK
    if (status == PGRES_TUPLES_CHUNK) {
        return true;
    }
#endif

#ifdef PGRES_PIPELINE_SYNC
    if (status == PGRES_PIPELINE_SYNC) {
        return true;
    }
#endif

#ifdef PGRES_PIPELINE_ABORTED
    if (status == PGRES_PIPELINE_ABORTED) {
        return true;
    }
#endif

    return false;
}

} // namespace

void detail::connection_deleter::operator()(PGconn* handle) const noexcept {
    if (handle != nullptr) {
        PQfinish(handle);
    }
}

connection::connection(handle_type handle) noexcept
    : handle_(std::move(handle)) {}

auto connection::ensure_handle() const -> std::expected<PGconn*, error> {
    if (handle_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    return handle_.get();
}

auto connection::ensure_open() const -> std::expected<PGconn*, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    if (PQstatus(*handle) != CONNECTION_OK) {
        return std::unexpected(make_connection_error(*handle));
    }

    return *handle;
}

auto connection::ensure_nonblocking() -> std::expected<void, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    if (PQisnonblocking(*handle) != 0) {
        return {};
    }

    if (PQsetnonblocking(*handle, 1) != 0) {
        return std::unexpected(make_connection_error(*handle, "failed to enable nonblocking mode"));
    }

    return {};
}

auto connection::connect(std::string_view conninfo) -> std::expected<connection, error> {
    const auto copied_conninfo = copy_c_string_argument(conninfo, "connection string");
    if (!copied_conninfo) {
        return std::unexpected(std::move(copied_conninfo.error()));
    }

    handle_type handle {PQconnectdb(copied_conninfo->c_str())};
    if (handle == nullptr) {
        return std::unexpected(make_error(
            "PQconnectdb returned a null connection handle",
            errc::connection_failure));
    }

    if (PQstatus(handle.get()) != CONNECTION_OK) {
        return std::unexpected(make_connection_error(handle.get()));
    }

    connection conn {std::move(handle)};
    const auto nonblocking = conn.ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    return conn;
}

auto connection::connect_start(std::string_view conninfo) -> std::expected<connection, error> {
    const auto copied_conninfo = copy_c_string_argument(conninfo, "connection string");
    if (!copied_conninfo) {
        return std::unexpected(std::move(copied_conninfo.error()));
    }

    handle_type handle {PQconnectStart(copied_conninfo->c_str())};
    if (handle == nullptr) {
        return std::unexpected(make_error(
            "PQconnectStart returned a null connection handle",
            errc::connection_failure));
    }

    if (PQstatus(handle.get()) == CONNECTION_BAD) {
        return std::unexpected(make_connection_error(handle.get()));
    }

    connection conn {std::move(handle)};
    if (conn.status() == CONNECTION_OK) {
        const auto nonblocking = conn.ensure_nonblocking();
        if (!nonblocking) {
            return std::unexpected(std::move(nonblocking.error()));
        }
    }

    return conn;
}

auto connection::poll_connect() -> std::expected<poll_state, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    switch (PQconnectPoll(*handle)) {
        case PGRES_POLLING_READING:
            return poll_state::reading;
        case PGRES_POLLING_WRITING:
            return poll_state::writing;
        case PGRES_POLLING_ACTIVE:
            return poll_state::active;
        case PGRES_POLLING_OK: {
            const auto nonblocking = ensure_nonblocking();
            if (!nonblocking) {
                return std::unexpected(std::move(nonblocking.error()));
            }
            return poll_state::ready;
        }
        case PGRES_POLLING_FAILED:
            return std::unexpected(make_connection_error(*handle));
    }

    return std::unexpected(make_connection_error(*handle, "libpq returned an unknown poll state"));
}

auto connection::exec(std::string_view sql) -> std::expected<result, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    return wrap_result(PQexec(*handle, copied_sql->c_str()));
}

auto connection::exec_params(std::string_view sql, std::span<const text_param> params)
    -> std::expected<result, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    const auto prepared = prepare_params(params);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }

    const auto* values = prepared->raw_values.empty() ? nullptr : prepared->raw_values.data();
    return wrap_result(PQexecParams(
        *handle,
        copied_sql->c_str(),
        prepared->count,
        nullptr,
        values,
        nullptr,
        nullptr,
        0));
}

auto connection::send_query(std::string_view sql) -> std::expected<void, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    const auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    if (PQsendQuery(*handle, copied_sql->c_str()) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to dispatch query"));
    }

    return {};
}

auto connection::send_query_params(std::string_view sql, std::span<const text_param> params)
    -> std::expected<void, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    const auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    const auto prepared = prepare_params(params);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }

    const auto* values = prepared->raw_values.empty() ? nullptr : prepared->raw_values.data();
    if (PQsendQueryParams(
            *handle,
            copied_sql->c_str(),
            prepared->count,
            nullptr,
            values,
            nullptr,
            nullptr,
            0) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to dispatch parameterized query"));
    }

    return {};
}

auto connection::flush() -> std::expected<bool, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    const auto status_value = PQflush(*handle);
    if (status_value == 0) {
        return false;
    }
    if (status_value == 1) {
        return true;
    }

    return std::unexpected(make_connection_error(*handle, "failed to flush libpq output"));
}

auto connection::consume_input() -> std::expected<void, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    if (PQconsumeInput(*handle) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to consume libpq input"));
    }

    return {};
}

auto connection::next_result() -> std::expected<std::optional<result>, error> {
    const auto handle = ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    PGresult* raw = PQgetResult(*handle);
    if (raw == nullptr) {
        return std::optional<result> {};
    }

    auto wrapped = wrap_result(raw);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }

    return std::optional<result> {std::move(*wrapped)};
}

auto connection::reset() -> std::expected<void, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    PQreset(*handle);
    if (PQstatus(*handle) != CONNECTION_OK) {
        return std::unexpected(make_connection_error(*handle));
    }

    const auto nonblocking = ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    return {};
}

auto connection::reset_start() -> std::expected<void, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    if (PQresetStart(*handle) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to start connection reset"));
    }

    return {};
}

auto connection::poll_reset() -> std::expected<poll_state, error> {
    const auto handle = ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    switch (PQresetPoll(*handle)) {
        case PGRES_POLLING_READING:
            return poll_state::reading;
        case PGRES_POLLING_WRITING:
            return poll_state::writing;
        case PGRES_POLLING_ACTIVE:
            return poll_state::active;
        case PGRES_POLLING_OK: {
            const auto nonblocking = ensure_nonblocking();
            if (!nonblocking) {
                return std::unexpected(std::move(nonblocking.error()));
            }
            return poll_state::ready;
        }
        case PGRES_POLLING_FAILED:
            return std::unexpected(make_connection_error(*handle));
    }

    return std::unexpected(make_connection_error(*handle, "libpq returned an unknown reset state"));
}

auto connection::is_busy() const noexcept -> bool {
    if (handle_ == nullptr || PQstatus(handle_.get()) != CONNECTION_OK) {
        return false;
    }

    return PQisBusy(handle_.get()) != 0;
}

auto connection::is_alive() const noexcept -> bool {
    return handle_ != nullptr && PQstatus(handle_.get()) == CONNECTION_OK;
}

auto connection::status() const noexcept -> ConnStatusType {
    if (handle_ == nullptr) {
        return CONNECTION_BAD;
    }

    return PQstatus(handle_.get());
}

auto connection::backend_pid() const noexcept -> int {
    if (handle_ == nullptr) {
        return 0;
    }

    return PQbackendPID(handle_.get());
}

auto connection::socket_fd() const noexcept -> int {
    if (handle_ == nullptr) {
        return -1;
    }

    return PQsocket(handle_.get());
}

auto connection::wrap_result(PGresult* raw) -> std::expected<result, error> {
    if (raw == nullptr) {
        return std::unexpected(make_connection_error(
            handle_.get(),
            "libpq returned a null result handle"));
    }

    if (is_success_status(PQresultStatus(raw))) {
        return result {result::handle_type {raw}};
    }

    auto err = make_result_error(raw);
    PQclear(raw);
    return std::unexpected(std::move(err));
}

} // namespace atlas::pg
