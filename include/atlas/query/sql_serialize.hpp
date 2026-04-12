#pragma once
//
// SQL serialisation engine for the query builder.
//
// Non-template helpers (serialize_context::next_param) are declared here and
// implemented in sql_serialize.cpp to keep the header light.
//
// Template helpers (serialize_predicate, serialize_column_ref,
// serialize_aggregate) are fully declared here with implementation-guide
// comment bodies because they must be visible at instantiation time.
//
// Design rules:
//   - No libpq dependency; all output is std::string.
//   - No exceptions; ill-formed queries produce a best-effort string with
//     an embedded "/* ERROR: ... */" marker.
//   - std::string / std::ostringstream / std::to_string are permitted here.

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "atlas/query/aggregate.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/schema/serde.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// serialize_context
// ---------------------------------------------------------------------------
// Accumulates the bound parameter strings and manages the $N counter.
// Passed by reference through all serialize_* functions so that parameter
// numbering is consistent across the whole query.

struct serialize_context {
    std::vector<std::string> params;
    int                      param_counter = 1;

    // Appends value to params and returns the "$N" placeholder string.
    // Declared here, implemented in sql_serialize.cpp (non-template).
    std::string next_param(std::string value);
};

// ---------------------------------------------------------------------------
// serialize_column_ref<Entity, T, Tables...>
// ---------------------------------------------------------------------------
// Returns the qualified SQL column name, e.g. "u.email".
// The table alias is derived from the first character of the table name,
// lowercased (e.g. "users" → alias "u").

template<typename Entity, typename T, typename... Tables>
[[nodiscard]] std::string
serialize_column_ref(const column_ref<Entity, T>& ref,
                     const storage_t<Tables...>& db)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Maps a column_ref<Entity, T> to "alias.column_name" by looking up
     *   the table for Entity in db, finding the column matching ref.ptr,
     *   and building the qualified name string.
     *
     * Step 1 — Resolve the table: const auto& tbl = db.get_table<Entity>();
     * Step 2 — Derive the alias: std::string alias{static_cast<char>(
     *               std::tolower(static_cast<unsigned char>(tbl.name[0])))};
     * Step 3 — Find the column name: tbl.find_column(ref.ptr).name
     *           (returns a detail::column_ref whose .name is std::string_view).
     * Step 4 — Return alias + "." + std::string(col_name).
     *
     * Key types involved:
     *   - storage_t::get_table<Entity>(): returns the table_t for Entity.
     *   - table_t::find_column(ptr): returns a detail::column_ref proxy whose
     *     .name field is the SQL column name (std::string_view).
     *
     * Preconditions:
     *   - Entity is registered in db (static_assert fires otherwise).
     *   - ref.ptr is registered in the table's column list.
     *   - tbl.name is non-empty.
     *
     * Postconditions:
     *   - Returns a non-empty string of the form "<alias>.<col_name>".
     *
     * Pitfalls:
     *   - The alias derivation (first char of table name) is a simplification;
     *     it can collide when two tables share the same initial (e.g. "users"
     *     and "updates"). A production implementation should track used aliases
     *     in a map; this implementation accepts the limitation.
     *   - table_t::find_column() throws a string literal if the ptr is not found;
     *     the caller is responsible for only passing registered pointers.
     *
     * Hint:
     *   const auto& tbl = db.get_table<Entity>();
     *   std::string alias{static_cast<char>(std::tolower(
     *       static_cast<unsigned char>(tbl.name[0])))};
     *   std::string col_name{tbl.find_column(ref.ptr).name};
     *   return alias + "." + col_name;
     */
}

// ---------------------------------------------------------------------------
// serialize_aggregate<AggExpr, Tables...>
// ---------------------------------------------------------------------------
// Returns the SQL aggregate fragment, e.g. "COUNT(u.id)" or "COUNT(*)".

template<typename AggExpr, typename... Tables>
[[nodiscard]] std::string
serialize_aggregate(const AggExpr& agg, const storage_t<Tables...>& db)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Detects the aggregate node type with a chain of if constexpr checks
     *   and emits the appropriate SQL function call.
     *
     * Step 1 — if constexpr (std::is_same_v<AggExpr, count_star_expr>)
     *             return "COUNT(*)";
     * Step 2 — else if constexpr (is_aggregate_impl<AggExpr>) {
     *             // Determine function name from type:
     *             // count_expr → "COUNT", sum_expr → "SUM", etc.
     *             std::string fn;
     *             if constexpr (detail::is_aggregate_impl<count_expr<...>>::value) fn = "COUNT";
     *             // ... (better: use a constexpr helper template)
     *             return fn + "(" + serialize_column_ref(agg.col, db) + ")";
     *           }
     *
     * Key types involved:
     *   - count_expr, sum_expr, avg_expr, min_expr, max_expr, count_star_expr:
     *     all defined in aggregate.hpp.
     *   - serialize_column_ref: resolves the inner column reference.
     *
     * Preconditions:
     *   - AggExpr satisfies is_aggregate<AggExpr>.
     *
     * Postconditions:
     *   - Returns a non-empty SQL aggregate fragment.
     *
     * Pitfalls:
     *   - Do not confuse AggExpr template parameters: count_expr<ColRef>
     *     carries a ColRef, not a raw member pointer.
     *
     * Hint:
     *   Use a local constexpr lambda to map type → function name string:
     *     constexpr auto fn_name = []() -> std::string_view {
     *         if constexpr (detail::is_count_expr<AggExpr>) return "COUNT";
     *         else if constexpr (detail::is_sum_expr<AggExpr>)  return "SUM";
     *         ...
     *     }();
     */
}

