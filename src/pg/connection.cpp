#include "atlas/pg/connection.hpp"
#include "detail/result_access.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace atlas::pg {

namespace {

struct prepared_params {
    std::vector<text_param> views;
    std::vector<std::string> owned_values;
    std::vector<const char *> raw_values;
    int count{0};
};

[[nodiscard]] auto trim_message(std::string message) -> std::string {
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

[[nodiscard]] auto make_error(std::string message, errc code = errc::unknown, std::string sqlstate = {}) -> error {
    return error{
        .message = trim_message(std::move(message)),
        .sqlstate = std::move(sqlstate),
        .code = code,
    };
}

[[nodiscard]] auto make_handle_error(std::string_view label) -> error {
    return make_error(std::string(label) + " handle is empty", errc::invalid_state);
}

[[nodiscard]] auto make_connection_error(PGconn *handle, std::string_view fallback = "libpq connection failure")
    -> error {
    if (handle == nullptr) {
        return make_error(std::string(fallback), errc::connection_failure);
    }

    const auto *message = PQerrorMessage(handle);
    if (message == nullptr || message[0] == '\0') {
        return make_error(std::string(fallback), errc::connection_failure);
    }

    return make_error(std::string{message}, errc::connection_failure);
}

[[nodiscard]] auto make_result_error(PGresult *handle) -> error {
    std::string sqlstate{};
    if (handle != nullptr) {
        if (const auto *field = PQresultErrorField(handle, PG_DIAG_SQLSTATE); field != nullptr) {
            sqlstate = field;
        }
    }

    const auto code = sqlstate.empty() ? errc::unknown : sqlstate_to_errc(sqlstate);
    const auto *message = handle != nullptr ? PQresultErrorMessage(handle) : nullptr;
    if (message == nullptr || message[0] == '\0') {
        return make_error("libpq command failed", code, std::move(sqlstate));
    }

    return make_error(std::string{message}, code, std::move(sqlstate));
}

[[nodiscard]] auto copy_c_string_argument(std::string_view value, std::string_view label)
    -> std::expected<std::string, error> {
    if (value.find('\0') != std::string_view::npos) {
        return std::unexpected(
            make_error(std::string(label) + " contains an embedded NUL byte", errc::invalid_argument));
    }

    return std::string{value};
}

[[nodiscard]] auto checked_count(std::size_t size, std::string_view label) -> std::expected<int, error> {
    constexpr auto max_int = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (size > max_int) {
        return std::unexpected(
            make_error(std::string(label) + " exceeds libpq integer limits", errc::invalid_argument));
    }

    return static_cast<int>(size);
}

[[nodiscard]] auto prepare_params(text_parameters params) -> std::expected<prepared_params, error> {
    auto count = checked_count(params.size(), "parameter count");
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }

    prepared_params prepared{};
    prepared.count = *count;
    prepared.views.reserve(params.size());
    prepared.owned_values.reserve(params.size());
    prepared.raw_values.reserve(params.size());

    for (const auto &param : params) {
        prepared.views.push_back(param);
        if (param.has_value()) {
            auto copied = copy_c_string_argument(*param, "query parameter");
            if (!copied) {
                return std::unexpected(std::move(copied.error()));
            }
            prepared.owned_values.push_back(std::move(*copied));
        }
    }

    auto owned_index = std::size_t{0};
    for (const auto &param : prepared.views) {
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
    case PGRES_PIPELINE_SYNC:
    case PGRES_PIPELINE_ABORTED:
    case PGRES_TUPLES_CHUNK:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] auto map_connection_status(ConnStatusType status) noexcept -> connection_status {
    switch (status) {
    case CONNECTION_OK:
        return connection_status::ok;
    case CONNECTION_BAD:
        return connection_status::bad;
    case CONNECTION_STARTED:
        return connection_status::started;
    case CONNECTION_MADE:
        return connection_status::made;
    case CONNECTION_AWAITING_RESPONSE:
        return connection_status::awaiting_response;
    case CONNECTION_AUTH_OK:
        return connection_status::auth_ok;
    case CONNECTION_SETENV:
        return connection_status::setenv;
    case CONNECTION_SSL_STARTUP:
        return connection_status::ssl_startup;
    case CONNECTION_NEEDED:
        return connection_status::needed;
    case CONNECTION_CHECK_WRITABLE:
        return connection_status::check_writable;
    case CONNECTION_CONSUME:
        return connection_status::consume;
    case CONNECTION_GSS_STARTUP:
        return connection_status::gss_startup;
    case CONNECTION_CHECK_TARGET:
        return connection_status::check_target;
    case CONNECTION_CHECK_STANDBY:
        return connection_status::check_standby;
    case CONNECTION_ALLOCATED:
        return connection_status::allocated;
    case CONNECTION_AUTHENTICATING:
        return connection_status::authenticating;
    default:
        return connection_status::unknown;
    }
}

} // namespace

struct connection::impl {
    explicit impl(detail::connection_handle connection_handle) noexcept
        : handle(std::move(connection_handle)) {
    }

