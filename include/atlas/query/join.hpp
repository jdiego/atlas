#pragma once
//
// JOIN clause types for the query builder.
//
// join_clause<RhsSource, OnPredicate, Kind> carries the ON predicate and the
// join kind as a non-type template parameter. RhsSource is either a plain
// entity type (Tag implicitly void) or a table_instance<Entity, Tag> when a
// self-join requires an explicit alias. The clause derives rhs_entity_type
// and tag_type from RhsSource via source_traits, so callers never need to
// pass the tag as a separate template parameter.
//
// To extend select_query with join support, the inner_join / left_join methods
// produce a new select_query specialisation. select_query_impl names that
// transition with aliases such as inner_join_result_t / left_join_result_t
// rather than exposing tuple plumbing directly in the public signature.
//
// Internally, the new JoinsTuple is:
//
//   detail::tuple_append_t<
//       OldJoinsTuple,
//       join_clause<JoinSource, OnPredicate, Kind>
//   >
//
// where JoinSource is qualify_source_t<RhsSource, RhsTag> — i.e. the public
// inner_join<RhsSource, RhsTag>(...) signature is preserved for callers, but
// the canonicalised source flows into join_clause as a single type.
//
// The ON predicate is typically a column_eq_ref (cross-table column equality)
// or any node satisfying is_predicate.

#include <type_traits>

#include "atlas/query/source.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// join_kind
// ---------------------------------------------------------------------------

enum class join_kind { inner, left, right, full };

// ---------------------------------------------------------------------------
// join_clause<RhsSource, OnPredicate, Kind>
// ---------------------------------------------------------------------------

template<typename RhsSource,
         typename OnPredicate,
         join_kind Kind = join_kind::inner>
struct join_clause {
    // RhsSource is expected to already be canonical: either a plain Entity
    // (tag void) or table_instance<Entity, Tag>. Callers obtain that via
    // detail::qualify_source_t<UserSource, UserTag> before instantiating.
    using source_type = detail::canonical_source_t<RhsSource>;

    // The RHS entity type drives the table name lookup in storage_t.
    using rhs_entity_type = detail::source_entity_t<source_type>;

    // Tag type for self-join disambiguation. When void, the serializer
    // falls back to the default first-letter alias scheme. When set to a
    // user-defined tag type (e.g. struct mgr { ... alias = "m"; }), the
    // alias is used to qualify the joined table in the ON and SELECT
    // clauses.
    using tag_type = detail::source_tag_t<source_type>;

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

template<typename Src, typename P, join_kind K>
struct is_join_clause_impl<join_clause<Src, P, K>> : std::true_type {};

} // namespace detail

template<typename T>
concept is_join_clause = detail::is_join_clause_impl<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// NOTE: select_query join methods
// ---------------------------------------------------------------------------
//
// inner_join<RhsSource>(on) and left_join<RhsSource>(on) are declared as
// member functions of select_query_impl (select.hpp). They follow this
// pattern:
//
//   template<typename RhsSource, typename RhsTag = void, typename OnPredicate>
//   auto inner_join(OnPredicate&& on) &&
//       -> inner_join_result_t<RhsSource, OnPredicate, RhsTag>;
//
//   template<typename RhsSource, typename RhsTag = void, typename OnPredicate>
//   auto left_join(OnPredicate&& on) &&
//       -> left_join_result_t<RhsSource, OnPredicate, RhsTag>;
//
// The ON predicate for column-to-column equality in JOINs is typically:
//
//   atlas::eq(&Post::user_id, &User::id)
//
// which creates an eq_expr<column_ref<Post, int32_t>, column_ref<User, int32_t>>.
// serialize_predicate in sql_serialize.hpp detects when both sides are
// column_ref and emits "tbl1.col1 = tbl2.col2" with no parameter placeholder.

} // namespace atlas
