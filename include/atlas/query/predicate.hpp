#pragma once
//
// Predicate AST nodes for the query builder.
//
// Each predicate is a plain struct with no virtual dispatch, no heap, and no
// exceptions. Nodes compose via and_/or_/not_ combinators to form a binary
// tree whose leaves are comparison expressions.
//
// The is_predicate concept detects any specialisation of the node templates
// listed here so that select_query::where() and update_query::where() can
// enforce a predicate argument at compile time.

#include <type_traits>

#include "atlas/query/expr.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Leaf comparison nodes
// ColRef is always column_ref<Entity, T>; Rhs is literal<V> or column_ref.
// ---------------------------------------------------------------------------

template<typename ColRef, typename Rhs> struct eq_expr   { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct ne_expr   { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct lt_expr   { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct gt_expr   { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct lte_expr  { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct gte_expr  { ColRef lhs; Rhs rhs; };
template<typename ColRef, typename Rhs> struct like_expr { ColRef lhs; Rhs rhs; };

template<typename ColRef> struct is_null_expr     { ColRef col; };
template<typename ColRef> struct is_not_null_expr { ColRef col; };

// Container is expected to be e.g. std::vector<T> or std::initializer_list<T>.
template<typename ColRef, typename Container>
struct in_expr { ColRef col; Container values; };

// ---------------------------------------------------------------------------
// Boolean combinators
// ---------------------------------------------------------------------------

template<typename L, typename R> struct and_expr { L lhs; R rhs; };
template<typename L, typename R> struct or_expr  { L lhs; R rhs; };
template<typename P>             struct not_expr  { P inner; };

// ---------------------------------------------------------------------------
// Concept: is_predicate
// ---------------------------------------------------------------------------

namespace detail {

template<typename T> struct is_predicate_impl : std::false_type {};

template<typename C, typename R> struct is_predicate_impl<eq_expr<C,R>>       : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<ne_expr<C,R>>       : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<lt_expr<C,R>>       : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<gt_expr<C,R>>       : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<lte_expr<C,R>>      : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<gte_expr<C,R>>      : std::true_type {};
template<typename C, typename R> struct is_predicate_impl<like_expr<C,R>>     : std::true_type {};
template<typename C>             struct is_predicate_impl<is_null_expr<C>>     : std::true_type {};
template<typename C>             struct is_predicate_impl<is_not_null_expr<C>> : std::true_type {};
template<typename C, typename V> struct is_predicate_impl<in_expr<C,V>>        : std::true_type {};
template<typename L, typename R> struct is_predicate_impl<and_expr<L,R>>       : std::true_type {};
template<typename L, typename R> struct is_predicate_impl<or_expr<L,R>>        : std::true_type {};
template<typename P>             struct is_predicate_impl<not_expr<P>>          : std::true_type {};

} // namespace detail

template<typename P>
concept is_predicate = detail::is_predicate_impl<std::remove_cvref_t<P>>::value;

// ---------------------------------------------------------------------------
// Factory: eq
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto eq(T Entity::* col, V&& val)
    -> eq_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an eq_expr node representing "col = val".
     *
     * Step 1 — Construct lhs = column_ref<Entity, T>{col}.
     * Step 2 — Construct rhs = literal<std::remove_cvref_t<V>>{std::forward<V>(val)}.
     * Step 3 — Return eq_expr<...>{lhs, rhs}.
     *
     * Key types involved:
     *   - column_ref<Entity, T>: the left-hand column reference.
     *   - literal<V>: the right-hand bound parameter.
     *
     * Preconditions:
     *   - col is a valid non-null member pointer.
     *   - V must be serialisable via detail::value_serializer<V>::to_string().
     *
     * Postconditions:
     *   - Returned node carries lhs.ptr == col and rhs.value == val.
     *
     * Pitfalls:
     *   - Forward val, do not copy twice.
     *
     * Hint:
     *   return {column_ref<Entity,T>{col}, literal<std::remove_cvref_t<V>>{std::forward<V>(val)}};
     */
}

// ---------------------------------------------------------------------------
// Factory: ne
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto ne(T Entity::* col, V&& val)
    -> ne_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an ne_expr node representing "col != val".
     *
     * Step 1 — Same pattern as eq(): construct lhs and rhs, return ne_expr{lhs, rhs}.
     *
     * Hint: return {column_ref<Entity,T>{col}, literal<std::remove_cvref_t<V>>{std::forward<V>(val)}};
     */
}

// ---------------------------------------------------------------------------
// Factory: lt
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto lt(T Entity::* col, V&& val)
    -> lt_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an lt_expr node representing "col < val".
     *
     * Step 1 — Construct and return lt_expr{column_ref{col}, literal{val}}.
     *
     * Hint: identical pattern to eq().
     */
}

// ---------------------------------------------------------------------------
// Factory: gt
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto gt(T Entity::* col, V&& val)
    -> gt_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds a gt_expr node representing "col > val".
     *
     * Step 1 — Construct and return gt_expr{column_ref{col}, literal{val}}.
     *
     * Hint: identical pattern to eq().
     */
}

// ---------------------------------------------------------------------------
// Factory: lte
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto lte(T Entity::* col, V&& val)
    -> lte_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an lte_expr node representing "col <= val".
     *
     * Hint: identical pattern to eq().
     */
}

