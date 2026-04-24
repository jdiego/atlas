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

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "atlas/detail/type_utils.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/join.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/query/sql_serialize.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Internal: order_by_clause
// ---------------------------------------------------------------------------

namespace detail {

template <typename Entity, typename T>
struct order_by_clause {
    T Entity::*col;
    bool ascending = true;
};

} // namespace detail

// ---------------------------------------------------------------------------
// select_query_impl — full template state
// ---------------------------------------------------------------------------

template <
    typename Selected,                      // std::tuple<column_ref<...>|agg, ...>
    typename FromEntity = std::monostate,   // entity type supplied by .from<E>()
    typename WherePred = std::monostate,    // predicate node or std::monostate
    typename OrderBy = std::tuple<>,        // std::tuple<order_by_clause<...>, ...>
    typename Joins = std::tuple<>,          // std::tuple<join_clause<...>, ...>
    typename FromTag = void                 // tag for self-join on FROM entity
>
struct select_query_impl {

    // Runtime state — stored by value.
    Selected selected;
    WherePred where_pred;
    OrderBy order_cols;
    Joins joins;
    std::optional<std::size_t> limit_n;
    std::optional<std::size_t> offset_n;

    // Named state transitions keep public method signatures in query terms
    // rather than exposing tuple plumbing from detail::.
    using self_t = select_query_impl<Selected, FromEntity, WherePred, OrderBy, Joins, FromTag>;

    template <
        typename NewFromEntity = FromEntity,
        typename NewWherePred = WherePred,
        typename NewOrderBy = OrderBy,
        typename NewJoins = Joins,
        typename NewFromTag = FromTag
    >
    using rebind_t = select_query_impl<Selected, NewFromEntity, NewWherePred,  NewOrderBy, NewJoins, NewFromTag>;

    template <typename Entity, typename Tag = void>
    using with_from_t = rebind_t<Entity, WherePred, OrderBy, Joins, Tag>;

    template <typename Predicate>
    using with_where_t = rebind_t<FromEntity, std::remove_cvref_t<Predicate>, OrderBy, Joins, FromTag>;

    template <typename Entity, typename T>
    using with_order_by_t = rebind_t<
        FromEntity,
        WherePred,
        detail::tuple_append_t<OrderBy, detail::order_by_clause<Entity, T>>,
        Joins,
        FromTag
    >;

    template <typename JoinClause>
    using with_join_clause_t = rebind_t<
        FromEntity,
        WherePred,
        OrderBy,
        detail::tuple_append_t<Joins, JoinClause>,
        FromTag
    >;

    template <typename RhsEntity, typename OnPredicate, join_kind Kind, typename RhsTag = void>
    using with_join_t = with_join_clause_t<join_clause<RhsEntity, std::remove_cvref_t<OnPredicate>, Kind, RhsTag>>;

    template <typename RhsEntity, typename OnPredicate, typename RhsTag = void>
    using inner_join_result_t = with_join_t<RhsEntity, OnPredicate, join_kind::inner, RhsTag>;

    template <typename RhsEntity, typename OnPredicate, typename RhsTag = void>
    using left_join_result_t = with_join_t<RhsEntity, OnPredicate, join_kind::left, RhsTag>;

    // -----------------------------------------------------------------------
    // .from<Entity>()
    // -----------------------------------------------------------------------
    // Stamps the FROM entity type.  Typically called immediately after
    // select() when the entity cannot be deduced from the column list alone
    // (e.g. COUNT(*), or multi-entity column lists).
    template <typename Entity, typename Tag = void>
    auto from() && -> with_from_t<Entity, Tag> {
        /*
         * What this function does:
         *   Transfers all accumulated state into a new select_query_impl that
         *   fixes the FromEntity type to Entity.
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
         */
        return {
            std::move(selected), 
            std::move(where_pred), 
            std::move(order_cols), 
            std::move(joins), 
            limit_n, offset_n
        };
    }

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template <typename Predicate> 
    requires is_predicate<Predicate>
    auto where(Predicate&& pred) && -> with_where_t<Predicate> {
        /*
         * What this function does:
         *   Attaches a WHERE predicate to the query.  Replaces any previously
         *   set predicate (the old WherePred type is discarded in the return
         *   type).
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
         */
        return {
            std::move(selected), 
            std::forward<Predicate>(pred),
            std::move(order_cols), 
            std::move(joins), 
            limit_n, offset_n
        };
    }

