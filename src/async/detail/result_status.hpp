#pragma once

#include <libpq-fe.h>

namespace atlas::detail {

enum class async_result_disposition {
    success,
    unsupported_copy,
    error,
};

[[nodiscard]] constexpr auto classify_async_result(ExecStatusType status) noexcept -> async_result_disposition {
    switch (status) {
    case PGRES_EMPTY_QUERY:
    case PGRES_COMMAND_OK:
    case PGRES_TUPLES_OK:
    case PGRES_SINGLE_TUPLE:
    case PGRES_PIPELINE_SYNC:
    case PGRES_TUPLES_CHUNK:
        return async_result_disposition::success;
    case PGRES_COPY_OUT:
    case PGRES_COPY_IN:
    case PGRES_COPY_BOTH:
        return async_result_disposition::unsupported_copy;
    default:
        return async_result_disposition::error;
    }
}

} // namespace atlas::detail
