#pragma once
//
// INSERT query builder.
//
// Two modes are supported:
//
//   Full-object: atlas::insert<User>().value(user_instance)
//     All columns from the table definition are included in declaration order.
//     The parameter list is produced via atlas::to_params().
//
//   Partial / column-by-column:
//     atlas::insert<User>().set(&User::name, "Bob").set(&User::email, "b@c.com")
//     Only the explicitly set columns appear in the INSERT; others are omitted.
//     This is useful for tables with DEFAULT or auto-generated columns.
//
// Both modes share insert_query_impl; the SetClausesTuple template parameter
// accumulates (column_ref, literal) pairs as .set() is called.

#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "atlas/query/expr.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Internal: set_clause<ColRef, Lit>
// ---------------------------------------------------------------------------

namespace detail {

template<typename ColRef, typename Lit>
struct set_clause {
    ColRef col;
    Lit    val;
};

} // namespace detail

// ---------------------------------------------------------------------------
// insert_query_impl
// ---------------------------------------------------------------------------

template<typename Entity, typename SetClausesTuple = std::tuple<>>
struct insert_query_impl {

    SetClausesTuple set_clauses;

    // full-object mode flag: set by .value()
    // When true, to_sql serialises all columns from the table descriptor;
    // set_clauses is ignored.
    bool full_object_mode = false;

    // -----------------------------------------------------------------------
    // .value(entity)
    // -----------------------------------------------------------------------
    // Full-object insert: all columns from the table definition are included.
    // Returns a new query tagged for full-object mode.
    auto value(const Entity& e) &&
        -> insert_query_impl<Entity, SetClausesTuple>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Stores the entity and marks the query for full-object serialisation.
         *   to_sql will call atlas::to_params(e, table) to build the parameter list.
         *
         * Step 1 — Store a copy of e (add an std::optional<Entity> entity_val field
         *           or similar storage mechanism to the struct).
         * Step 2 — Set full_object_mode = true.
         * Step 3 — Return std::move(*this).
         *
         * Key types involved:
         *   - atlas::to_params(e, table): serde.hpp function that serialises all
         *     columns in declaration order — reuse it here.
         *
         * Preconditions:
         *   - .value() and .set() are mutually exclusive; do not call both.
         *
         * Postconditions:
         *   - full_object_mode == true.
         *   - entity_val holds a copy of e.
         *
         * Pitfalls:
         *   - The Entity copy may be expensive for large structs; consider
         *     storing only the parameter strings instead of the entity.
         *
         * Hint:
         *   Add   std::optional<Entity> entity_val;   to the struct.
         *   entity_val = e; full_object_mode = true; return std::move(*this);
         */
    }

    // -----------------------------------------------------------------------
    // .set(col, val)
    // -----------------------------------------------------------------------
    // Partial insert: bind one column at a time.
    template<typename T>
    auto set(T Entity::* col, T val) &&
        -> insert_query_impl<
               Entity,
               detail::tuple_append_t<
                   SetClausesTuple,
                   detail::set_clause<
                       column_ref<Entity, T>,
                       literal<T>>>>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Appends a (column_ref, literal) pair to the partial column list.
         *
         * Step 1 — Construct detail::set_clause<column_ref<Entity,T>, literal<T>>
         *           {column_ref<Entity,T>{col}, literal<T>{std::move(val)}}.
         * Step 2 — Append via std::tuple_cat(std::move(set_clauses), clause_tuple).
         * Step 3 — Return new insert_query_impl with the extended SetClausesTuple.
         *
         * Key types involved:
         *   - detail::set_clause: pairs the column reference with its value.
         *   - detail::tuple_append_t: grows the SetClausesTuple by one element.
         *
         * Preconditions:
         *   - .set() must not be mixed with .value().
         *   - col must be registered in the table_t for Entity.
         *
         * Postconditions:
         *   - SetClausesTuple has one more element.
         *
         * Hint:
         *   using SC = detail::set_clause<column_ref<Entity,T>, literal<T>>;
         *   auto new_clauses = std::tuple_cat(
         *       std::move(set_clauses),
         *       std::make_tuple(SC{column_ref<Entity,T>{col}, literal<T>{std::move(val)}}));
         *   return {std::move(new_clauses)};
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
         *   Serialises the INSERT query to a parameterised SQL string.
         *
         * Step 1 — Resolve the table via db.get_table<Entity>().
         * Step 2 — Determine the column list and VALUES placeholder list:
         *   a) Full-object mode: iterate all columns via table.for_each_column()
         *      to build "(col1, col2, …)" and "($1, $2, …)".
         *   b) Partial mode: iterate SetClausesTuple via std::apply to build
         *      "(col1, col2)" and "($1, $2)".
         * Step 3 — Build "INSERT INTO <table> (<cols>) VALUES (<params>)".
         *
         * Key types involved:
         *   - serialize_context: accumulates $N params (sql_serialize.hpp).
         *   - atlas::to_params(e, table): for full-object mode, serialises all
         *     column values in declaration order.
         *   - serialize_column_ref: for partial mode, resolves column names.
         *
         * Preconditions:
         *   - Either .value() or at least one .set() must have been called.
         *   - Entity must be registered in db.
         *
         * Postconditions:
         *   - The SQL string contains no embedded values; all appear as $N.
         *   - params() returns values in the same $N order.
         *
         * Pitfalls:
         *   - Column order in full-object mode must match to_params() order
         *     (both driven by table.for_each_column()).
         *   - Partial mode: only explicitly set columns appear; DEFAULT applies
         *     to omitted columns.
         *
         * Hint:
         *   Create a serialize_context; reuse it in params().
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
         *   Returns the parameter value strings corresponding to the $N
         *   placeholders in to_sql().
         *
         * Step 1 — Replicate the same column/value traversal as to_sql().
         * Step 2 — Return the accumulated params vector.
         *
         * Hint: share a private build_params() helper with to_sql().
         */
    }
};

// ---------------------------------------------------------------------------
// User-facing alias: insert_query<Entity>
// ---------------------------------------------------------------------------

template<typename Entity>
using insert_query = insert_query_impl<Entity, std::tuple<>>;

// ---------------------------------------------------------------------------
// Factory: atlas::insert<Entity>()
// ---------------------------------------------------------------------------

template<typename Entity>
constexpr auto insert() -> insert_query<Entity>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Returns a default-constructed insert_query ready for .value() or
     *   .set() calls.
     *
     * Step 1 — Return insert_query<Entity>{}.
     *
     * Hint:
     *   return insert_query<Entity>{};
     */
}

} // namespace atlas