    // -----------------------------------------------------------------------
    // .order_by(member_ptr, ascending = true)
    // -----------------------------------------------------------------------
    template <typename Entity, typename T>
    auto order_by(T Entity::*col, bool ascending = true) && -> with_order_by_t<Entity, T> {
        /*
         *
         * What this function does:
         *   Appends an ORDER BY clause.  Multiple calls chain additional
         *   columns; they are serialised in call order.
         *
         * Key types involved:
         *   - detail::order_by_clause<Entity, T>: pairs the member pointer
         *     with the ascending flag.
         *   - with_order_by_t<Entity, T>: the returned query state.
         *
         * Preconditions:
         *   - col must be a valid member pointer registered in a table_t for Entity.
         *
         * Postconditions:
         *   - The returned query has one more element in its OrderBy.
         *
         */
        auto new_order = std::tuple_cat(
            std::move(order_cols),
            std::make_tuple(detail::order_by_clause<Entity,T>{col, ascending})
        );
        return {
            std::move(selected), 
            std::move(where_pred),
            std::move(new_order), 
            std::move(joins), 
            limit_n, offset_n
        };
    }

    // -----------------------------------------------------------------------
    // .limit(n)
    // -----------------------------------------------------------------------
    auto limit(std::size_t n) && -> self_t {
        /*
         * What this function does:
         *   Sets the LIMIT clause.  The return type is the same specialisation
         *   so the limit can be chained freely before or after other methods.
         *
         * Preconditions:
         *   - n > 0 is not enforced at compile time; the developer may add a
         *     runtime assertion.
         *
         */
        limit_n = n;
        return std::move(*this);
    }

    // -----------------------------------------------------------------------
    // .offset(n)
    // -----------------------------------------------------------------------
    auto offset(std::size_t n) && -> self_t {
        /*
         * What this function does:
         *   Sets the OFFSET clause.
         *
         */
        offset_n = n;
        return std::move(*this);
    }

    // -----------------------------------------------------------------------
    // .inner_join<RhsEntity>(on_predicate)
    // -----------------------------------------------------------------------
    template <typename RhsEntity, typename RhsTag = void, typename OnPredicate>
    auto inner_join(OnPredicate &&on) && -> inner_join_result_t<RhsEntity, OnPredicate, RhsTag>
    {
        /*
         * What this function does:
         *   Appends an INNER JOIN clause for RhsEntity with the given ON
         *   predicate.
         *
         * Key types involved:
         *   - join_clause<RhsEntity, OnPredicate, join_kind::inner>: the JOIN
         *     descriptor (join.hpp).
         *   - inner_join_result_t<...>: the returned query state.
         *
         * Preconditions:
         *   - RhsEntity must be registered in the storage_t passed to to_sql().
         *   - on must be a valid predicate (typically an eq_expr comparing two
         *     column_refs).
         *
         */
        using JoinT = join_clause<RhsEntity, std::remove_cvref_t<OnPredicate>, join_kind::inner, RhsTag>;
        auto new_join = std::tuple_cat(
            std::move(joins),
            std::make_tuple(JoinT{std::forward<OnPredicate>(on)})
        );
        return {
            std::move(selected), 
            std::move(where_pred),
            std::move(order_cols), 
            std::move(new_join), 
            limit_n, offset_n
        };

    }

    // -----------------------------------------------------------------------
    // .left_join<RhsEntity>(on_predicate)
    // -----------------------------------------------------------------------
    template <typename RhsEntity, typename RhsTag = void, typename OnPredicate>
    auto left_join(OnPredicate &&on) && -> left_join_result_t<RhsEntity, OnPredicate, RhsTag> {
        /*
         * What this function does:
         *   Appends a LEFT JOIN clause for RhsEntity with the given ON
         *   predicate.
         *
         * Key types involved:
         *   - join_clause<RhsEntity, OnPredicate, join_kind::left>: the JOIN
         *     descriptor (join.hpp).
         *   - left_join_result_t<...>: the returned query state.
         *
         * Preconditions:
         *   - RhsEntity must be registered in the storage_t passed to to_sql().
         *   - on must be a valid predicate (typically an eq_expr comparing two
         *     column_refs).
         */
        using JoinT = join_clause<RhsEntity, std::remove_cvref_t<OnPredicate>, join_kind::left, RhsTag>;
        auto new_join = std::tuple_cat(
            std::move(joins),
            std::make_tuple(JoinT{std::forward<OnPredicate>(on)})
        );
        return {
            std::move(selected),
            std::move(where_pred),
            std::move(order_cols),
            std::move(new_join),
            limit_n, offset_n
        };
    }

