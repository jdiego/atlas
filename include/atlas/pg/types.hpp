#pragma once

#include <cstdint>

namespace atlas::pg {

using oid = std::uint32_t;

enum class connection_status : std::uint8_t {
    ok,
    bad,
    started,
    made,
    awaiting_response,
    auth_ok,
    setenv,
    ssl_startup,
    needed,
    check_writable,
    consume,
    gss_startup,
    check_target,
    check_standby,
    allocated,
    authenticating,
    unknown,
};

enum class result_status : std::uint8_t {
    empty_query,
    command_ok,
    tuples_ok,
    copy_out,
    copy_in,
    bad_response,
    nonfatal_error,
    fatal_error,
    copy_both,
    single_tuple,
    pipeline_sync,
    pipeline_aborted,
    tuples_chunk,
    unknown,
};

} // namespace atlas::pg
