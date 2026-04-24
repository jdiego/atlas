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

#include "atlas/query/expr.hpp"
#include "atlas/schema/serde.hpp"
#include <concepts>
#include <ranges>
#include <type_traits>
#include <utility>

namespace atlas {

// ---------------------------------------------------------------------------
// Leaf comparison nodes
// ColRef is always column_ref<Entity, T>; Rhs is literal<V> or column_ref.
// ---------------------------------------------------------------------------

template <typename ColRef, typename Rhs>
struct eq_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct ne_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct lt_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct gt_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct lte_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct gte_expr {
    ColRef lhs;
    Rhs rhs;
};
template <typename ColRef, typename Rhs>
struct like_expr {
    ColRef lhs;
    Rhs rhs;
};

template <typename ColRef>
struct is_null_expr {
    ColRef col;
};
template <typename ColRef>
struct is_not_null_expr {
    ColRef col;
};

// Container is expected to be e.g. std::vector<T> or std::initializer_list<T>.
template <typename ColRef, typename Container>
struct in_expr {
    ColRef col;
    Container values;
};

// ---------------------------------------------------------------------------
// Boolean combinators
// ---------------------------------------------------------------------------

template <typename L, typename R>
struct and_expr {
    L lhs;
    R rhs;
};
template <typename L, typename R>
struct or_expr {
    L lhs;
    R rhs;
};
template <typename P>
struct not_expr {
    P inner;
};

// ---------------------------------------------------------------------------
// Concept: is_predicate
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct is_predicate_impl : std::false_type {};

template <typename C, typename R>
struct is_predicate_impl<eq_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<ne_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<lt_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<gt_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<lte_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<gte_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_predicate_impl<like_expr<C, R>> : std::true_type {};
template <typename C>
struct is_predicate_impl<is_null_expr<C>> : std::true_type {};
template <typename C>
struct is_predicate_impl<is_not_null_expr<C>> : std::true_type {};
template <typename C, typename V>
struct is_predicate_impl<in_expr<C, V>> : std::true_type {};
template <typename L, typename R>
struct is_predicate_impl<and_expr<L, R>> : std::true_type {};
template <typename L, typename R>
struct is_predicate_impl<or_expr<L, R>> : std::true_type {};
template <typename P>
struct is_predicate_impl<not_expr<P>> : std::true_type {};

template <typename T>
struct is_comparison_predicate_impl : std::false_type {};

template <typename C, typename R>
struct is_comparison_predicate_impl<eq_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<ne_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<lt_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<gt_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<lte_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<gte_expr<C, R>> : std::true_type {};
template <typename C, typename R>
struct is_comparison_predicate_impl<like_expr<C, R>> : std::true_type {};

template <typename V>
struct is_in_predicate_impl : std::false_type {};
template <typename C, typename V>
struct is_in_predicate_impl<in_expr<C, V>> : std::true_type {};

template <typename V>
struct is_null_check_impl : std::false_type {};

template <typename C>
struct is_null_check_impl<is_null_expr<C>> : std::true_type {};

template <typename C>
struct is_null_check_impl<is_not_null_expr<C>> : std::true_type {};


template<typename V>
struct is_not_predicate_impl : std::false_type {};

template<typename P>
struct is_not_predicate_impl<not_expr<P>> : std::true_type {};


template <typename T>
struct is_binary_logical_predicate_impl : std::false_type {};

template <typename L, typename R>
struct is_binary_logical_predicate_impl<and_expr<L, R>> : std::true_type {};

template <typename L, typename R>
struct is_binary_logical_predicate_impl<or_expr<L, R>> : std::true_type {};

// ---------------------------------------------------------------------------
// Concepts for predicate factory constraints
template <typename T>
concept serializable = requires(const T &value) {
    { value_serializer<T>::to_string(value) } -> std::convertible_to<std::string>;
};

template <typename Container, typename T>
concept iterable_of =
    std::ranges::range<Container> &&
    std::convertible_to<std::ranges::range_value_t<Container>, T> &&
    serializable<std::ranges::range_value_t<Container>>;

} // namespace detail

template <typename P>
concept is_predicate = detail::is_predicate_impl<std::remove_cvref_t<P>>::value;

template <typename P>
concept is_comparison_predicate = detail::is_comparison_predicate_impl<std::remove_cvref_t<P>>::value;

