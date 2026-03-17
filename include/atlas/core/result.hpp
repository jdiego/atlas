#pragma once

#include <string_view>
#include <cstddef>
#include <utility>
#include <libpq-fe.h>

namespace atlas {

class result {
public:
    explicit result(PGresult* res) noexcept : res_(res) {}

    ~result() noexcept {
        if (res_) PQclear(res_);
    }

    result(const result&)            = delete;
    result& operator=(const result&) = delete;

    result(result&& other) noexcept
        : res_(std::exchange(other.res_, nullptr)) {}

    result& operator=(result&& other) noexcept {
        if (this != &other) {
            if (res_) PQclear(res_);
            res_ = std::exchange(other.res_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] auto rows() const noexcept -> int {
        return PQntuples(res_);
    }

    [[nodiscard]] auto columns() const noexcept -> int {
        return PQnfields(res_);
    }

    [[nodiscard]] auto is_null(int row, int col) const noexcept -> bool {
        return PQgetisnull(res_, row, col) == 1;
    }

    [[nodiscard]] auto get(int row, int col) const noexcept -> std::string_view {
        if (is_null(row, col)) return {};
        return {PQgetvalue(res_, row, col),
                static_cast<std::size_t>(PQgetlength(res_, row, col))};
    }

    [[nodiscard]] auto status() const noexcept -> ExecStatusType {
        return PQresultStatus(res_);
    }

    [[nodiscard]] auto error_message() const noexcept -> std::string_view {
        return PQresultErrorMessage(res_);
    }

    [[nodiscard]] auto column_type(int col) const noexcept -> Oid {
        return PQftype(res_, col);
    }

private:
    PGresult* res_;
};

} // namespace atlas
