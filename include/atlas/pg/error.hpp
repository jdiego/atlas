#pragma once

#include <string>
#include <string_view>

namespace atlas::pg {

enum class errc {
    unknown,
    invalid_argument,
    invalid_state,
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

[[nodiscard]] inline auto sqlstate_to_errc(std::string_view sqlstate) noexcept -> errc {
    if (sqlstate == "23505") {
        return errc::unique_violation;
    }
    if (sqlstate == "23503") {
        return errc::foreign_key_violation;
    }
    if (sqlstate == "23502") {
        return errc::not_null_violation;
    }
    if (sqlstate == "40001") {
        return errc::serialization_failure;
    }
    if (sqlstate == "40P01") {
        return errc::deadlock_detected;
    }
    if (sqlstate == "57014") {
        return errc::query_canceled;
    }
    if (sqlstate == "42P01") {
        return errc::undefined_table;
    }
    if (sqlstate == "42601") {
        return errc::syntax_error;
    }
    return errc::unknown;
}

struct error {
    std::string message {};
    std::string sqlstate {};
    errc code {errc::unknown};

    [[nodiscard]] auto is_retryable() const noexcept -> bool {
        return code == errc::connection_failure ||
               code == errc::serialization_failure ||
               code == errc::deadlock_detected;
    }
};

} // namespace atlas::pg
