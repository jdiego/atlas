#pragma once
//
// SQL serialisation engine for the query builder.
//
// Non-template helpers (serialize_context::next_param) are declared here and
// implemented in sql_serialize.cpp to keep the header light.
//
// Template helpers (serialize_predicate, serialize_column_ref,
// serialize_aggregate) are fully defined here because they must be visible
// at template instantiation time.
//
// Design rules:
//   - No libpq dependency; all output is std::string.
//   - No exceptions; ill-formed queries produce a best-effort string with
//     an embedded "/* ERROR: ... */" marker.

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <cctype>
#include "atlas/query/aggregate.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/schema/serde.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/type_traits.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// serialize_context
// ---------------------------------------------------------------------------
// Accumulates the bound parameter strings and manages the $N counter.
// Passed by reference through all serialize_* functions so that parameter
// numbering is consistent across the whole query.

struct serialize_context {
    int param_counter = 1;
    std::vector<std::string> params;

    // Appends value to params and returns the "$N" placeholder string.
    std::string next_param(std::string value);
};

// ---------------------------------------------------------------------------
// has_alias concept — detects tags carrying a static alias string
// ---------------------------------------------------------------------------
// A "tag" is any user-defined type used to disambiguate self-join instances
// in column_ref<Entity, T, Tag>. When the tag exposes a static `alias` member
// convertible to std::string_view, serialize_column_ref uses it as the SQL
// alias for that instance. Example:
//
//   struct mgr { static constexpr std::string_view alias = "m"; };
//   col<mgr>(&Employee::id)  ->  serialised as "m.id"

namespace detail {

template <typename Tag>
concept has_alias = requires {
    { Tag::alias } -> std::convertible_to<std::string_view>;
};

} // namespace detail

// ---------------------------------------------------------------------------
// serialize_column_ref<Entity, T, Tag, Tables...>
// ---------------------------------------------------------------------------
// Returns the qualified SQL column name, e.g. "u.email" or "m.id".
//
// Alias resolution:
//   - If Tag is void (non-self-join, default): alias = first lowercase
//     letter of the table name (e.g. "employees" -> "e").
//   - If Tag carries a static alias member: alias = Tag::alias.
//   - Otherwise (Tag is non-void but without alias): same default as void.
//
// Known limitation: the default first-letter scheme collides when two
// distinct tables share an initial (e.g. "users" and "updates"). For
// self-join this is resolved explicitly by providing tags with aliases.
// Cross-table collisions still need a future alias-map pass.

template <typename Entity, typename T, typename Tag, typename... Tables>
[[nodiscard]] std::string serialize_column_ref(const column_ref<Entity, T, Tag> &ref, const storage_t<Tables...> &db) {
    const auto &table = db.template get_table<Entity>();
    std::string alias;
    if constexpr (!std::is_void_v<Tag> && detail::has_alias<Tag>) {
        alias = std::string(Tag::alias);
    } else {
        alias = std::string{static_cast<char>(std::tolower(static_cast<unsigned char>(table.name[0])))};
    }
    std::string col_name{table.find_column(ref.ptr).name};
    return alias + "." + col_name;
}

// ---------------------------------------------------------------------------
// serialize_aggregate<AggExpr, Tables...>
// ---------------------------------------------------------------------------
// Returns the SQL aggregate fragment, e.g. "COUNT(u.id)" or "COUNT(*)".

template <typename AggExpr, typename... Tables>
    requires is_aggregate<AggExpr>
[[nodiscard]] std::string serialize_aggregate(const AggExpr &agg, const storage_t<Tables...> &db) {
    if constexpr (std::is_same_v<AggExpr, count_star_expr>) {
        return "COUNT(*)";
    } else {
        constexpr auto fn_name = []() -> std::string_view {
            if constexpr (is_specialization_of<count_expr, AggExpr>) {
                return "COUNT";
            } else if constexpr (is_specialization_of<sum_expr, AggExpr>) {
                return "SUM";
            } else if constexpr (is_specialization_of<avg_expr, AggExpr>) {
                return "AVG";
            } else if constexpr (is_specialization_of<min_expr, AggExpr>) {
                return "MIN";
            } else if constexpr (is_specialization_of<max_expr, AggExpr>) {
                return "MAX";
            } else {
                static_assert(detail::always_false<AggExpr>, "serialize_aggregate: unmapped aggregate type");
            }
        }();
        return std::string(fn_name) + "(" + serialize_column_ref(agg.col, db) + ")";
    }
}

