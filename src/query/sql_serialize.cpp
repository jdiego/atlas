// Implementation of the non-template serialize_context methods.
//
// Only serialize_context::next_param lives here; all template functions
// (serialize_predicate, serialize_column_ref, serialize_aggregate) are
// header-only so they can be instantiated for any storage_t specialisation.

#include "atlas/query/sql_serialize.hpp"

#include <string>

namespace atlas {

// ---------------------------------------------------------------------------
// serialize_context::next_param
// ---------------------------------------------------------------------------

std::string serialize_context::next_param(std::string value)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Registers a new bound parameter value and returns its SQL placeholder
     *   string ("$1", "$2", …) so the caller can embed it in the SQL fragment.
     *
     * Step 1 — Move value into params: params.push_back(std::move(value));
     * Step 2 — Build the placeholder: "$" + std::to_string(param_counter).
     * Step 3 — Increment param_counter so the next call returns the next index.
     * Step 4 — Return the placeholder string.
     *
     * Key types involved:
     *   - params (std::vector<std::string>): the accumulated parameter list
     *     that the caller will pass to PQsendQueryParams as the paramValues
     *     array.  Index 0 → $1, index 1 → $2, etc.
     *   - param_counter (int): monotonically increasing, starts at 1.
     *
     * Preconditions:
     *   - value must be a non-empty UTF-8 string representing the serialised
     *     text value of one SQL parameter.
     *
     * Postconditions:
     *   - params.size() == old_size + 1.
     *   - param_counter == old_param_counter + 1.
     *   - Returned string == "$" + std::to_string(old_param_counter).
     *
     * Pitfalls:
     *   - Do not increment param_counter BEFORE building the placeholder
     *     string, or the returned placeholder will be off by one.
     *   - push_back(std::move(value)) invalidates the local variable; build
     *     the placeholder string BEFORE the push_back, or use the old counter
     *     value captured before the move.
     *
     * Hint:
     *   std::string placeholder = "$" + std::to_string(param_counter);
     *   params.push_back(std::move(value));
     *   ++param_counter;
     *   return placeholder;
     */
}

} // namespace atlas
