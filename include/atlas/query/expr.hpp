#pragma once
//
// Compile-time expression AST leaf nodes for the query builder.
//
// Every node is a value type: no heap allocation, no virtual dispatch, and no
// inheritance. The AST is later walked by sql_serialize.hpp via a chain of
// if constexpr checks.
//
// Nodes may be created either via semantic factory helpers (e.g. col(), lit())
// or, where supported, via direct construction with CTAD.
//
// NOTE on operand<Entity>:
//   A single variant<column_ref<Entity,T>, literal<V>> cannot be expressed as a
//   simple alias because the two sides of a predicate may have different value
//   types (e.g. column_ref<User,int32_t> vs. literal<std::string>) and
//   different entity types in JOIN conditions. Each predicate node in
//   predicate.hpp therefore carries its lhs/rhs operand types directly as
//   template parameters, making the "operand" concept structural rather than
//   concrete.

#include <tuple>
#include <type_traits>
#include <utility>

namespace atlas {

// ---------------------------------------------------------------------------
// column_ref<Entity, T>
// ---------------------------------------------------------------------------
// References a mapped column via a pointer-to-member.
// The qualified SQL name (e.g. "u.email") is resolved at serialisation time
// by looking up Entity in storage_t and then finding the column whose
// member_ptr matches ptr.
//
// Construction:
//   - Preferred semantic helper: col(&Entity::member)
//   - Also supported directly via CTAD:
//       column_ref c{&Entity::member};
template <typename Entity, typename T>
struct column_ref {
    T Entity::*ptr;
};

// CTAD guide for direct construction from a raw member pointer.
// Keeps direct construction aligned with the col() factory:
//   column_ref c{&User::id};   // -> column_ref<User, int>
template <typename Entity, typename T>
column_ref(T Entity::*) -> column_ref<Entity, T>;

// ---------------------------------------------------------------------------
// literal<T>
// ---------------------------------------------------------------------------
// Wraps a value as a SQL parameter node.
// At serialisation time the stored value is converted to text, appended to the
// params vector, and replaced by a "$N" placeholder in the SQL fragment.
//
// The payload is stored by value, not by reference.
template <typename T>
struct literal {
    T value;
};

// CTAD guide for direct value construction.
// Mirrors the lit() factory by normalising the stored type to a plain owning
// value type with cv/ref qualifiers removed:
//   literal x{some_string};    // -> literal<std::string>
template <typename T>
literal(T) -> literal<std::remove_cvref_t<T>>;

// ---------------------------------------------------------------------------
// column_eq_ref<LhsEntity, LhsT, RhsEntity, RhsT>
// ---------------------------------------------------------------------------
// Models a cross-table column equality used in JOIN ON clauses.
// e.g. eq(&Post::user_id, &User::id)
// Both sides are resolved to qualified column names at serialisation time;
// no parameter placeholder is emitted.
template <typename LhsEntity, typename LhsT, typename RhsEntity, typename RhsT>
struct column_eq_ref {
    LhsT LhsEntity::*lhs;
    RhsT RhsEntity::*rhs;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

// Maps a raw pointer-to-member type MPtr to the corresponding column_ref type.
// Used by normalize_select_col and the select() factory.
template <typename MPtr>
struct member_to_col_ref;

template <typename Entity, typename T>
struct member_to_col_ref<T Entity::*> {
    using type = column_ref<Entity, T>;
};

template <typename MPtr>
using member_to_col_ref_t = typename member_to_col_ref<MPtr>::type;

// Appends a single type to a std::tuple type.
// Used by select_query::order_by and select_query::inner_join / left_join.
template <typename Tuple, typename Elem>
using tuple_append_t = decltype(std::tuple_cat(std::declval<Tuple>(), std::declval<std::tuple<Elem>>()));

} // namespace detail

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct is_column_ref_impl : std::false_type {};
template <typename E, typename T>
struct is_column_ref_impl<column_ref<E, T>> : std::true_type {};

template <typename T>
struct is_literal_impl : std::false_type {};
template <typename T>
struct is_literal_impl<literal<T>> : std::true_type {};

} // namespace detail

template <typename T>
concept is_column_ref = detail::is_column_ref_impl<std::remove_cvref_t<T>>::value;

template <typename T>
concept is_literal = detail::is_literal_impl<std::remove_cvref_t<T>>::value;

template <typename V>
using literal_operand_t = literal<std::remove_cvref_t<V>>;

// ---------------------------------------------------------------------------
// Factory: col()
// ---------------------------------------------------------------------------

template <typename Entity, typename T>
constexpr auto col(T Entity::*ptr) -> column_ref<Entity, T> {
    /*
     * Wraps a raw pointer-to-member as a column_ref node.
     *
     * This is the semantic DSL entry point for referring to a mapped column.
     * It also keeps call sites uniform when predicate builders accept either
     * raw member pointers or already-normalised column_ref nodes.
     *
     * Preconditions:
     *   - ptr is a valid pointer-to-member (possibly null, though typically not).
     *
     * Postconditions:
     *   - Returned column_ref::ptr == ptr.
     */
    return column_ref<Entity, T>{ptr};
}

// ---------------------------------------------------------------------------
// Factory: lit()
// ---------------------------------------------------------------------------

template <typename T>
constexpr auto lit(T &&v) -> literal<std::remove_cvref_t<T>> {
    /*
     * Wraps an lvalue or rvalue as a literal node, stripping cv/ref qualifiers
     * so the stored type is always a plain owning value type.
     *
     * This is the preferred semantic helper for SQL parameter nodes, even
     * though direct construction via CTAD is also supported.
     *
     * Key types involved:
     *   - literal<T>: the value-holding node that becomes "$N" at runtime.
     *   - std::remove_cvref_t<T>: ensures the stored type is unqualified.
     *
     * Preconditions:
     *   - T must be copy- or move-constructible.
     *
     * Postconditions:
     *   - Returned literal::value is a copy or move of v.
     *
     * Pitfalls:
     *   - Do not store references here; always normalise to an owning value.
     */
    return literal<std::remove_cvref_t<T>>{std::forward<T>(v)};
}

} // namespace atlas
