#pragma once
//
// Compile-time expression AST leaf nodes for the query builder.
//
// Every node is a value type: no heap allocation, no virtual dispatch, no
// inheritance. The AST is later walked by sql_serialize.hpp using a chain of
// if constexpr checks.
//
// NOTE on operand<Entity>:
//   A single variant<column_ref<Entity,T>, literal<V>> cannot be declared as a
//   simple alias because the two sides of a predicate may have different value
//   types (e.g. column_ref<User,int32_t> vs. literal<std::string>) and
//   different entity types in JOIN conditions. Each predicate node in
//   predicate.hpp therefore carries its own lhs/rhs types directly as template
//   parameters, making the "operand" concept structural rather than concrete.

#include <type_traits>

namespace atlas {

// ---------------------------------------------------------------------------
// column_ref<Entity, T>
// ---------------------------------------------------------------------------
// References a table column via its member pointer.
// The qualified SQL name (e.g. "u.email") is resolved at serialisation time
// by looking up Entity in storage_t and then finding the column whose
// member_ptr matches ptr.
template<typename Entity, typename T>
struct column_ref {
    T Entity::* ptr;
};

// ---------------------------------------------------------------------------
// literal<T>
// ---------------------------------------------------------------------------
// Wraps a compile-time-bound scalar value. At serialisation time the value
// is converted to a std::string, appended to the params vector, and
// replaced by a "$N" placeholder in the SQL fragment.
template<typename T>
struct literal {
    T value;
};

// ---------------------------------------------------------------------------
// column_eq_ref<LhsEntity, LhsT, RhsEntity, RhsT>
// ---------------------------------------------------------------------------
// Models a cross-table column equality used in JOIN ON clauses.
// e.g.  eq(&Post::user_id, &User::id)
// Both sides are resolved to qualified column names at serialisation time;
// no parameter placeholder is emitted.
template<typename LhsEntity, typename LhsT,
         typename RhsEntity, typename RhsT>
struct column_eq_ref {
    LhsT LhsEntity::* lhs;
    RhsT RhsEntity::* rhs;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

// Deduces the column_ref type for a raw member pointer MPtr.
// Used by normalize_select_col and the select() factory.
template<typename MPtr>
struct member_to_col_ref;

template<typename Entity, typename T>
struct member_to_col_ref<T Entity::*> {
    using type = column_ref<Entity, T>;
};

template<typename MPtr>
using member_to_col_ref_t = typename member_to_col_ref<MPtr>::type;

// Appends a single type to a std::tuple type.
// Used by select_query::order_by and select_query::inner_join / left_join.
template<typename Tuple, typename Elem>
using tuple_append_t = decltype(
    std::tuple_cat(std::declval<Tuple>(), std::declval<std::tuple<Elem>>())
);

} // namespace detail

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

namespace detail {

template<typename T> struct is_column_ref_impl : std::false_type {};
template<typename E, typename T>
struct is_column_ref_impl<column_ref<E, T>> : std::true_type {};

template<typename T> struct is_literal_impl : std::false_type {};
template<typename T>
struct is_literal_impl<literal<T>> : std::true_type {};

} // namespace detail

template<typename T>
concept is_column_ref = detail::is_column_ref_impl<std::remove_cvref_t<T>>::value;

template<typename T>
concept is_literal = detail::is_literal_impl<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// Factory: col()
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
constexpr auto col(T Entity::* ptr) -> column_ref<Entity, T>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps a raw member pointer in a column_ref so that predicate
     *   factories (eq, gt, like, …) can accept either col() or raw member
     *   pointers uniformly via overloads.
     *
     * Step 1 — Construct column_ref<Entity, T>{ptr} and return it.
     *
     * Key types involved:
     *   - column_ref<Entity, T>: the compile-time column descriptor above.
     *
     * Preconditions:
     *   - ptr is a valid (possibly null, but typically non-null) member pointer.
     *
     * Postconditions:
     *   - Returned column_ref::ptr == ptr.
     *
     * Pitfalls:
     *   - None; this is a trivial aggregate construction.
     *
     * Hint:
     *   return column_ref<Entity, T>{ptr};
     */
}

// ---------------------------------------------------------------------------
// Factory: lit()
// ---------------------------------------------------------------------------

template<typename T>
constexpr auto lit(T&& v) -> literal<std::remove_cvref_t<T>>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Wraps an rvalue or lvalue into a literal<T>, stripping cv-refs so
     *   the stored type is always a plain value type.
     *
     * Step 1 — Forward v into literal<std::remove_cvref_t<T>>{std::forward<T>(v)}.
     * Step 2 — Return the constructed literal.
     *
     * Key types involved:
     *   - literal<T>: the value-holding node that becomes "$N" at runtime.
     *   - std::remove_cvref_t<T>: ensures the stored type is unqualified.
     *
     * Preconditions:
     *   - T must be copy- or move-constructible.
     *
     * Postconditions:
     *   - Returned literal::value is a copy (or move) of v.
     *
     * Pitfalls:
     *   - Do NOT store a dangling reference: always forward or copy.
     *
     * Hint:
     *   return literal<std::remove_cvref_t<T>>{std::forward<T>(v)};
     */
}

} // namespace atlas
