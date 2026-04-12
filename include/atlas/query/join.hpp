#pragma once
//
// JOIN clause types for the query builder.
//
// join_clause<RhsEntity, OnPredicate, Kind> carries the ON predicate and the
// join kind as a non-type template parameter. The select_query (in select.hpp)
// accumulates join_clause nodes in its JoinsTuple template parameter and
// serialises them after the primary FROM clause.
//
// To extend select_query with join support, the inner_join / left_join methods
// produce a new select_query specialisation where:
//
//   NewJoinsTuple = detail::tuple_append_t<
//       OldJoinsTuple,
//       join_clause<RhsEntity, OnPredicate, Kind>
//   >
//
// The ON predicate is typically a column_eq_ref (cross-table column equality)
// or any node satisfying is_predicate.

#include <type_traits>

namespace atlas {

// ---------------------------------------------------------------------------
// join_kind
// ---------------------------------------------------------------------------

enum class join_kind { inner, left, right, full };

// ---------------------------------------------------------------------------
// join_clause<RhsEntity, OnPredicate, Kind>
// ---------------------------------------------------------------------------

template<typename RhsEntity,
         typename OnPredicate,
         join_kind Kind = join_kind::inner>
struct join_clause {
    // The RHS entity type drives the table name lookup in storage_t.
    using rhs_entity_type = RhsEntity;

    // The join kind controls the SQL keyword (INNER JOIN, LEFT JOIN, etc.).
    static constexpr join_kind kind = Kind;

    // The ON predicate: serialised as "ON <predicate>".
    OnPredicate on;
};

// ---------------------------------------------------------------------------
// Concept: is_join_clause
// ---------------------------------------------------------------------------

namespace detail {

template<typename T> struct is_join_clause_impl : std::false_type {};

template<typename E, typename P, join_kind K>
struct is_join_clause_impl<join_clause<E, P, K>> : std::true_type {};

} // namespace detail

template<typename T>
concept is_join_clause = detail::is_join_clause_impl<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// NOTE: select_query join methods
// ---------------------------------------------------------------------------
//
// inner_join<RhsEntity>(on) and left_join<RhsEntity>(on) are declared as
// member functions of select_query_impl (select.hpp). They follow this
// pattern:
//
//   template<typename RhsEntity, typename OnPredicate>
//   auto inner_join(OnPredicate&& on) &&
//       -> select_query_impl<
//              ColRefsTuple,
//              FromEntity,
//              WherePred,
//              OrderByTuple,
//              detail::tuple_append_t<
//                  JoinsTuple,
//                  join_clause<RhsEntity,
//                              std::remove_cvref_t<OnPredicate>,
//                              join_kind::inner>
//              >
//          >;
//
//   template<typename RhsEntity, typename OnPredicate>
//   auto left_join(OnPredicate&& on) &&
//       -> select_query_impl<
//              ColRefsTuple,
//              FromEntity,
//              WherePred,
//              OrderByTuple,
//              detail::tuple_append_t<
//                  JoinsTuple,
//                  join_clause<RhsEntity,
//                              std::remove_cvref_t<OnPredicate>,
//                              join_kind::left>
//              >
//          >;
//
// The ON predicate for column-to-column equality in JOINs is typically:
//
//   atlas::eq(&Post::user_id, &User::id)
//
// which creates an eq_expr<column_ref<Post, int32_t>, column_ref<User, int32_t>>.
// serialize_predicate in sql_serialize.hpp detects when both sides are
// column_ref and emits "tbl1.col1 = tbl2.col2" with no parameter placeholder.

} // namespace atlas
