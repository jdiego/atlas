#pragma once
//
// DELETE query builder.
//
// Usage:
//   atlas::remove<User>().where(atlas::eq(&User::id, 1))
//
// Serialises to:
//   "DELETE FROM users WHERE id = $1"
//   params: {"1"}
//
// Predicate is std::monostate when no .where() has been called, which
// produces a full-table DELETE — serialise with a warning comment.

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "atlas/query/predicate.hpp"
#include "atlas/query/sql_serialize.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

namespace detail {

template <typename Entity, typename T, typename Tag, typename... Tables>
[[nodiscard]] inline std::string serialize_delete_column_ref(
    const atlas::column_ref<Entity, T, Tag>& ref, 
    const storage_t<Tables...>& db
){
    const auto& table = db.template get_table<Entity>();
    std::string col_name;

    table.for_each_column([&](const auto& col) {
        if constexpr (std::is_same_v<decltype(col.member_ptr), decltype(ref.ptr)>) {
            if (col.member_ptr == ref.ptr && col_name.empty()) {
                col_name = std::string(col.name);
            }
        }
    });

    return col_name;
}

template <bool EmitSql, typename Rhs, typename... Tables>
inline void walk_delete_rhs(
    const Rhs& rhs,
    const storage_t<Tables...>& db,
    serialize_context& ctx,
    std::string& sql)
{
    using R = std::remove_cvref_t<Rhs>;

    if constexpr (is_literal<R>) {
        auto placeholder = ctx.next_param(detail::serialize_value(rhs.value));
        if constexpr (EmitSql) {
            sql += placeholder;
        }
    } else if constexpr (is_column_ref<R>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(rhs, db);
        }
    } else {
        static_assert(always_false<R>, "walk_delete_rhs: unsupported RHS type in DELETE predicate");
    }
}