    [[nodiscard]] auto ensure_handle() const -> std::expected<PGconn *, error> {
        if (handle == nullptr) {
            return std::unexpected(make_handle_error("connection"));
        }

        return handle.get();
    }

    [[nodiscard]] auto ensure_open() const -> std::expected<PGconn *, error> {
        auto raw_handle = ensure_handle();
        if (!raw_handle) {
            return std::unexpected(std::move(raw_handle.error()));
        }

        if (PQstatus(*raw_handle) != CONNECTION_OK) {
            return std::unexpected(make_connection_error(*raw_handle));
        }

        return *raw_handle;
    }

    [[nodiscard]] auto ensure_nonblocking() -> std::expected<void, error> {
        auto raw_handle = ensure_handle();
        if (!raw_handle) {
            return std::unexpected(std::move(raw_handle.error()));
        }

        if (PQisnonblocking(*raw_handle) != 0) {
            return {};
        }

        if (PQsetnonblocking(*raw_handle, 1) != 0) {
            return std::unexpected(make_connection_error(*raw_handle, "failed to enable nonblocking mode"));
        }

        return {};
    }

    [[nodiscard]] auto wrap_result(PGresult *raw) const -> std::expected<result, error> {
        if (raw == nullptr) {
            return std::unexpected(make_connection_error(handle.get(), "libpq returned a null result handle"));
        }

        detail::result_handle result_handle{raw};
        if (is_success_status(PQresultStatus(result_handle.get()))) {
            return detail::result_access::make(std::move(result_handle));
        }

        return std::unexpected(make_result_error(result_handle.get()));
    }

    detail::connection_handle handle {};
};

connection::connection() noexcept = default;
connection::~connection() = default;
connection::connection(connection &&) noexcept = default;
auto connection::operator=(connection &&) noexcept -> connection & = default;

connection::connection(std::unique_ptr<impl> impl) noexcept
    : impl_(std::move(impl)) {
}

auto connection::connect(std::string_view conninfo) -> std::expected<connection, error> {
    auto copied_conninfo = copy_c_string_argument(conninfo, "connection string");
    if (!copied_conninfo) {
        return std::unexpected(std::move(copied_conninfo.error()));
    }

    detail::connection_handle handle{PQconnectdb(copied_conninfo->c_str())};
    if (handle == nullptr) {
        return std::unexpected(make_error("PQconnectdb returned a null connection handle", errc::connection_failure));
    }

    if (PQstatus(handle.get()) != CONNECTION_OK) {
        return std::unexpected(make_connection_error(handle.get()));
    }

    auto impl = std::make_unique<connection::impl>(std::move(handle));
    auto nonblocking = impl->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    return connection{std::move(impl)};
}

auto connection::connect_start(std::string_view conninfo) -> std::expected<connection, error> {
    auto copied_conninfo = copy_c_string_argument(conninfo, "connection string");
    if (!copied_conninfo) {
        return std::unexpected(std::move(copied_conninfo.error()));
    }

    detail::connection_handle handle{PQconnectStart(copied_conninfo->c_str())};
    if (handle == nullptr) {
        return std::unexpected(
            make_error("PQconnectStart returned a null connection handle", errc::connection_failure));
    }

    if (PQstatus(handle.get()) == CONNECTION_BAD) {
        return std::unexpected(make_connection_error(handle.get()));
    }

    auto impl = std::make_unique<connection::impl>(std::move(handle));
    if (map_connection_status(PQstatus(impl->handle.get())) == connection_status::ok) {
        auto nonblocking = impl->ensure_nonblocking();
        if (!nonblocking) {
            return std::unexpected(std::move(nonblocking.error()));
        }
    }

    return connection{std::move(impl)};
}

auto connection::poll_connect() -> std::expected<poll_state, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_handle();
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
        auto nonblocking = impl_->ensure_nonblocking();
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
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    return impl_->wrap_result(PQexec(*handle, copied_sql->c_str()));
}

