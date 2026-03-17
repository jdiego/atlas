#pragma once

#include <string>
#include <string_view>

namespace atlas {

enum class pg_errc {
    unknown,
    connection_failure,
    unique_violation,
    foreign_key_violation,
    not_null_violation,
    serialization_failure,
    deadlock_detected,
    query_canceled,
    undefined_table,
    syntax_error,
};

[[nodiscard]] inline auto sqlstate_to_errc(std::string_view sqlstate) noexcept -> pg_errc {
    if (sqlstate == "23505") return pg_errc::unique_violation;
    if (sqlstate == "23503") return pg_errc::foreign_key_violation;
    if (sqlstate == "23502") return pg_errc::not_null_violation;
    if (sqlstate == "40001") return pg_errc::serialization_failure;
    if (sqlstate == "40P01") return pg_errc::deadlock_detected;
    if (sqlstate == "57014") return pg_errc::query_canceled;
    if (sqlstate == "42P01") return pg_errc::undefined_table;
    if (sqlstate == "42601") return pg_errc::syntax_error;
    return pg_errc::unknown;
}

struct pg_error {
    std::string message;
    std::string sqlstate;
    pg_errc     code;

    [[nodiscard]] auto is_retryable() const noexcept -> bool {
        return code == pg_errc::serialization_failure
            || code == pg_errc::deadlock_detected;
    }
};

} // namespace atlas
