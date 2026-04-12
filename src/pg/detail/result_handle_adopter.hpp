#pragma once

#include "atlas/pg/result.hpp"
#include "libpq_handles.hpp"

namespace atlas::pg::detail {

// Adopts a validated libpq result handle into the public result wrapper.
struct result_handle_adopter {
    [[nodiscard]] static auto make(result_handle handle) -> result;
};

} // namespace atlas::pg::detail