// ---------------------------------------------------------------------------
// Factory: gte
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto gte(T Entity::* col, V&& val)
    -> gte_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds a gte_expr node representing "col >= val".
     *
     * Hint: identical pattern to eq().
     */
}

// ---------------------------------------------------------------------------
// Factory: like
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename V>
constexpr auto like(T Entity::* col, V&& val)
    -> like_expr<column_ref<Entity, T>, literal<std::remove_cvref_t<V>>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds a like_expr node representing "col LIKE val".
     *
     * Step 1 — Construct and return like_expr{column_ref{col}, literal{val}}.
     *
     * Pitfalls:
     *   - The pattern string (val) is sent as a PostgreSQL parameter ($N), so
     *     the driver handles quoting; do NOT add extra quotes here.
     *
     * Hint: identical pattern to eq().
     */
}

// ---------------------------------------------------------------------------
// Factory: is_null
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto is_null(T Entity::* col)
    -> is_null_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an is_null_expr node representing "col IS NULL".
     *
     * Step 1 — Return is_null_expr<column_ref<Entity,T>>{column_ref<Entity,T>{col}}.
     *
     * Preconditions:
     *   - col is a valid member pointer.
     *
     * Postconditions:
     *   - No parameter placeholder is emitted; IS NULL takes no bound value.
     *
     * Hint:
     *   return {column_ref<Entity, T>{col}};
     */
}

// ---------------------------------------------------------------------------
// Factory: is_not_null
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto is_not_null(T Entity::* col)
    -> is_not_null_expr<column_ref<Entity, T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an is_not_null_expr node representing "col IS NOT NULL".
     *
     * Hint: mirror is_null() exactly, changing the return type.
     */
}

// ---------------------------------------------------------------------------
// Factory: in
// ---------------------------------------------------------------------------

template<typename Entity, typename T, typename Container>
constexpr auto in(T Entity::* col, Container&& vals)
    -> in_expr<column_ref<Entity, T>, std::remove_cvref_t<Container>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Builds an in_expr node representing "col IN ($1, $2, …)".
     *
     * Step 1 — Construct lhs = column_ref<Entity, T>{col}.
     * Step 2 — Move/copy vals into the in_expr::values field.
     * Step 3 — Return in_expr<...>{lhs, std::forward<Container>(vals)}.
     *
     * Key types involved:
     *   - Container: e.g. std::vector<int32_t>; iterated by serialise_predicate
     *     to emit one "$N" placeholder per element.
     *
     * Preconditions:
     *   - Container must be range-iterable with elements serialisable by
     *     detail::value_serializer<value_type>.
     *
     * Postconditions:
     *   - in_expr::values holds a copy (or move) of vals.
     *
     * Pitfalls:
     *   - An empty container must still produce valid SQL: "col IN ()" is a
     *     PostgreSQL error; the serialiser should handle this edge case.
     *
     * Hint:
     *   return {column_ref<Entity,T>{col}, std::forward<Container>(vals)};
     */
}

// ---------------------------------------------------------------------------
// Factory: and_
// ---------------------------------------------------------------------------

template<typename L, typename R>
constexpr auto and_(L&& l, R&& r)
    -> and_expr<std::remove_cvref_t<L>, std::remove_cvref_t<R>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Combines two predicate nodes into an and_expr, producing
     *   "(lhs AND rhs)" when serialised.
     *
     * Step 1 — Forward both arguments into and_expr{l, r}.
     *
     * Key types involved:
     *   - L, R: any two types satisfying is_predicate.
     *
     * Preconditions:
     *   - Both l and r must be valid predicate AST nodes.
     *
     * Postconditions:
     *   - and_expr::lhs and ::rhs hold copies (or moves) of l and r.
     *
     * Hint:
     *   return {std::forward<L>(l), std::forward<R>(r)};
     */
}

// ---------------------------------------------------------------------------
// Factory: or_
// ---------------------------------------------------------------------------

template<typename L, typename R>
constexpr auto or_(L&& l, R&& r)
    -> or_expr<std::remove_cvref_t<L>, std::remove_cvref_t<R>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Combines two predicate nodes into an or_expr, producing
     *   "(lhs OR rhs)" when serialised.
     *
     * Hint: mirror and_() exactly; only the return type differs.
     */
}

// ---------------------------------------------------------------------------
// Factory: not_
// ---------------------------------------------------------------------------

template<typename P>
constexpr auto not_(P&& p)
    -> not_expr<std::remove_cvref_t<P>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a predicate in a not_expr, producing "NOT (inner)" when
     *   serialised.
     *
     * Step 1 — Forward p into not_expr{p}.
     *
     * Hint:
     *   return {std::forward<P>(p)};
     */
}

} // namespace atlas