// ---------------------------------------------------------------------------
// serialize_rhs<Rhs, Tables...>
// ---------------------------------------------------------------------------
// Helper: serialise the right-hand side of a binary predicate.
//   - If Rhs is a literal<T>: append param, return "$N".
//   - If Rhs is a column_ref<Entity,T>: return "alias.col_name" (no param).

template<typename Rhs, typename... Tables>
[[nodiscard]] std::string
serialize_rhs(const Rhs& rhs,
              const storage_t<Tables...>& db,
              serialize_context& ctx)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Dispatches on whether Rhs is a literal or a column_ref and returns
     *   the appropriate SQL fragment, updating ctx for literals.
     *
     * Step 1 — if constexpr (is_literal<Rhs>)
     *               return ctx.next_param(detail::serialize_value(rhs.value));
     * Step 2 — else if constexpr (is_column_ref<Rhs>)
     *               return serialize_column_ref(rhs, db);
     * Step 3 — else static_assert(false, "unsupported RHS type in predicate");
     *
     * Key types involved:
     *   - detail::serialize_value<T>: serde.hpp; converts T to std::string.
     *   - serialize_column_ref: for cross-column predicates (JOIN ON).
     *
     * Preconditions:
     *   - Rhs is either literal<T> or column_ref<Entity, T>.
     *
     * Postconditions:
     *   - If Rhs is literal: ctx.params.size() increases by 1.
     *   - If Rhs is column_ref: ctx.params is unchanged.
     *
     * Hint:
     *   if constexpr (is_literal<Rhs>) {
     *       return ctx.next_param(detail::serialize_value(rhs.value));
     *   } else {
     *       return serialize_column_ref(rhs, db);
     *   }
     */
}

// ---------------------------------------------------------------------------
// serialize_predicate<Predicate, Tables...>
// ---------------------------------------------------------------------------
// Recursively serialises a predicate AST node to a SQL fragment, updating ctx
// with bound parameters.  Uses if constexpr to dispatch on node type.

template<typename Predicate, typename... Tables>
[[nodiscard]] std::string
serialize_predicate(const Predicate& pred,
                    const storage_t<Tables...>& db,
                    serialize_context& ctx)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Walks the predicate AST with compile-time dispatch and emits a SQL
     *   fragment with $N placeholders for each literal value.
     *
     * Step 1 — Handle leaf comparison nodes (eq, ne, lt, gt, lte, gte, like):
     *   if constexpr (std::is_same_v<Predicate, eq_expr<...>>) {
     *       return serialize_column_ref(pred.lhs, db)
     *              + " = " + serialize_rhs(pred.rhs, db, ctx);
     *   }
     *   Use the appropriate operator string for each type:
     *     eq: "=", ne: "!=", lt: "<", gt: ">", lte: "<=", gte: ">=", like: "LIKE"
     *
     * Step 2 — Handle is_null / is_not_null:
     *   return serialize_column_ref(pred.col, db) + " IS NULL";
     *   return serialize_column_ref(pred.col, db) + " IS NOT NULL";
     *
     * Step 3 — Handle in_expr:
     *   Iterate pred.values; for each element call ctx.next_param(serialize_value(v)).
     *   Join placeholders with ", " and wrap in "col IN ($1, $2, …)".
     *   Edge case: empty container → emit "col IN (NULL)" or a specific error marker.
     *
     * Step 4 — Handle and_expr: "(" + serialize_predicate(pred.lhs) + " AND "
     *                               + serialize_predicate(pred.rhs) + ")"
     *
     * Step 5 — Handle or_expr: "(" + serialize_predicate(pred.lhs) + " OR "
     *                              + serialize_predicate(pred.rhs) + ")"
     *
     * Step 6 — Handle not_expr: "NOT (" + serialize_predicate(pred.inner) + ")"
     *
     * Key types involved:
     *   - All predicate node types from predicate.hpp.
     *   - serialize_rhs: handles literal vs column_ref dispatch.
     *   - detail::serialize_value<T>: converts T to std::string (serde.hpp).
     *
     * Preconditions:
     *   - Predicate satisfies is_predicate<Predicate>.
     *   - All column_refs in the predicate must reference entities in db.
     *
     * Postconditions:
     *   - Returns a non-empty SQL fragment.
     *   - ctx.params contains all bound literal values in left-to-right,
     *     depth-first order.
     *
     * Pitfalls:
     *   - and_expr and or_expr must recurse on BOTH sides; missing a side
     *     produces wrong parameter numbering.
     *   - The if constexpr chain must be exhaustive; add a final
     *     static_assert(false, "unhandled predicate type") in the else branch
     *     so compile errors surface immediately.
     *
     * Hint:
     *   Use a chain of:
     *     if constexpr (detail::is_predicate_impl<eq_expr<
     *         std::remove_cvref_t<decltype(pred.lhs)>,
     *         std::remove_cvref_t<decltype(pred.rhs)>>>::value)
     *   Or restructure by detecting the presence of ::lhs / ::rhs members
     *   and checking the SQL operator via a constexpr string_view table.
     */
}

} // namespace atlas
