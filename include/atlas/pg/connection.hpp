#pragma once

#include <libpq-fe.h>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

namespace atlas::pg {

using text_param = std::optional<std::string_view>;

enum class poll_state {
    reading,
    writing,
    active,
    ready,
};

namespace detail {

struct connection_deleter {
    void operator()(PGconn* handle) const noexcept;
};

} // namespace detail

class connection {
public:
    connection() noexcept = default;
    ~connection() = default;

    connection(const connection&) = delete;
    auto operator=(const connection&) -> connection& = delete;

    connection(connection&&) noexcept = default;
    auto operator=(connection&&) noexcept -> connection& = default;

    [[nodiscard]] static auto connect(std::string_view conninfo) -> std::expected<connection, error>;
    [[nodiscard]] static auto connect_start(std::string_view conninfo) -> std::expected<connection, error>;

    [[nodiscard]] auto poll_connect() -> std::expected<poll_state, error>;

    [[nodiscard]] auto exec(std::string_view sql) -> std::expected<result, error>;
    [[nodiscard]] auto exec_params(std::string_view sql, std::span<const text_param> params)
        -> std::expected<result, error>;

    [[nodiscard]] auto send_query(std::string_view sql) -> std::expected<void, error>;
    [[nodiscard]] auto send_query_params(std::string_view sql, std::span<const text_param> params)
        -> std::expected<void, error>;

    [[nodiscard]] auto flush() -> std::expected<bool, error>;
    [[nodiscard]] auto consume_input() -> std::expected<void, error>;
    [[nodiscard]] auto next_result() -> std::expected<std::optional<result>, error>;

    [[nodiscard]] auto reset() -> std::expected<void, error>;
    [[nodiscard]] auto reset_start() -> std::expected<void, error>;
    [[nodiscard]] auto poll_reset() -> std::expected<poll_state, error>;

    [[nodiscard]] auto is_busy() const noexcept -> bool;
    [[nodiscard]] auto is_alive() const noexcept -> bool;
    [[nodiscard]] auto status() const noexcept -> ConnStatusType;
    [[nodiscard]] auto backend_pid() const noexcept -> int;
    [[nodiscard]] auto socket_fd() const noexcept -> int;

private:
    using handle_type = std::unique_ptr<PGconn, detail::connection_deleter>;

    explicit connection(handle_type handle) noexcept;

    [[nodiscard]] auto ensure_handle() const -> std::expected<PGconn*, error>;
    [[nodiscard]] auto ensure_open() const -> std::expected<PGconn*, error>;
    [[nodiscard]] auto ensure_nonblocking() -> std::expected<void, error>;
    [[nodiscard]] auto wrap_result(PGresult* raw) -> std::expected<result, error>;

    handle_type handle_ {};
};

} // namespace atlas::pg