auto connection::exec_params(std::string_view sql, text_parameters params) -> std::expected<result, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    auto prepared = prepare_params(params);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }

    const auto *values = prepared->raw_values.empty() ? nullptr : prepared->raw_values.data();
    return impl_->wrap_result(
        PQexecParams(*handle, copied_sql->c_str(), prepared->count, nullptr, values, nullptr, nullptr, 0));
}

auto connection::send_query(std::string_view sql) -> std::expected<void, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto nonblocking = impl_->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    if (PQsendQuery(*handle, copied_sql->c_str()) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to dispatch query"));
    }

    return {};
}

auto connection::send_query_params(std::string_view sql, text_parameters params)
    -> std::expected<void, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto nonblocking = impl_->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    auto copied_sql = copy_c_string_argument(sql, "SQL command");
    if (!copied_sql) {
        return std::unexpected(std::move(copied_sql.error()));
    }

    auto prepared = prepare_params(params);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }

    const auto *values = prepared->raw_values.empty() ? nullptr : prepared->raw_values.data();
    if (PQsendQueryParams(*handle, copied_sql->c_str(), prepared->count, nullptr, values, nullptr, nullptr, 0) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to dispatch parameterized query"));
    }

    return {};
}

auto connection::flush() -> std::expected<bool, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto nonblocking = impl_->ensure_nonblocking();
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
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto nonblocking = impl_->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    if (PQconsumeInput(*handle) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to consume libpq input"));
    }

    return {};
}

auto connection::next_result() -> std::expected<std::optional<result>, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_open();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    auto nonblocking = impl_->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    PGresult *raw = PQgetResult(*handle);
    if (raw == nullptr) {
        return std::optional<result>{};
    }

    auto wrapped = impl_->wrap_result(raw);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }

    return std::optional<result>{std::move(*wrapped)};
}

auto connection::reset() -> std::expected<void, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    PQreset(*handle);
    if (PQstatus(*handle) != CONNECTION_OK) {
        return std::unexpected(make_connection_error(*handle));
    }

    auto nonblocking = impl_->ensure_nonblocking();
    if (!nonblocking) {
        return std::unexpected(std::move(nonblocking.error()));
    }

    return {};
}

auto connection::reset_start() -> std::expected<void, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_handle();
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    if (PQresetStart(*handle) != 1) {
        return std::unexpected(make_connection_error(*handle, "failed to start connection reset"));
    }

    return {};
}

auto connection::poll_reset() -> std::expected<poll_state, error> {
    if (impl_ == nullptr) {
        return std::unexpected(make_handle_error("connection"));
    }

    auto handle = impl_->ensure_handle();
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
        auto nonblocking = impl_->ensure_nonblocking();
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
    if (impl_ == nullptr || impl_->handle == nullptr || PQstatus(impl_->handle.get()) != CONNECTION_OK) {
        return false;
    }

    return PQisBusy(impl_->handle.get()) != 0;
}

auto connection::is_alive() const noexcept -> bool {
    return impl_ != nullptr && impl_->handle != nullptr && PQstatus(impl_->handle.get()) == CONNECTION_OK;
}

auto connection::status() const noexcept -> connection_status {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return connection_status::bad;
    }

    return map_connection_status(PQstatus(impl_->handle.get()));
}

auto connection::backend_pid() const noexcept -> int {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return 0;
    }

    return PQbackendPID(impl_->handle.get());
}

auto connection::socket_fd() const noexcept -> int {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return -1;
    }

    return PQsocket(impl_->handle.get());
}

} // namespace atlas::pg
