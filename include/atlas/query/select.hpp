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
#include <utility>

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

template <typename Entity, typename T, typename Tag = void>
struct order_by_clause {
    using entity_type = Entity;
    using value_type = T;
    using tag_type = Tag;

    T Entity::*col;
    bool ascending = true;
};

template <typename Tag, typename Entity, typename T>
constexpr auto tagged_column_ref(T Entity::*ptr) -> atlas::column_ref<Entity, T, Tag> {
    return {ptr};
}

} // namespace detail

// ---------------------------------------------------------------------------
// all_columns marker
// ---------------------------------------------------------------------------

template<typename Source>
struct all_columns_t {
    using source_type = detail::canonical_source_t<Source>;
    using entity_type = detail::source_entity_t<source_type>;
    using tag_type = detail::source_tag_t<source_type>;
};

template<typename T>
struct is_all_columns : std::false_type {};

template<typename Source>
struct is_all_columns<all_columns_t<Source>> : std::true_type {};

template<typename T>
inline constexpr bool is_all_columns_v = is_all_columns<T>::value;

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

    template <typename Source, typename Tag = void>
    using with_from_t = rebind_t<
        detail::source_entity_t<detail::qualify_source_t<Source, Tag>>,
        WherePred,
        OrderBy,
        Joins,
        detail::source_tag_t<detail::qualify_source_t<Source, Tag>>
    >;

    template <typename Predicate>
    using with_where_t = rebind_t<FromEntity, std::remove_cvref_t<Predicate>, OrderBy, Joins, FromTag>;

    template <typename Entity, typename T, typename Tag = void>
    using with_order_by_t = rebind_t<
        FromEntity,
        WherePred,
        detail::tuple_append_t<OrderBy, detail::order_by_clause<Entity, T, Tag>>,
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

    template <typename RhsSource, typename OnPredicate, join_kind Kind, typename RhsTag = void>
    using with_join_t = with_join_clause_t<join_clause<
        detail::qualify_source_t<RhsSource, RhsTag>,
        std::remove_cvref_t<OnPredicate>,
        Kind
    >>;

    template <typename RhsSource, typename OnPredicate, typename RhsTag = void>
    using inner_join_result_t = with_join_t<RhsSource, OnPredicate, join_kind::inner, RhsTag>;

    template <typename RhsSource, typename OnPredicate, typename RhsTag = void>
    using left_join_result_t = with_join_t<RhsSource, OnPredicate, join_kind::left, RhsTag>;

    // -----------------------------------------------------------------------
    // .from<Source>()
    // -----------------------------------------------------------------------
    // Stamps the FROM entity type.  Typically called immediately after
    // select() when the entity cannot be deduced from the column list alone
    // (e.g. COUNT(*), or multi-entity column lists).
    template <typename Source, typename Tag = void>
    auto from() && -> with_from_t<Source, Tag> {
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
         * Source forms accepted:
         *   - .from<Entity>()                      (Tag=void; default alias)
         *   - .from<Entity, Tag>()                 (legacy two-arg form)
         *   - .from<table_instance<Entity, Tag>>() (preferred when tagged)
         *   The two-arg form and the table_instance form produce the same
         *   internal type. Mixing both — e.g. .from<table_instance<E,T1>, T2>()
         *   — is rejected at compile time by detail::qualify_source.
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
            std::make_tuple(detail::order_by_clause<Entity, T>{col, ascending})
        );
        return {
            std::move(selected), 
            std::move(where_pred),
            std::move(new_order), 
            std::move(joins), 
            limit_n, offset_n
        };
    }

    // For tagged self-join columns, pass a column_ref produced by
    // col<table_instance<E,Tag>>(...) to the column_ref overload below.
    template <typename Entity, typename T, typename Tag>
    auto order_by(column_ref<Entity, T, Tag> col, bool ascending = true) && -> with_order_by_t<Entity, T, Tag> {
        auto new_order = std::tuple_cat(
            std::move(order_cols),
            std::make_tuple(detail::order_by_clause<Entity, T, Tag>{col.ptr, ascending})
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
    // .inner_join<RhsSource>(on_predicate)
    // -----------------------------------------------------------------------
    template <typename RhsSource, typename RhsTag = void, typename OnPredicate>
    auto inner_join(OnPredicate &&on) && -> inner_join_result_t<RhsSource, OnPredicate, RhsTag>
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
         * Source forms accepted (same rules as .from()):
         *   - .inner_join<Entity>(on)
         *   - .inner_join<Entity, Tag>(on)
         *   - .inner_join<table_instance<Entity, Tag>>(on)
         *   Mixing table_instance with a separate Tag is a compile error.
         *
         */
        using JoinSource = detail::qualify_source_t<RhsSource, RhsTag>;
        using JoinT = join_clause<
            JoinSource,
            std::remove_cvref_t<OnPredicate>,
            join_kind::inner
        >;
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
    // .left_join<RhsSource>(on_predicate)
    // -----------------------------------------------------------------------
    template <typename RhsSource, typename RhsTag = void, typename OnPredicate>
    auto left_join(OnPredicate &&on) && -> left_join_result_t<RhsSource, OnPredicate, RhsTag> {
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
         *
         * Source forms accepted: see .inner_join() above.
         */
        using JoinSource = detail::qualify_source_t<RhsSource, RhsTag>;
        using JoinT = join_clause<
            JoinSource,
            std::remove_cvref_t<OnPredicate>,
            join_kind::left
        >;
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
         *   - Untagged self-joins still collide on the default alias. Use
         *     table_instance<Entity, Tag> or the legacy <Entity, Tag> overloads.
         *   - std::monostate check: use if constexpr (!std::is_same_v<WherePred,
         *     std::monostate>) to skip the WHERE clause.
         */
        static_assert(!std::is_same_v<FromEntity, std::monostate>,
            "to_sql function requires .from<Source>() before serialisation");

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

        static_assert(std::tuple_size_v<Selected> > 0,
            "select_query::to_sql() requires at least one selected expression");

        serialize_context ctx{};
        std::string sql = "SELECT ";

        // `selected` is a heterogeneous tuple of SELECT expressions
        // (column_ref, aggregate nodes, all_columns_t markers). `std::apply`
        // expands the tuple in declaration order so each element can be
        // dispatched at compile time via `if constexpr`. The single
        // `emit_separator` helper centralises comma placement so every leaf
        // (including each column expanded out of an all_columns_t marker)
        // inserts at most one preceding ", ".
        bool first_select = true;
        auto emit_separator = [&] {
            if (!first_select) {
                sql += ", ";
            }
            first_select = false;
        };

        std::apply([&](const auto &...exprs) {
            auto append_select = [&](const auto &expr) {
                using Expr = std::remove_cvref_t<decltype(expr)>;
                if constexpr (is_aggregate<Expr>) {
                    emit_separator();
                    sql += serialize_aggregate(expr, db);
                }
                else if constexpr (is_column_ref<Expr>) {
                    emit_separator();
                    sql += serialize_column_ref(expr, db);
                }
                else if constexpr (is_all_columns_v<Expr>) {
                    using AllEntity = typename Expr::entity_type;
                    using AllTag = typename Expr::tag_type;
                    const auto& table = db.template get_table<AllEntity>();
                    table.for_each_column([&](const auto& col) {
                        emit_separator();
                        sql += serialize_column_ref(detail::tagged_column_ref<AllTag>(col.member_ptr), db);
                    });
                }
                else {
                    static_assert(detail::always_false<Expr>,
                        "select_query::to_sql(): unsupported SELECT expression");
                }
            };

            (append_select(exprs), ...);
        }, selected);
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
                    using Clause = std::remove_cvref_t<decltype(clause)>;
                    using OrderTag = typename Clause::tag_type;
                    if (!first_order) {
                        sql += ", ";
                    }

                    sql += serialize_column_ref(detail::tagged_column_ref<OrderTag>(clause.col), db);
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
    // .params(db)
    // -----------------------------------------------------------------------
    template <typename... Tables>
    [[nodiscard]] std::vector<std::string> params(const storage_t<Tables...>&) const {
        /*
         * Returns the ordered list of bound parameter values matching the
         * $1, $2, … placeholders emitted by to_sql(db).
         *
         * db is accepted for API symmetry with to_sql(db); not used here
         * because JOIN/WHERE predicates store literal values directly.
         *
         * Pitfalls:
         *   - If to_sql() and params() traverse the AST in different orders the
         *     parameter indices will be wrong. Share implementation.
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

template<typename T>
using select_expr_t = decltype(detail::normalize_select_col(std::declval<T>()));

template <typename... Args>
constexpr auto select(Args &&...args) -> select_query<select_expr_t<Args&&>...> 
{
    /*
     * What this function does:
     *   Creates an empty select_query with the given select expressions.
     *   Raw member pointers are wrapped in column_ref via
     *   detail::normalize_select_col; aggregate nodes and all_columns_t<...>
     *   markers pass through unchanged.
     *
     * Key types involved:
     *   - detail::normalize_select_col: adapter that handles raw member
     *     pointers, aggregate/column_ref types, and all_columns_t<...>
     *     markers uniformly.
     *
     * Preconditions:
     *   - At least one argument must be provided.
     *   - Each argument must be either a member pointer, an is_aggregate
     *     type, an is_column_ref type, or an all_columns_t<...> marker.
     *
     * Postconditions:
     *   - Returned select_query carries all provided select expressions.
     *
     * Pitfalls:
     *   - Do not call normalize_select_col twice; compute the tuple in one pass.
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

// ---------------------------------------------------------------------------
// Factory: atlas::all<Entity>()
// ---------------------------------------------------------------------------

template<typename Source, typename Tag = void>
constexpr auto all() -> all_columns_t<detail::qualify_source_t<Source, Tag>> {
    /*
     * What this function does:
     *   Returns the all_columns_t marker so it can be passed to select() as
     *   one of several SELECT expressions, e.g.:
     *
     *     atlas::select(atlas::all<User>(), &Post::title)
     *         .from<User>()
     *         .inner_join<Post>(...)
     *
     *   For a plain "SELECT * FROM Entity" prefer atlas::select_all<Entity>(),
     *   which also stamps the FROM clause for you.
     */
    return {};
}

// ---------------------------------------------------------------------------
// Factory: atlas::select_all<Entity>()
// ---------------------------------------------------------------------------

template<typename Source, typename Tag = void>
constexpr auto select_all() {
    /*
     * What this function does:
     *   Builds a "SELECT * FROM Entity" query in one step. The all_columns_t
     *   marker is expanded during SQL serialization into Entity's full column
     *   list (in table declaration order), and the FromEntity template
     *   parameter is pre-stamped so calling .from<Entity>() is unnecessary.
     *
     *   Further chaining (.where, .order_by, .limit, .offset, .inner_join,
     *   .left_join) is supported and behaves exactly as on a query produced
     *   by atlas::select(...).from<Entity>().
     *
     * Preconditions:
     *   - Entity must be registered in the storage_t passed to to_sql().
     *
     * Postconditions:
     *   - to_sql(db) expands the marker into the mapped column list and
     *     emits "FROM <table> <alias>".
     *
     * Pitfalls:
     *   - Not emitted as a raw SQL '*'. Explicit column expansion keeps
     *     serialization type-aware and preserves deterministic column order.
     *   - Calling .from<Other>() afterwards rebinds FromEntity but leaves
     *     all_columns_t<Entity> in the SELECT list, so to_sql() will still
     *     expand Entity's columns. Don't do that.
     */
    using SourceT = detail::qualify_source_t<Source, Tag>;
    using Entity = detail::source_entity_t<SourceT>;
    using SourceTag = detail::source_tag_t<SourceT>;
    using AllColumns = all_columns_t<SourceT>;
    using query_t = select_query_impl<
        std::tuple<AllColumns>,
        Entity,
        std::monostate,
        std::tuple<>,
        std::tuple<>,
        SourceTag
    >;
    return query_t{
        std::make_tuple(AllColumns{}),
        {},
        {},
        {},
        std::nullopt,
        std::nullopt
    };
}

} // namespace atlas
