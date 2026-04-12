#pragma once
//
// SELECT query builder using compile-time method chaining.
//
// select_query_impl is the internal type that accumulates all query state as
// template parameters.  select_query<ColRefs...> is the user-facing alias
// for the initial (no FROM / no WHERE / no ORDER BY / no JOINs) state.
//
// Method chaining pattern:
//   Each builder method takes *this by rvalue reference (&&) and returns a
//   new select_query_impl specialisation with an updated type parameter
//   (e.g. a different WherePred).  The old object is consumed and must not
//   be used after the call.
//
// Serialisation:
//   to_sql(db) and params() are NOT constexpr: they traverse the query state
//   at runtime, accumulate parameter strings, and produce the final SQL
//   and parameter list.  to_sql() requires the storage_t to resolve table
//   aliases and column names.

#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "atlas/query/expr.hpp"
#include "atlas/query/join.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Internal: order_by_clause
// ---------------------------------------------------------------------------

namespace detail {

template<typename Entity, typename T>
struct order_by_clause {
    T Entity::* col;
    bool ascending = true;
};

} // namespace detail

// ---------------------------------------------------------------------------
// select_query_impl — full template state
// ---------------------------------------------------------------------------

template<
    typename ColRefsTuple,                  // std::tuple<column_ref<...>|agg, ...>
    typename FromEntity    = std::monostate,// entity type supplied by .from<E>()
    typename WherePred     = std::monostate,// predicate node or std::monostate
    typename OrderByTuple  = std::tuple<>,  // std::tuple<order_by_clause<...>, ...>
    typename JoinsTuple    = std::tuple<>   // std::tuple<join_clause<...>, ...>
>
struct select_query_impl {

    // Runtime state — stored by value.
    ColRefsTuple                 col_refs;
    WherePred                    where_pred;
    OrderByTuple                 order_cols;
    JoinsTuple                   joins;
    std::optional<std::size_t>   limit_n;
    std::optional<std::size_t>   offset_n;

