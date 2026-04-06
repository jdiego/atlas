#pragma once

#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"
#include "atlas/pg/types.hpp"
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
namespace atlas::pg {

using text_param = std::optional<std::string_view>;
using text_parameters = std::span<const text_param>;
enum class poll_state : std::uint8_t {
    reading,
    writing,
    active,
    ready,
};

class connection {
public:
    connection() noexcept;
    ~connection();

    connection(const connection &) = delete;
    auto operator=(const connection &) -> connection & = delete;

    connection(connection &&) noexcept;
    auto operator=(connection &&) noexcept -> connection &;

    [[nodiscard]] static auto connect(std::string_view conninfo) -> std::expected<connection, error>;
    [[nodiscard]] static auto connect_start(std::string_view conninfo) -> std::expected<connection, error>;

    [[nodiscard]] auto poll_connect() -> std::expected<poll_state, error>;

    [[nodiscard]] auto exec(std::string_view sql) -> std::expected<result, error>;
    [[nodiscard]] auto exec_params(std::string_view sql, text_parameters params) -> std::expected<result, error>;

    [[nodiscard]] auto send_query(std::string_view sql) -> std::expected<void, error>;
    [[nodiscard]] auto send_query_params(std::string_view sql, text_parameters params) -> std::expected<void, error>;

    [[nodiscard]] auto flush() -> std::expected<bool, error>;
    [[nodiscard]] auto consume_input() -> std::expected<void, error>;
    [[nodiscard]] auto next_result() -> std::expected<std::optional<result>, error>;

    [[nodiscard]] auto reset() -> std::expected<void, error>;
    [[nodiscard]] auto reset_start() -> std::expected<void, error>;
    [[nodiscard]] auto poll_reset() -> std::expected<poll_state, error>;

    [[nodiscard]] auto is_busy() const noexcept -> bool;
    [[nodiscard]] auto is_alive() const noexcept -> bool;
    [[nodiscard]] auto status() const noexcept -> connection_status;
    [[nodiscard]] auto backend_pid() const noexcept -> int;
    [[nodiscard]] auto socket_fd() const noexcept -> int;

private:
    struct impl;

    explicit connection(std::unique_ptr<impl> impl) noexcept;

    std::unique_ptr<impl> impl_ {};
};

} // namespace atlas::pg