template <typename P>
concept is_in_predicate = detail::is_in_predicate_impl<std::remove_cvref_t<P>>::value;

template <typename P>
concept is_null_check = detail::is_null_check_impl<std::remove_cvref_t<P>>::value;

template <typename P>
concept is_not_predicate = detail::is_not_predicate_impl<std::remove_cvref_t<P>>::value;


template <typename P>
concept is_binary_logical_predicate = detail::is_binary_logical_predicate_impl<std::remove_cvref_t<P>>::value;

// ---------------------------------------------------------------------------
// Factory: eq
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto eq(T Entity::*col, V &&val) {
    /*
     * Builds an equality predicate node representing "col = val".
     * The right-hand side is normalised to literal<std::remove_cvref_t<V>>,
     * so the payload is always stored as an owning value type.
     *
     * Preconditions:
     *   - col refers to a mapped data member.
     *   - V must be storable in literal<std::remove_cvref_t<V>> and later
     *     serialisable by the SQL serialisation layer.
     *
     * Postconditions:
     *   - Returned node stores the member pointer in lhs.ptr.
     *   - Returned node stores val by value in rhs.value.
     *
     * Key types involved:
     *   - column_ref<Entity, T>: the left-hand column reference.
     *   - literal<V>: the right-hand bound parameter.
     *
     * Pitfalls:
     *   - Forward val, do not copy twice.
     */
    if constexpr (detail::is_col_ref_v<std::remove_cvref_t<V>>) {
        using Rhs = std::remove_cvref_t<V>;
        static_assert(!std::is_same_v<typename Rhs::entity_type, Entity> || std::is_void_v<typename Rhs::tag_type>,
                      "Self-join detected: use col<Tag>() on both sides");
    }
    return eq_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                    detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Tagged column_ref overloads — enable self-join predicates
// ---------------------------------------------------------------------------
// These overloads accept a column_ref<E, T, Tag> directly as the LHS,
// allowing predicates to carry instance identity for self-join
// disambiguation. The raw member-pointer overloads above remain the
// primary API; these kick in when the user writes col<Tag>(&E::member).

template <typename E, typename T, typename Tag, typename V>
constexpr auto eq(column_ref<E, T, Tag> col, V &&val) {
    return eq_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: ne
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto ne(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds an ne_expr node representing "col != val".
     *
     */
    return ne_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                    detail::make_rhs(std::forward<V>(val))};
}
template <typename E, typename T, typename Tag, typename V>
constexpr auto ne(column_ref<E, T, Tag> col, V &&val) {
    return ne_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}
// ---------------------------------------------------------------------------
// Factory: lt
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto lt(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds an lt_expr node representing "col < val".
     *
     */
    return lt_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                    detail::make_rhs(std::forward<V>(val))};
}

template <typename E, typename T, typename Tag, typename V>
constexpr auto lt(column_ref<E, T, Tag> col, V &&val) {
    return lt_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: gt
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto gt(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds a gt_expr node representing "col > val".
     *
     */

    return gt_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                    detail::make_rhs(std::forward<V>(val))};
}

template <typename E, typename T, typename Tag, typename V>
constexpr auto gt(column_ref<E, T, Tag> col, V &&val) {
    return gt_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: lte
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto lte(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds an lte_expr node representing "col <= val".
     *
     */

    return lte_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                     detail::make_rhs(std::forward<V>(val))};
}

template <typename E, typename T, typename Tag, typename V>
constexpr auto lte(column_ref<E, T, Tag> col, V &&val) {
    return lte_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: gte
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto gte(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds a gte_expr node representing "col >= val".
     *
     */

    return gte_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                     detail::make_rhs(std::forward<V>(val))};
}