    // -----------------------------------------------------------------------
    // .from<Entity>()
    // -----------------------------------------------------------------------
    // Stamps the FROM entity type.  Typically called immediately after
    // select() when the entity cannot be deduced from the column list alone
    // (e.g. COUNT(*), or multi-entity column lists).
    template<typename Entity>
    auto from() &&
        -> select_query_impl<ColRefsTuple, Entity, WherePred, OrderByTuple, JoinsTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Transfers all accumulated state into a new select_query_impl that
         *   fixes the FromEntity type to Entity.
         *
         * Step 1 — Construct the new query type with the same col_refs,
         *           where_pred, order_cols, joins, limit_n, offset_n.
         * Step 2 — Move all fields from *this into the new instance.
         * Step 3 — Return the new instance.
         *
         * Key types involved:
         *   - select_query_impl<..., Entity, ...>: the returned type with
         *     FromEntity fixed.
         *
         * Preconditions:
         *   - from() should only be called once per query chain.
         *   - Entity must be registered in the storage_t passed to to_sql().
         *
         * Postconditions:
         *   - The returned query's FromEntity template parameter is Entity.
         *
         * Pitfalls:
         *   - Move, do not copy, all member fields to avoid expensive copies
         *     of large tuple/optional states.
         *
         * Hint:
         *   return {std::move(col_refs), std::move(where_pred),
         *           std::move(order_cols), std::move(joins),
         *           limit_n, offset_n};
         */
    }

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template<typename Predicate>
        requires is_predicate<Predicate>
    auto where(Predicate&& p) &&
        -> select_query_impl<
               ColRefsTuple, FromEntity,
               std::remove_cvref_t<Predicate>,
               OrderByTuple, JoinsTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Attaches a WHERE predicate to the query.  Replaces any previously
         *   set predicate (the old WherePred type is discarded in the return
         *   type).
         *
         * Step 1 — Forward p into the new query's where_pred field.
         * Step 2 — Move all other fields unchanged.
         * Step 3 — Return the new select_query_impl specialisation.
         *
         * Preconditions:
         *   - p must satisfy is_predicate<Predicate>.
         *
         * Postconditions:
         *   - The returned query's WherePred == std::remove_cvref_t<Predicate>.
         *
         * Pitfalls:
         *   - Calling .where() twice discards the first predicate silently.
         *     Consider static_assert(!std::is_same_v<WherePred, std::monostate>)
         *     to warn the developer if desired.
         *
         * Hint:
         *   return {std::move(col_refs), std::forward<Predicate>(p),
         *           std::move(order_cols), std::move(joins), limit_n, offset_n};
         */
    }

    // -----------------------------------------------------------------------
    // .order_by(member_ptr, ascending = true)
    // -----------------------------------------------------------------------
    template<typename Entity, typename T>
    auto order_by(T Entity::* col, bool ascending = true) &&
        -> select_query_impl<
               ColRefsTuple, FromEntity, WherePred,
               detail::tuple_append_t<
                   OrderByTuple,
                   detail::order_by_clause<Entity, T>>,
               JoinsTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Appends an ORDER BY clause.  Multiple calls chain additional
         *   columns; they are serialised in call order.
         *
         * Step 1 — Construct a detail::order_by_clause<Entity, T>{col, ascending}.
         * Step 2 — Append it to order_cols via std::tuple_cat.
         * Step 3 — Construct and return the new select_query_impl with the
         *           extended OrderByTuple.
         *
         * Key types involved:
         *   - detail::order_by_clause<Entity, T>: pairs the member pointer
         *     with the ascending flag.
         *   - detail::tuple_append_t<OrderByTuple, ...>: the new tuple type.
         *
         * Preconditions:
         *   - col must be a valid member pointer registered in a table_t for Entity.
         *
         * Postconditions:
         *   - The returned query has one more element in its OrderByTuple.
         *
         * Hint:
         *   auto new_order = std::tuple_cat(
         *       std::move(order_cols),
         *       std::make_tuple(detail::order_by_clause<Entity,T>{col, ascending}));
         *   return {std::move(col_refs), std::move(where_pred),
         *           std::move(new_order), std::move(joins), limit_n, offset_n};
         */
    }

    // -----------------------------------------------------------------------
    // .limit(n)
    // -----------------------------------------------------------------------
    auto limit(std::size_t n) &&
        -> select_query_impl<ColRefsTuple, FromEntity, WherePred, OrderByTuple, JoinsTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Sets the LIMIT clause.  The return type is the same specialisation
         *   so the limit can be chained freely before or after other methods.
         *
         * Step 1 — Set limit_n = n on the current object.
         * Step 2 — Move *this into a new instance and return it.
         *
         * Preconditions:
         *   - n > 0 is not enforced at compile time; the developer may add a
         *     runtime assertion.
         *
         * Hint:
         *   limit_n = n; return std::move(*this);
         */
    }

    // -----------------------------------------------------------------------
    // .offset(n)
    // -----------------------------------------------------------------------
    auto offset(std::size_t n) &&
        -> select_query_impl<ColRefsTuple, FromEntity, WherePred, OrderByTuple, JoinsTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Sets the OFFSET clause.
         *
         * Step 1 — Set offset_n = n.
         * Step 2 — Return std::move(*this).
         *
         * Hint: identical to limit().
         */
    }

    // -----------------------------------------------------------------------
    // .inner_join<RhsEntity>(on_predicate)
    // -----------------------------------------------------------------------
    template<typename RhsEntity, typename OnPredicate>
    auto inner_join(OnPredicate&& on) &&
        -> select_query_impl<
               ColRefsTuple, FromEntity, WherePred, OrderByTuple,
               detail::tuple_append_t<
                   JoinsTuple,
                   join_clause<RhsEntity,
                               std::remove_cvref_t<OnPredicate>,
                               join_kind::inner>>>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Appends an INNER JOIN clause for RhsEntity with the given ON
         *   predicate.
         *
         * Step 1 — Construct join_clause<RhsEntity, OnPred, join_kind::inner>
         *           {std::forward<OnPredicate>(on)}.
         * Step 2 — Append to joins via std::tuple_cat.
         * Step 3 — Move all other fields and return the new query.
         *
         * Key types involved:
         *   - join_clause<RhsEntity, OnPredicate, join_kind::inner>: the JOIN
         *     descriptor (join.hpp).
         *   - detail::tuple_append_t: appends to JoinsTuple.
         *
         * Preconditions:
         *   - RhsEntity must be registered in the storage_t passed to to_sql().
         *   - on must be a valid predicate (typically an eq_expr comparing two
         *     column_refs).
         *
         * Hint:
         *   using JoinT = join_clause<RhsEntity,
         *                             std::remove_cvref_t<OnPredicate>,
         *                             join_kind::inner>;
         *   auto new_joins = std::tuple_cat(
         *       std::move(joins),
         *       std::make_tuple(JoinT{std::forward<OnPredicate>(on)}));
         *   return {std::move(col_refs), std::move(where_pred),
         *           std::move(order_cols), std::move(new_joins), limit_n, offset_n};
         */
    }

    // -----------------------------------------------------------------------
    // .left_join<RhsEntity>(on_predicate)
    // -----------------------------------------------------------------------
    template<typename RhsEntity, typename OnPredicate>
    auto left_join(OnPredicate&& on) &&
        -> select_query_impl<
               ColRefsTuple, FromEntity, WherePred, OrderByTuple,
               detail::tuple_append_t<
                   JoinsTuple,
                   join_clause<RhsEntity,
                               std::remove_cvref_t<OnPredicate>,
                               join_kind::left>>>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Appends a LEFT JOIN clause for RhsEntity.
         *
         * Hint: identical to inner_join, use join_kind::left.
         */
    }

    // -----------------------------------------------------------------------
    // .to_sql(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::string
    to_sql(const storage_t<Tables...>& db) const
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Serialises the entire SELECT query to a parameterised SQL string.
         *   Delegates to sql_serialize.hpp helpers.
         *
         * Step 1 — Create a serialize_context ctx{}.
         * Step 2 — Build the SELECT column list:
         *           iterate col_refs tuple with std::apply; for each element call
         *           serialize_column_ref (or serialize_aggregate if is_aggregate).
         *           Join with ", ".
         * Step 3 — Determine the primary table name and alias from
         *           db.get_table<FromEntity>().name.  Use first letter of name
         *           lowercased as alias (e.g. "users" → alias "u").
         * Step 4 — Append "FROM <table> <alias>".
         * Step 5 — Serialise JoinsTuple: for each join_clause in joins, emit
         *           "INNER JOIN <rhs_table> <alias> ON <predicate>".
         * Step 6 — If WherePred != std::monostate, append
         *           "WHERE " + serialize_predicate(where_pred, db, ctx).
         * Step 7 — Serialise OrderByTuple: "ORDER BY col [ASC|DESC], …".
         * Step 8 — If limit_n has value, append "LIMIT N".
         * Step 9 — If offset_n has value, append "OFFSET N".
         *
         * Key types involved:
         *   - serialize_context: accumulates $N params (sql_serialize.hpp).
         *   - serialize_column_ref<Entity, T>: resolves "alias.column_name".
         *   - serialize_predicate<Pred>: recursively serialises the WHERE tree.
         *
         * Preconditions:
         *   - FromEntity must not be std::monostate (call .from<E>() first).
         *   - All entities referenced in col_refs/joins must be in storage.
         *
         * Postconditions:
         *   - Returns a valid parameterised SQL string; ctx.params holds the
         *     ordered bound values (same order as params() return value).
         *
         * Pitfalls:
         *   - Table alias collisions when joining two tables of the same type
         *     are not handled; emit a compile-time warning comment if desired.
         *   - std::monostate check: use if constexpr (!std::is_same_v<WherePred,
         *     std::monostate>) to skip the WHERE clause.
         *
         * Hint:
         *   Build the result with std::string sql = "SELECT "; then += each part.
         *   Use a local serialize_context; params() reuses the same traversal.
         */
    }

    // -----------------------------------------------------------------------
    // .params()
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<std::string> params() const
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Returns the ordered list of bound parameter values that correspond
         *   to the $1, $2, … placeholders emitted by to_sql().
         *
         * Step 1 — Create a dummy serialize_context ctx{} (same as to_sql).
         * Step 2 — Re-run the serialisation traversal (or share a private
         *           helper with to_sql to avoid code duplication).
         * Step 3 — Return ctx.params.
         *
         * Preconditions:
         *   - to_sql() and params() must agree on parameter order; implement
         *     both via a shared private helper to guarantee consistency.
         *
         * Postconditions:
         *   - ctx.params.size() == number of $N placeholders in to_sql() output.
         *
         * Pitfalls:
         *   - If to_sql() and params() traverse the AST in different orders the
         *     parameter indices will be wrong. Share implementation.
         *
         * Hint:
         *   Consider a private method
         *     std::pair<std::string, std::vector<std::string>>
         *     build(const storage_t<Tables...>& db) const;
         *   called by both to_sql and params.
         *   But params() has no storage_t argument — store table names in the
         *   query at .from<E>() time, or require callers to pass a db reference
         *   to params() as well (change the signature if needed).
         */
    }
};

