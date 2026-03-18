#include "atlas/pg/result.hpp"

#include <limits>
#include <string>
#include <utility>

namespace atlas::pg {

namespace {

[[nodiscard]] auto make_error(std::string message, errc code = errc::unknown) -> error {
    return error {
        .message = std::move(message),
        .sqlstate = {},
        .code = code,
    };
}

[[nodiscard]] auto make_empty_result_error() -> error {
    return make_error("result handle is empty", errc::invalid_state);
}

[[nodiscard]] auto make_out_of_range_error(
    std::string_view label,
    std::size_t index,
    std::size_t bound) -> error {
    return make_error(
        std::string(label) + " index " + std::to_string(index) +
        " is out of range for bound " + std::to_string(bound),
        errc::invalid_argument);
}

[[nodiscard]] auto make_too_large_error(std::string_view label, std::size_t index) -> error {
    return make_error(
        std::string(label) + " index " + std::to_string(index) +
        " exceeds libpq integer limits",
        errc::invalid_argument);
}

[[nodiscard]] auto to_libpq_index(std::size_t index, std::string_view label)
    -> std::expected<int, error> {
    constexpr auto max_int = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (index > max_int) {
        return std::unexpected(make_too_large_error(label, index));
    }
    return static_cast<int>(index);
}

} // namespace

void detail::result_deleter::operator()(PGresult* handle) const noexcept {
    if (handle != nullptr) {
        PQclear(handle);
    }
}

result::result(handle_type handle) noexcept
    : handle_(std::move(handle)) {}

auto result::empty() const noexcept -> bool {
    return handle_ == nullptr;
}

auto result::rows() const noexcept -> std::size_t {
    if (handle_ == nullptr) {
        return 0U;
    }
    return static_cast<std::size_t>(PQntuples(handle_.get()));
}

auto result::columns() const noexcept -> std::size_t {
    if (handle_ == nullptr) {
        return 0U;
    }
    return static_cast<std::size_t>(PQnfields(handle_.get()));
}

auto result::validate_column(std::size_t col) const -> std::expected<void, error> {
    if (handle_ == nullptr) {
        return std::unexpected(make_empty_result_error());
    }

    const auto checked_col = to_libpq_index(col, "column");
    if (!checked_col) {
        return std::unexpected(std::move(checked_col.error()));
    }

    const auto column_count = columns();
    if (col >= column_count) {
        return std::unexpected(make_out_of_range_error("column", col, column_count));
    }

    return {};
}

auto result::validate_cell(std::size_t row, std::size_t col) const -> std::expected<void, error> {
    if (handle_ == nullptr) {
        return std::unexpected(make_empty_result_error());
    }

    const auto checked_row = to_libpq_index(row, "row");
    if (!checked_row) {
        return std::unexpected(std::move(checked_row.error()));
    }

    const auto checked_column = validate_column(col);
    if (!checked_column) {
        return std::unexpected(std::move(checked_column.error()));
    }

    const auto row_count = rows();
    if (row >= row_count) {
        return std::unexpected(make_out_of_range_error("row", row, row_count));
    }

    return {};
}

auto result::is_null(std::size_t row, std::size_t col) const -> std::expected<bool, error> {
    const auto checked_cell = validate_cell(row, col);
    if (!checked_cell) {
        return std::unexpected(std::move(checked_cell.error()));
    }

    const auto row_index = to_libpq_index(row, "row");
    if (!row_index) {
        return std::unexpected(std::move(row_index.error()));
    }

    const auto column_index = to_libpq_index(col, "column");
    if (!column_index) {
        return std::unexpected(std::move(column_index.error()));
    }

    return PQgetisnull(handle_.get(), *row_index, *column_index) != 0;
}

auto result::field(std::size_t row, std::size_t col) const
    -> std::expected<std::optional<std::string_view>, error> {
    const auto checked_cell = validate_cell(row, col);
    if (!checked_cell) {
        return std::unexpected(std::move(checked_cell.error()));
    }

    const auto row_index = to_libpq_index(row, "row");
    if (!row_index) {
        return std::unexpected(std::move(row_index.error()));
    }

    const auto column_index = to_libpq_index(col, "column");
    if (!column_index) {
        return std::unexpected(std::move(column_index.error()));
    }

    if (PQgetisnull(handle_.get(), *row_index, *column_index) != 0) {
        return std::optional<std::string_view> {};
    }

    const auto* value = PQgetvalue(handle_.get(), *row_index, *column_index);
    const auto length = static_cast<std::size_t>(PQgetlength(handle_.get(), *row_index, *column_index));
    return std::optional<std::string_view> {std::string_view {value, length}};
}

auto result::get(std::size_t row, std::size_t col) const
    -> std::expected<std::optional<std::string_view>, error> {
    return field(row, col);
}

auto result::status() const noexcept -> ExecStatusType {
    if (handle_ == nullptr) {
        return PGRES_FATAL_ERROR;
    }
    return PQresultStatus(handle_.get());
}

auto result::error_message() const noexcept -> std::string_view {
    if (handle_ == nullptr) {
        return {};
    }

    const auto* message = PQresultErrorMessage(handle_.get());
    if (message == nullptr) {
        return {};
    }

    return std::string_view {message};
}

auto result::column_type(std::size_t col) const -> std::expected<Oid, error> {
    const auto checked_column = validate_column(col);
    if (!checked_column) {
        return std::unexpected(std::move(checked_column.error()));
    }

    const auto column_index = to_libpq_index(col, "column");
    if (!column_index) {
        return std::unexpected(std::move(column_index.error()));
    }

    return PQftype(handle_.get(), *column_index);
}

} // namespace atlas::pg