template <typename E, typename T, typename Tag, typename V>
constexpr auto gte(column_ref<E, T, Tag> col, V &&val) {
    return gte_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: like
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename V>
constexpr auto like(T Entity::*col, V &&val) {
    /*
     * What this function does:
     *   Builds a like_expr node representing "col LIKE val".
     *
     * Pitfalls:
     *   - The pattern string (val) is sent as a PostgreSQL parameter ($N), so
     *     the driver handles quoting; do NOT add extra quotes here.
     *
     */
    return like_expr<column_ref<Entity, T>, detail::rhs_operand_t<V>>{atlas::col(col),
                                                                      detail::make_rhs(std::forward<V>(val))};
}

template <typename E, typename T, typename Tag, typename V>
constexpr auto like(column_ref<E, T, Tag> col, V &&val) {
    return like_expr<column_ref<E, T, Tag>, detail::rhs_operand_t<V>>{col, detail::make_rhs(std::forward<V>(val))};
}

// ---------------------------------------------------------------------------
// Factory: is_null
// ---------------------------------------------------------------------------

template <typename Entity, typename T>
constexpr auto is_null(T Entity::*col) -> is_null_expr<column_ref<Entity, T>> {
    /*
     * What this function does:
     *   Builds an is_null_expr node representing "col IS NULL".
     *
     * Preconditions:
     *   - col is a valid member pointer.
     *
     * Postconditions:
     *   - No parameter placeholder is emitted; IS NULL takes no bound value.
     *
     */
    return {atlas::col(col)};
}

template <typename E, typename T, typename Tag>
constexpr auto is_null(column_ref<E, T, Tag> col) -> is_null_expr<column_ref<E, T, Tag>> {
    return {col};
}

// ---------------------------------------------------------------------------
// Factory: is_not_null
// ---------------------------------------------------------------------------

template <typename Entity, typename T>
constexpr auto is_not_null(T Entity::*col) -> is_not_null_expr<column_ref<Entity, T>> {
    /*
     * What this function does:
     *   Builds an is_not_null_expr node representing "col IS NOT NULL".
     *
     */
    return {atlas::col(col)};
}

template <typename Entity, typename T, typename Tag>
constexpr auto is_not_null(column_ref<Entity, T, Tag> col) -> is_not_null_expr<column_ref<Entity, T, Tag>> {
    return {col};
}

// ---------------------------------------------------------------------------
// Factory: in
// ---------------------------------------------------------------------------

template <typename Entity, typename T, typename Container>
    requires detail::iterable_of<std::remove_cvref_t<Container>, T>
constexpr auto in(T Entity::*col, Container &&vals) -> in_expr<column_ref<Entity, T>, std::remove_cvref_t<Container>> {
    /*
     * What this function does:
     *   Builds an in_expr node representing "col IN ($1, $2, …)".
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
     */
    return {atlas::col(col), std::forward<Container>(vals)};
}

template <typename E, typename T, typename Tag, typename Container>
    requires detail::iterable_of<std::remove_cvref_t<Container>, T>
constexpr auto in(column_ref<E, T, Tag> c, Container &&vals)
    -> in_expr<column_ref<E, T, Tag>, std::remove_cvref_t<Container>> {
    return {c, std::forward<Container>(vals)};
}
// ---------------------------------------------------------------------------
// Factory: and_
// ---------------------------------------------------------------------------

template <typename L, typename R>
    requires is_predicate<L> && is_predicate<R>
constexpr auto and_(L &&lhs, R &&rhs) -> and_expr<std::remove_cvref_t<L>, std::remove_cvref_t<R>> {
    /*
     * What this function does:
     *   Combines two predicate nodes into an and_expr, producing
     *   "(lhs AND rhs)" when serialised.
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
     */
    return {std::forward<L>(lhs), std::forward<R>(rhs)};
}

// ---------------------------------------------------------------------------
// Factory: or_
// ---------------------------------------------------------------------------

template <typename L, typename R>
    requires is_predicate<L> && is_predicate<R>
constexpr auto or_(L &&lhs, R &&rhs) -> or_expr<std::remove_cvref_t<L>, std::remove_cvref_t<R>> {
    /*
     * What this function does:
     *   Combines two predicate nodes into an or_expr, producing
     *   "(lhs OR rhs)" when serialised.
     */
    return {std::forward<L>(lhs), std::forward<R>(rhs)};
}

// ---------------------------------------------------------------------------
// Factory: not_
// ---------------------------------------------------------------------------

template <typename P>
    requires is_predicate<P>
constexpr auto not_(P &&predicate) -> not_expr<std::remove_cvref_t<P>> {
    /*
     * What this function does:
     *   Wraps a predicate in a not_expr, producing "NOT (inner)" when
     *   serialised.
     *
     */
    return {std::forward<P>(predicate)};
}

} // namespace atlas
