#pragma once

#include <libpq-fe.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "atlas/pg/error.hpp"

namespace atlas::pg {

namespace detail {

struct result_deleter {
    void operator()(PGresult* handle) const noexcept;
};

} // namespace detail

class connection;

class result {
public:
    result() noexcept = default;
    ~result() = default;

    result(const result&) = delete;
    auto operator=(const result&) -> result& = delete;

    result(result&&) noexcept = default;
    auto operator=(result&&) noexcept -> result& = default;

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto rows() const noexcept -> std::size_t;
    [[nodiscard]] auto columns() const noexcept -> std::size_t;
    [[nodiscard]] auto is_null(std::size_t row, std::size_t col) const -> std::expected<bool, error>;
    [[nodiscard]] auto field(std::size_t row, std::size_t col) const
        -> std::expected<std::optional<std::string_view>, error>;
    [[nodiscard]] auto get(std::size_t row, std::size_t col) const
        -> std::expected<std::optional<std::string_view>, error>;
    [[nodiscard]] auto status() const noexcept -> ExecStatusType;
    [[nodiscard]] auto error_message() const noexcept -> std::string_view;
    [[nodiscard]] auto column_type(std::size_t col) const -> std::expected<Oid, error>;

private:
    using handle_type = std::unique_ptr<PGresult, detail::result_deleter>;

    explicit result(handle_type handle) noexcept;

    [[nodiscard]] auto validate_column(std::size_t col) const -> std::expected<void, error>;
    [[nodiscard]] auto validate_cell(std::size_t row, std::size_t col) const -> std::expected<void, error>;

    handle_type handle_ {};

    friend class connection;
};

} // namespace atlas::pg