// ---------------------------------------------------------------------------
// User-facing alias: select_query<ColRefs...>
// Initial state: no FROM, no WHERE, no ORDER BY, no JOINs.
// ---------------------------------------------------------------------------

template<typename... ColRefs>
using select_query = select_query_impl<
    std::tuple<ColRefs...>,
    std::monostate,
    std::monostate,
    std::tuple<>,
    std::tuple<>
>;

// ---------------------------------------------------------------------------
// Internal: normalise select() arguments
// ---------------------------------------------------------------------------

namespace detail {

// Member pointer → column_ref (wrap).
template<typename Entity, typename T>
constexpr auto normalize_select_col(T Entity::* ptr)
    -> column_ref<Entity, T>
{
    /*
     * IMPLEMENTATION GUIDE:
     * Step 1 — return column_ref<Entity, T>{ptr};
     */
}

// Already-wrapped type (aggregate node, column_ref): pass through.
template<typename T>
    requires (!std::is_member_pointer_v<std::remove_cvref_t<T>>)
constexpr auto normalize_select_col(T&& t)
    -> std::remove_cvref_t<T>
{
    /*
     * IMPLEMENTATION GUIDE:
     * Step 1 — return std::forward<T>(t);
     */
}

} // namespace detail

// ---------------------------------------------------------------------------
// Factory: atlas::select(args...)
// ---------------------------------------------------------------------------