    // -----------------------------------------------------------------------
    // .to_sql(db)
    // -----------------------------------------------------------------------
    template <typename... Tables>
    [[nodiscard]] std::string to_sql(const storage_t<Tables...> &db) const {
        /*
         *
         * What this function does:
         *   Serialises the entire SELECT query to a parameterised SQL string.
         *   Delegates to sql_serialize.hpp helpers. 
         *
         * Key types involved:
         *   - serialize_context: accumulates $N params (sql_serialize.hpp).
         *   - serialize_column_ref<Entity, T>: resolves "alias.column_name".
         *   - serialize_predicate<Pred>: recursively serialises the WHERE tree.
         *
         * Preconditions:
         *   - FromEntity must not be std::monostate (call .from<E>() first).
         *   - All entities referenced in selected/joins must be in storage.
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
         */
        static_assert(!std::is_same_v<FromEntity, std::monostate>,
            "to_sql function requires .from<Entity>() before serialisation");

        auto alias_for = []<typename Tag>(std::string_view table_name) -> std::string {
            if constexpr (!std::is_void_v<Tag> && detail::has_alias<Tag>) 
            {
                return std::string(Tag::alias);
            } 
            else 
            {
                return std::string{
                    static_cast<char>(std::tolower(static_cast<unsigned char>(table_name.front())))
                };
            }
        };

        serialize_context ctx{};
        std::string sql = "SELECT ";
        static_assert(std::tuple_size_v<Selected> > 0,
            "select_query::to_sql() requires at least one selected expression");
        // `selected` is a heterogeneous tuple of SELECT expressions
        // (`column_ref` and aggregate nodes). `std::apply` expands that tuple
        // in declaration order so each element can be serialized without type
        // erasure. `append_select` then uses `if constexpr` to dispatch at
        // compile time to `serialize_column_ref()` or `serialize_aggregate()`,
        // inserting ", " between items as needed.
        if constexpr (std::tuple_size_v<Selected> > 0) {
            bool first_select = true;
            std::apply([&](const auto &...exprs) {
                auto append_select = [&](const auto &expr) {
                    using Expr = std::remove_cvref_t<decltype(expr)>;
                    if (!first_select) {
                        sql += ", ";
                    }
                    if constexpr (is_aggregate<Expr>) {
                        sql += serialize_aggregate(expr, db);
                    } 
                    else if constexpr (is_column_ref<Expr>) {
                        sql += serialize_column_ref(expr, db);
                    } 
                    else {
                        static_assert(detail::always_false<Expr>, "select_query::to_sql(): unsupported SELECT expression");
                    }

                    first_select = false;
                };

                (append_select(exprs), ...);
            }, selected);
        }
        // Resolve the FROM table once; its metadata drives both the clause
        // itself and the default alias fallback used by column serialization.
        const auto &from_table = db.template get_table<FromEntity>();
        sql += " FROM ";
        sql += from_table.name;
        sql += " ";
        sql += alias_for.template operator()<FromTag>(from_table.name);

        // Emit JOINs before WHERE so placeholders produced by ON predicates
        // occupy the leading slots in ctx, matching params() traversal order.
        if constexpr (std::tuple_size_v<Joins> > 0) {
            std::apply([&](const auto &...join_nodes) {
                auto append_join = [&](const auto &join_node) {
                    using Join = std::remove_cvref_t<decltype(join_node)>;
                    using JoinEntity = typename Join::rhs_entity_type;
                    using JoinTag = typename Join::tag_type;
                    const auto& join_table = db.template get_table<JoinEntity>();

                    sql += " ";
                    if constexpr (Join::kind == join_kind::inner) {
                        sql += "INNER JOIN ";
                    } 
                    else if constexpr (Join::kind == join_kind::left) {
                        sql += "LEFT JOIN ";
                    } 
                    else if constexpr (Join::kind == join_kind::right) {
                        sql += "RIGHT JOIN ";
                    } 
                    else if constexpr (Join::kind == join_kind::full) {
                        sql += "FULL JOIN ";
                    } 
                    else {
                        static_assert(detail::always_false<Join>, "select_query::to_sql(): unsupported join kind");
                    }

                    sql += join_table.name;
                    sql += " ";
                    sql += alias_for.template operator()<JoinTag>(join_table.name);
                    sql += " ON ";
                    sql += serialize_predicate(join_node.on, db, ctx);
                };

                (append_join(join_nodes), ...);
            }, joins);
        }
        // WHERE reuses predicate serialization so SQL emission and parameter
        // collection stay coupled through the same ctx state.
        if constexpr (!std::is_same_v<WherePred, std::monostate>) {
            sql += " WHERE ";
            sql += serialize_predicate(where_pred, db, ctx);
        }

        // ORDER BY stores raw member pointers; wrap each one in a temporary
        // column_ref so it can reuse the standard qualified-column serializer.
        if constexpr (std::tuple_size_v<OrderBy> > 0) 
        {
            sql += " ORDER BY ";
            bool first_order = true;

            std::apply([&](const auto &...clauses) {
                auto append_order = [&](const auto &clause) {
                    if (!first_order) {
                        sql += ", ";
                    }

                    sql += serialize_column_ref(column_ref{clause.col}, db);
                    sql += (clause.ascending ? " ASC" : " DESC");
                    first_order = false;
                };

                (append_order(clauses), ...);
            }, order_cols);
        }

        // LIMIT and OFFSET shape the statement and therefore render inline
        // instead of consuming placeholder slots in ctx.
        if (limit_n.has_value()) {
            sql += " LIMIT ";
            sql += std::to_string(*limit_n);
        }

        if (offset_n.has_value()) {
            sql += " OFFSET ";
            sql += std::to_string(*offset_n);
        }

        return sql;
    }

