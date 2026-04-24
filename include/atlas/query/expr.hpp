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
template <typename Entity, typename T, typename Tag = void>
struct column_ref {
    T Entity::*ptr;
};

// CTAD guide for direct construction from a raw member pointer.
// Keeps direct construction aligned with the col() factory:
//   column_ref c{&User::id};   // -> column_ref<User, int, void>
// Tag defaults to void for non-self-join cases; the col<Tag>() factory
// below covers the self-join case by providing an explicit Tag type.
template <typename Entity, typename T>
column_ref(T Entity::*) -> column_ref<Entity, T, void>;

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

// literal_operand_t<V>: canonical alias used in factory return types.
// Strips cv-refs so the stored type is always a plain value type.
template <typename V>
using literal_operand_t = literal<std::remove_cvref_t<V>>;


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


// Detect column_ref early; needed by make_rhs dispatch below.
// The canonical is_column_ref_impl trait lives later in this header and
// defines the is_column_ref concept — this variable template is a local
// helper to keep the ordering simple without forward-declaration gymnastics.
template<typename T> inline constexpr bool is_col_ref_v = false;
template<typename E, typename T, typename Tag>
inline constexpr bool is_col_ref_v<column_ref<E, T, Tag>> = true;

// ---------------------------------------------------------------------------
// rhs_operand<V> — maps a raw RHS argument type to the correct AST node type
//
// Primary template: any non-member-pointer type V -> literal<remove_cvref_t<V>>
// Specialisations:
//   - member pointer T Entity::*          ->  column_ref<Entity, T> (Tag=void)
//   - column_ref<Entity, T, Tag>          ->  column_ref<Entity, T, Tag>
// The column_ref pass-through is what lets self-join ON clauses work:
//   eq(col<emp>(&E::manager_id), col<mgr>(&E::id))
// preserves both tags end-to-end.
// ---------------------------------------------------------------------------

template<typename V>
struct rhs_operand {
    static_assert(!std::is_member_pointer_v<V>,
        "member function pointers are not valid RHS operands; "
        "use a member data pointer or a value"
    );
    using type = literal<std::remove_cvref_t<V>>;
};

template<typename Entity, typename T>
struct rhs_operand<T Entity::*> {
    using type = column_ref<Entity, T>;
};

template<typename Entity, typename T, typename Tag>
struct rhs_operand<column_ref<Entity, T, Tag>> {
    using type = column_ref<Entity, T, Tag>;
};
 
// Alias: resolves to literal<V> or column_ref<Entity,T> depending on V.
// Always apply to the raw (possibly ref-qualified) V before remove_cvref_t
// so that the member pointer specialisation fires correctly.
template<typename V>
using rhs_operand_t = typename rhs_operand<std::remove_cvref_t<V>>::type;
 
// make_rhs(v): constructs the correct AST node for a given RHS value.
// Uses is_member_object_pointer_v (not is_member_pointer_v) to exclude
// member function pointers, which rhs_operand's specialisation also rejects.
template<typename V>
constexpr auto make_rhs(V&& value) -> rhs_operand_t<V>
{
    using U = std::remove_cvref_t<V>;
    if constexpr (std::is_member_object_pointer_v<U>) {
        return rhs_operand_t<V>{value};
    }
    else if constexpr (is_col_ref_v<U>) {
        // Pass-through: preserve the column_ref's Entity/T/Tag. We rebuild
        // from .ptr because column_ref is an aggregate with a single
        // member-pointer field, so brace-init from another column_ref
        // would fail to match the aggregate.
        return rhs_operand_t<V>{value.ptr};
    }
    else {
        return rhs_operand_t<V>{std::forward<V>(value)};
    }
}


} // namespace detail

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct is_column_ref_impl : std::false_type {};
template <typename E, typename T, typename Tag>
struct is_column_ref_impl<column_ref<E, T, Tag>> : std::true_type {};

template <typename T>
struct is_literal_impl : std::false_type {};
template <typename T>
struct is_literal_impl<literal<T>> : std::true_type {};

} // namespace detail

template <typename T>
concept is_column_ref = detail::is_column_ref_impl<std::remove_cvref_t<T>>::value;

template <typename T>
concept is_literal = detail::is_literal_impl<std::remove_cvref_t<T>>::value;


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
// Factory: col<Tag>() — tagged column_ref for self-join
// ---------------------------------------------------------------------------
// Usage:
//   struct mgr { static constexpr std::string_view alias = "m"; };
//   col<mgr>(&Employee::id)  // -> column_ref<Employee, int, mgr>
//
// The Tag is a user-defined empty type that carries instance identity
// through the AST. Serialisation uses Tag::alias if defined, otherwise
// falls back to the default first-letter-of-table-name scheme.
template <typename Tag, typename Entity, typename T>
constexpr auto col(T Entity::*ptr) -> column_ref<Entity, T, Tag> {
    return column_ref<Entity, T, Tag>{ptr};
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
