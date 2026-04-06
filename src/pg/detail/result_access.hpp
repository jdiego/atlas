#pragma once

#include "atlas/pg/result.hpp"
#include "libpq_handles.hpp"

namespace atlas::pg::detail {

struct result_access {
    [[nodiscard]] static auto make(result_handle handle) -> result;
};

} // namespace atlas::pg::detail