    // -----------------------------------------------------------------------
    // .params()
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<std::string> params() const {
        /*
         * What this function does:
         *   Returns the ordered list of bound parameter values that correspond
         *   to the $1, $2, … placeholders emitted by to_sql().
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
        */
        serialize_context ctx{};
        std::apply([&](const auto &...join_nodes) {
            (detail::collect_predicate_params(join_nodes.on, ctx), ...);
        }, joins);
        if constexpr (not std::is_same_v<WherePred, std::monostate>) 
        {
            detail::collect_predicate_params(where_pred, ctx);
        }
        return std::move(ctx.params);
    }
};

// ---------------------------------------------------------------------------
// User-facing alias: select_query<ColRefs...>
// Initial state: no FROM, no WHERE, no ORDER BY, no JOINs.
// ---------------------------------------------------------------------------

template <typename... Selected>
using select_query = select_query_impl<
    std::tuple<Selected...>, 
    std::monostate, 
    std::monostate, 
    std::tuple<>, 
    std::tuple<>, 
    void
>;

// ---------------------------------------------------------------------------
// Internal: normalise select() arguments
// ---------------------------------------------------------------------------

namespace detail {

// Member pointer → column_ref (wrap).
template <typename Entity, typename T>
constexpr auto normalize_select_col(T Entity::*ptr) -> atlas::column_ref<Entity, T> {
    return atlas::column_ref<Entity, T>{ptr};
}

// Already-wrapped type (aggregate node, column_ref): pass through.
template <typename T> requires(!std::is_member_pointer_v<std::remove_cvref_t<T>>)
constexpr auto normalize_select_col(T &&t) -> std::remove_cvref_t<T> {
    return std::forward<T>(t);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Factory: atlas::select(args...)
// ---------------------------------------------------------------------------

template <typename... Args>
constexpr auto select(Args &&...args) -> select_query<decltype(detail::normalize_select_col(std::forward<Args>(args)))...> {
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
     *           selected = std::tuple{normalize_select_col(args)...},
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
    static_assert(sizeof...(Args) > 0,
        "atlas::select() requires at least one column or aggregate");

    return {
        std::make_tuple(detail::normalize_select_col(std::forward<Args>(args))...),
        {},
        {},
        {},
        std::nullopt,
        std::nullopt
    };
}

} // namespace atlas