template<typename... Args>
constexpr auto select(Args&&... args)
    -> select_query<
           decltype(detail::normalize_select_col(std::forward<Args>(args)))...>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Creates an empty select_query with the given column/aggregate
     *   references.  Raw member pointers are wrapped in column_ref via
     *   detail::normalize_select_col; aggregate nodes pass through unchanged.
     *
     * Step 1 — Normalise each argument with normalize_select_col.
     * Step 2 — Pack the normalised refs into a std::tuple.
     * Step 3 — Return a default-constructed select_query_impl with:
     *           col_refs = std::tuple{normalize_select_col(args)...},
     *           all other fields default-initialised.
     *
     * Key types involved:
     *   - detail::normalize_select_col: adapter that handles both raw member
     *     pointers and aggregate/column_ref types uniformly.
     *
     * Preconditions:
     *   - At least one argument must be provided.
     *   - Each argument must be either a member pointer or an is_aggregate
     *     or is_column_ref type.
     *
     * Postconditions:
     *   - Returned select_query carries all provided column/aggregate refs.
     *
     * Pitfalls:
     *   - Do not call normalize_select_col twice; compute the tuple in one pass.
     *
     * Hint:
     *   return select_query<...>{
     *       std::make_tuple(detail::normalize_select_col(std::forward<Args>(args))...),
     *       {}, {}, {}, {}, std::nullopt, std::nullopt
     *   };
     */
}

} // namespace atlas