// ---------------------------------------------------------------------------
// serialize_rhs<Rhs, Tables...>
// ---------------------------------------------------------------------------
// Serialises the right-hand side of a binary predicate.
//   - literal<T>:              appends param to ctx, returns "$N" placeholder.
//   - column_ref<Entity,T>:    returns "alias.col_name" (no param appended).

namespace detail {

struct params_only_tag {};

template <bool EmitSql, typename Rhs, typename Db>
inline void walk_predicate_rhs(
    const Rhs &rhs,
    const Db &db,
    serialize_context &ctx,
    std::string &sql
) {
    using R = std::remove_cvref_t<Rhs>;

    if constexpr (is_literal<R>) {
        auto placeholder = ctx.next_param(detail::serialize_value(rhs.value));
        if constexpr (EmitSql) {
            sql += placeholder;
        }
    } else if constexpr (is_column_ref<R>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(rhs, db);
        }
    } else {
        static_assert(always_false<R>, "walk_predicate_rhs: unsupported RHS type in predicate");
    }
}

template <bool EmitSql, typename Predicate, typename Db>
inline void walk_predicate(
    const Predicate &pred,
    const Db &db,
    serialize_context &ctx,
    std::string &sql
) {
    using Pred = std::remove_cvref_t<Predicate>;
    static_assert(is_predicate<Pred>, "walk_predicate: unsupported predicate type");

    if constexpr (is_specialization_of<eq_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " = ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<ne_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " != ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<lt_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " < ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<gt_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " > ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<lte_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " <= ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<gte_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " >= ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<like_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.lhs, db);
            sql += " LIKE ";
        }
        walk_predicate_rhs<EmitSql>(pred.rhs, db, ctx, sql);
    } else if constexpr (is_specialization_of<is_null_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.col, db);
            sql += " IS NULL";
        }
    } else if constexpr (is_specialization_of<is_not_null_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += serialize_column_ref(pred.col, db);
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
            sql += serialize_column_ref(pred.col, db);
            sql += " IN (";
            bool first = true;
            for (const auto &value : pred.values) {
                if (!first) {
                    sql += ", ";
                }
                sql += ctx.next_param(detail::serialize_value(value));
                first = false;
            }
            sql += ")";
        } else {
            for (const auto &value : pred.values) {
                ctx.next_param(detail::serialize_value(value));
            }
        }
    } else if constexpr (is_specialization_of<and_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "(";
        }
        walk_predicate<EmitSql>(pred.lhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += " AND ";
        }
        walk_predicate<EmitSql>(pred.rhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else if constexpr (is_specialization_of<or_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "(";
        }
        walk_predicate<EmitSql>(pred.lhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += " OR ";
        }
        walk_predicate<EmitSql>(pred.rhs, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else if constexpr (is_specialization_of<not_expr, Pred>) {
        if constexpr (EmitSql) {
            sql += "NOT (";
        }
        walk_predicate<EmitSql>(pred.inner, db, ctx, sql);
        if constexpr (EmitSql) {
            sql += ")";
        }
    } else {
        static_assert(always_false<Pred>, "walk_predicate: unhandled predicate type");
    }
}

template <typename Predicate>
inline void collect_predicate_params(const Predicate &pred, serialize_context &ctx) {
    params_only_tag params_only{};
    std::string ignored_sql;
    walk_predicate<false>(pred, params_only, ctx, ignored_sql);
}

} // namespace detail

template <typename Rhs, typename... Tables>
[[nodiscard]] std::string serialize_rhs(const Rhs &rhs, const storage_t<Tables...> &db, serialize_context &ctx) {
    std::string sql;
    detail::walk_predicate_rhs<true>(rhs, db, ctx, sql);
    return sql;
}

// ---------------------------------------------------------------------------
// serialize_predicate<Predicate, Tables...>
// ---------------------------------------------------------------------------
// Recursively serialises a predicate AST node to a SQL fragment, updating ctx
// with bound parameters.  Dispatches on node type via is_specialization_of.

template <typename Predicate, typename... Tables>
[[nodiscard]] std::string serialize_predicate(const Predicate &pred, const storage_t<Tables...> &db, serialize_context &ctx) {
    std::string sql;
    detail::walk_predicate<true>(pred, db, ctx, sql);
    return sql;
}

} // namespace atlas