template <bool EmitSql, typename Predicate, typename... Tables>
inline void walk_delete_predicate(
    const Predicate& pred,
    const storage_t<Tables...>& db,
    serialize_context& ctx,
    std::string& sql)
{
    using Pred = std::remove_cvref_t<Predicate>;
    static_assert(is_predicate<Pred>, "walk_delete_predicate: unsupported predicate type");

    if constexpr (is_specialization_of<eq_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " = ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<ne_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " != ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<lt_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " < ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<gt_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " > ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<lte_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " <= ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<gte_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " >= ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<like_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.lhs, db);
            sql += " LIKE ";
        }
        walk_delete_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<is_null_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.col, db);
            sql += " IS NULL";
        }
    } else if constexpr (is_specialization_of<is_not_null_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.col, db);
            sql += " IS NOT NULL";
        }
    } else if constexpr (is_specialization_of<in_expr, Pred>) {
        if (pred.values.empty()) {
            if constexpr (EmitSql) {
                sql += "FALSE";
            }
            return;
        }

        if constexpr (EmitSql) {
            sql += serialize_delete_column_ref(pred.col, db);
            sql += " IN (";
            bool first = true;
            for (const auto& value : pred.values) {
                if (!first) {
                    sql += ", ";
                }
                sql += ctx.next_param(detail::serialize_value(value));
                first = false;
            }
            sql += ")";
        } else {
            for (const auto& value : pred.values) {
                ctx.next_param(detail::serialize_value(value));
            }
        }
    } else if constexpr (is_specialization_of<and_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "(";
        }
        walk_delete_predicate<EmitSql>(pred.lhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += " AND ";
        }
        walk_delete_predicate<EmitSql>(pred.rhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else if constexpr (is_specialization_of<or_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "(";
        }
        walk_delete_predicate<EmitSql>(pred.lhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += " OR ";
        }
        walk_delete_predicate<EmitSql>(pred.rhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else if constexpr (is_specialization_of<not_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "NOT (";
        }
        walk_delete_predicate<EmitSql>(pred.inner, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else {
        static_assert(always_false<Pred>, "walk_delete_predicate: unhandled predicate type");
    }
}

template <typename Predicate, typename... Tables>
[[nodiscard]] inline std::string serialize_delete_predicate(
    const Predicate& pred,
    const storage_t<Tables...>& db,
    serialize_context& ctx)
{
    std::string sql;
    walk_delete_predicate<true>(pred, db, ctx, sql);
    return sql;
}

} // namespace detail

// ---------------------------------------------------------------------------
// remove_query
// ---------------------------------------------------------------------------

template<typename Entity, typename Predicate = std::monostate>
struct remove_query {

    Predicate where_pred;

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template<typename P> requires is_predicate<P>
    auto where(P&& p) && -> remove_query<Entity, std::remove_cvref_t<P>>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Attaches a WHERE predicate to the DELETE.
         *
         * Step 1 — Forward p into the new remove_query's where_pred field.
         * Step 2 — Return remove_query<Entity, std::remove_cvref_t<P>>
         *           {std::forward<P>(p)}.
         *
         * Key types involved:
         *   - remove_query<Entity, P>: the returned specialisation with P as
         *     the WHERE predicate type.
         *
         * Preconditions:
         *   - p must satisfy is_predicate<P>.
         *
         * Postconditions:
         *   - The returned query's Predicate template parameter == std::remove_cvref_t<P>.
         *
         * Pitfalls:
         *   - An unguarded DELETE (no WHERE) deletes every row. The serialiser
         *     should emit a warning via a static_assert or a comment when
         *     Predicate == std::monostate and the query is to_sql()'d.
         *
         * Hint:
         *   return remove_query<Entity, std::remove_cvref_t<P>>{std::forward<P>(p)};
         */
        return remove_query<Entity, std::remove_cvref_t<P>>{std::forward<P>(p)};
    }

    // -----------------------------------------------------------------------
    // .to_sql(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::string to_sql(const storage_t<Tables...>& db) const
    {
        /*
         * Serialises the DELETE query to a parameterised SQL string.
         *
         * Key types involved:
         *   - detail::serialize_delete_predicate: emits WHERE SQL without
         *     introducing table aliases into DELETE statements.
         *   - serialize_context: accumulates $N params.
         *
         * Preconditions:
         *   - Entity must be registered in db.
         *
         * Postconditions:
         *   - Returns valid PostgreSQL DELETE statement with $N placeholders.
         *   - ctx.params holds the WHERE clause parameters in order.
         *
         * Pitfalls:
         *   - DELETE does not support table aliases in all PostgreSQL versions;
         *     use only the bare table name, not "DELETE FROM users u".
         *
         */
        const auto& table = db.template get_table<Entity>();
        std::string sql = "DELETE FROM " + std::string(table.name);
        if constexpr (!std::is_same_v<Predicate, std::monostate>) {
            serialize_context ctx{};
            sql += " WHERE " + detail::serialize_delete_predicate(where_pred, db, ctx);
        }
        return sql;
    }

    // -----------------------------------------------------------------------
    // .params(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::vector<std::string> params(const storage_t<Tables...>&) const
    {
        /*
         * Returns the WHERE clause parameter strings.
         * db is accepted for API symmetry with to_sql(db); not used here
         * because predicates store literal values directly.
         */
        if constexpr (std::is_same_v<Predicate, std::monostate>)
        {
            return {};
        }
         else {
            serialize_context ctx{};
            detail::collect_predicate_params(where_pred, ctx);
            return std::move(ctx.params);
        }
    }
};

// ---------------------------------------------------------------------------
// Factory: atlas::remove<Entity>()
// ---------------------------------------------------------------------------

template<typename Entity>
constexpr auto remove() -> remove_query<Entity>
{
    /*
     * What this function does:
     *   Returns a default-constructed remove_query with no WHERE predicate.
     */
    return remove_query<Entity>{};
}

} // namespace atlas
