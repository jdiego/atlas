#pragma once

#include "atlas/pg/error.hpp"
#include "atlas/pg/types.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace atlas::pg {

namespace detail {

struct result_handle_adopter;

} // namespace detail

class connection;

class result {
public:
    result() noexcept;
    ~result();

    result(const result&) = delete;
    auto operator=(const result&) -> result& = delete;

    result(result&&) noexcept;
    auto operator=(result&&) noexcept -> result&;

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto rows() const noexcept -> std::size_t;
    [[nodiscard]] auto columns() const noexcept -> std::size_t;
    [[nodiscard]] auto is_null(std::size_t row, std::size_t col) const -> std::expected<bool, error>;
    [[nodiscard]] auto field(std::size_t row, std::size_t col) const
        -> std::expected<std::optional<std::string_view>, error>;
    [[nodiscard]] auto get(std::size_t row, std::size_t col) const
        -> std::expected<std::optional<std::string_view>, error>;
    [[nodiscard]] auto status() const noexcept -> result_status;
    [[nodiscard]] auto error_message() const noexcept -> std::string_view;
    [[nodiscard]] auto column_type(std::size_t col) const -> std::expected<oid, error>;

private:
    struct impl;

    explicit result(std::unique_ptr<impl> impl) noexcept;

    [[nodiscard]] auto validate_column(std::size_t col) const -> std::expected<void, error>;
    [[nodiscard]] auto validate_cell(std::size_t row, std::size_t col) const -> std::expected<void, error>;

    std::unique_ptr<impl> impl_ {};

    friend class connection;
    // Keeps libpq ownership details private while allowing internal construction.
    friend struct detail::result_handle_adopter;
};

} // namespace atlas::pg
