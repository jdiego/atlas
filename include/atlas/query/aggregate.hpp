#pragma once
//
// Aggregate expression nodes for the query builder.
//
// Aggregate nodes are used as arguments to atlas::select() in place of
// (or alongside) plain column_ref nodes.  They carry a single ColRef and are
// recognised by the is_aggregate concept so that sql_serialize can emit the
// correct SQL function name (COUNT, SUM, AVG, MIN, MAX).
//
// count_star_expr is the only node with no ColRef; it represents COUNT(*).

#include <type_traits>

#include "atlas/query/expr.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Aggregate node templates
// ColRef is always column_ref<Entity, T>.
// ---------------------------------------------------------------------------

template<typename ColRef> struct count_expr { ColRef col; };
template<typename ColRef> struct sum_expr   { ColRef col; };
template<typename ColRef> struct avg_expr   { ColRef col; };
template<typename ColRef> struct min_expr   { ColRef col; };
template<typename ColRef> struct max_expr   { ColRef col; };

// COUNT(*) — no column reference.
struct count_star_expr {};

// ---------------------------------------------------------------------------
// Concept: is_aggregate
// ---------------------------------------------------------------------------

namespace detail {

template<typename T> struct is_aggregate_impl : std::false_type {};

template<typename C> struct is_aggregate_impl<count_expr<C>> : std::true_type {};
template<typename C> struct is_aggregate_impl<sum_expr<C>>   : std::true_type {};
template<typename C> struct is_aggregate_impl<avg_expr<C>>   : std::true_type {};
template<typename C> struct is_aggregate_impl<min_expr<C>>   : std::true_type {};
template<typename C> struct is_aggregate_impl<max_expr<C>>   : std::true_type {};
template<>           struct is_aggregate_impl<count_star_expr> : std::true_type {};

} // namespace detail

template<typename A>
concept is_aggregate = detail::is_aggregate_impl<std::remove_cvref_t<A>>::value;

// ---------------------------------------------------------------------------
// Factory: count(member_ptr) — COUNT(col)
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto count(T Entity::* col)
    -> count_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a column member pointer in a count_expr, so that
     *   atlas::select(atlas::count(&User::id)) emits "COUNT(u.id)".
     *
     * Step 1 — Construct column_ref<Entity, T>{col}.
     * Step 2 — Return count_expr<column_ref<Entity, T>>{col_ref}.
     *
     * Key types involved:
     *   - count_expr<ColRef>: the aggregate node (this file).
     *   - column_ref<Entity, T>: the leaf node wrapping the member pointer.
     *
     * Preconditions:
     *   - col must be a valid non-null member pointer registered in a table_t.
     *
     * Postconditions:
     *   - Returned count_expr::col.ptr == col.
     *
     * Pitfalls:
     *   - Do not confuse with count() (no args) which returns count_star_expr.
     *
     * Hint:
     *   return count_expr<column_ref<Entity,T>>{column_ref<Entity,T>{col}};
     */
}

// ---------------------------------------------------------------------------
// Factory: count() — COUNT(*)
// ---------------------------------------------------------------------------

constexpr auto count() -> count_star_expr
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Returns a count_star_expr, serialised as "COUNT(*)".
     *
     * Step 1 — Return a default-constructed count_star_expr{}.
     *
     * Pitfalls:
     *   - This overload must resolve before the member-pointer overload;
     *     it has a distinct signature (no parameters) so overload resolution
     *     is unambiguous.
     *
     * Hint:
     *   return count_star_expr{};
     */
}

// ---------------------------------------------------------------------------
// Factory: sum(member_ptr)
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto sum(T Entity::* col)
    -> sum_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a column in a sum_expr, emitting "SUM(tbl.col)".
     *
     * Step 1 — Return sum_expr<column_ref<Entity,T>>{column_ref<Entity,T>{col}}.
     *
     * Hint: identical pattern to count(member_ptr).
     */
}

// ---------------------------------------------------------------------------
// Factory: avg(member_ptr)
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto avg(T Entity::* col)
    -> avg_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a column in an avg_expr, emitting "AVG(tbl.col)".
     *
     * Hint: identical pattern to count(member_ptr).
     */
}

// ---------------------------------------------------------------------------
// Factory: min(member_ptr)
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto min(T Entity::* col)
    -> min_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a column in a min_expr, emitting "MIN(tbl.col)".
     *
     * Hint: identical pattern to count(member_ptr).
     */
}

// ---------------------------------------------------------------------------
// Factory: max(member_ptr)
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto max(T Entity::* col)
    -> max_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a column in a max_expr, emitting "MAX(tbl.col)".
     *
     * Hint: identical pattern to count(member_ptr).
     */
}

} // namespace atlas
