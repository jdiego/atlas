#include "atlas/pg/result.hpp"
#include "detail/result_handle_adopter.hpp"

#include <libpq-fe.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace atlas::pg {

struct result::impl {
    explicit impl(detail::result_handle result_handle) noexcept
        : handle(std::move(result_handle)) {
    }

    detail::result_handle handle {};
};

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

[[nodiscard]] auto map_result_status(ExecStatusType status) noexcept -> result_status {
    switch (status) {
    case PGRES_EMPTY_QUERY:
        return result_status::empty_query;
    case PGRES_COMMAND_OK:
        return result_status::command_ok;
    case PGRES_TUPLES_OK:
        return result_status::tuples_ok;
    case PGRES_COPY_OUT:
        return result_status::copy_out;
    case PGRES_COPY_IN:
        return result_status::copy_in;
    case PGRES_BAD_RESPONSE:
        return result_status::bad_response;
    case PGRES_NONFATAL_ERROR:
        return result_status::nonfatal_error;
    case PGRES_FATAL_ERROR:
        return result_status::fatal_error;
    case PGRES_COPY_BOTH:
        return result_status::copy_both;
    case PGRES_SINGLE_TUPLE:
        return result_status::single_tuple;
    case PGRES_PIPELINE_SYNC:
        return result_status::pipeline_sync;
    case PGRES_PIPELINE_ABORTED:
        return result_status::pipeline_aborted;
    case PGRES_TUPLES_CHUNK:
        return result_status::tuples_chunk;
    default:
        return result_status::unknown;
    }
}

} // namespace

result::result() noexcept = default;
result::~result() = default;
result::result(result &&) noexcept = default;
auto result::operator=(result &&) noexcept -> result & = default;

result::result(std::unique_ptr<impl> impl) noexcept
    : impl_(std::move(impl)) {
}

auto result::empty() const noexcept -> bool {
    return rows() == 0U;
}

auto result::rows() const noexcept -> std::size_t {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return 0U;
    }
    return static_cast<std::size_t>(PQntuples(impl_->handle.get()));
}

auto result::columns() const noexcept -> std::size_t {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return 0U;
    }
    return static_cast<std::size_t>(PQnfields(impl_->handle.get()));
}

auto result::validate_column(std::size_t col) const -> std::expected<void, error> {
    if (impl_ == nullptr || impl_->handle == nullptr) {
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
    if (impl_ == nullptr || impl_->handle == nullptr) {
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

    return PQgetisnull(impl_->handle.get(), *row_index, *column_index) != 0;
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

    if (PQgetisnull(impl_->handle.get(), *row_index, *column_index) != 0) {
        return std::optional<std::string_view> {};
    }

    const auto *value = PQgetvalue(impl_->handle.get(), *row_index, *column_index);
    const auto length = static_cast<std::size_t>(PQgetlength(impl_->handle.get(), *row_index, *column_index));
    return std::optional<std::string_view> {std::string_view {value, length}};
}

auto result::get(std::size_t row, std::size_t col) const
    -> std::expected<std::optional<std::string_view>, error> {
    return field(row, col);
}

auto result::status() const noexcept -> result_status {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return result_status::unknown;
    }
    return map_result_status(PQresultStatus(impl_->handle.get()));
}

auto result::error_message() const noexcept -> std::string_view {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return {};
    }

    const auto *message = PQresultErrorMessage(impl_->handle.get());
    if (message == nullptr) {
        return {};
    }

    return std::string_view {message};
}

auto result::column_type(std::size_t col) const -> std::expected<oid, error> {
    const auto checked_column = validate_column(col);
    if (!checked_column) {
        return std::unexpected(std::move(checked_column.error()));
    }

    const auto column_index = to_libpq_index(col, "column");
    if (!column_index) {
        return std::unexpected(std::move(column_index.error()));
    }

    return static_cast<oid>(PQftype(impl_->handle.get(), *column_index));
}

auto detail::result_handle_adopter::make(result_handle handle) -> result {
    return result {std::make_unique<result::impl>(std::move(handle))};
}

} // namespace atlas::pg
