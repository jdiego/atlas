#pragma once
//
// INSERT query builder.
//
// Two modes are supported:
//
//   Full-object: atlas::insert<User>().value(user_instance)
//     All columns from the table definition are included in declaration order.
//     The parameter list follows the table declaration order.
//
//   Partial / column-by-column:
//     atlas::insert<User>().set(&User::name, "Bob").set(&User::email, "b@c.com")
//     Only the explicitly set columns appear in the INSERT; others are omitted.
//     This is useful for tables with DEFAULT or auto-generated columns.
//
// Both modes share insert_query_impl; the SetClauses template parameter
// accumulates (column_ref, literal) pairs as .set() is called.

#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "atlas/detail/type_utils.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/sql_serialize.hpp"
#include "atlas/schema/serde.hpp"
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
// Insert SET clause aliases
// ---------------------------------------------------------------------------

template<typename Entity, typename T>
using insert_set_clause = detail::set_clause<column_ref<Entity, T>, literal<T>>;

template<typename Entity, typename SetClauses, typename T>
using insert_set_clauses_t = detail::tuple_append_t<SetClauses, insert_set_clause<Entity, T>>;

// ---------------------------------------------------------------------------
// insert_query_impl
// ---------------------------------------------------------------------------

template<typename Entity, typename SetClauses = std::tuple<>>
struct insert_query_impl {

    SetClauses set_clauses;

    // full-object mode flag: set by .value()
    // When true, to_sql serialises all columns from the table descriptor;
    // set_clauses is ignored.
    bool full_object_mode = false;
    std::optional<Entity> entity_val;

    // -----------------------------------------------------------------------
    // .value(entity)
    // -----------------------------------------------------------------------
    // Full-object insert: all columns from the table definition are included.
    // Returns a new query tagged for full-object mode.
    auto value(const Entity& entity) && -> insert_query_impl<Entity, SetClauses>
    {
        /*
         * What this function does:
         *   Stores the entity and marks the query for full-object serialisation.
         *   to_sql will serialise the stored entity using the table column order.
         *
         * Key types involved:
         *   - detail::serialize_value<T>: converts each mapped field into its
         *     PostgreSQL text representation.
         *
         * Preconditions:
         *   - .value() and .set() are mutually exclusive; do not call both.
         *
         * Postconditions:
         *   - full_object_mode == true.
         *   - entity_val holds a copy of entity.
         *
         * Pitfalls:
         *   - The Entity copy may be expensive for large structs; consider
         *     storing only the parameter strings instead of the entity.
         *
         */
        entity_val = entity;
        full_object_mode = true;
        return std::move(*this);
    }

    // -----------------------------------------------------------------------
    // .set(col, val)
    // -----------------------------------------------------------------------
    // Partial insert: bind one column at a time.
    template<typename T>
    auto set(T Entity::* col, T val) && 
        -> insert_query_impl<Entity, insert_set_clauses_t<Entity, SetClauses, T>>
    {
        /*
         * What this function does:
         *   Appends a (column_ref, literal) pair to the partial column list.
         *
         * Key types involved:
         *   - detail::set_clause: pairs the column reference with its value.
         *   - detail::tuple_append_t: grows the SetClauses by one element.
         *
         * Preconditions:
         *   - .set() must not be mixed with .value().
         *   - col must be registered in the table_t for Entity.
         *
         * Postconditions:
         *   - SetClauses has one more element.
         *
         */ 
        using SetClause = insert_set_clause<Entity, T>;
        auto new_clauses = std::tuple_cat(
            std::move(set_clauses),
            std::make_tuple(SetClause{column_ref<Entity,T>{col}, literal<T>{std::move(val)}})
        );
        return {std::move(new_clauses), false, std::nullopt};
    }

    // -----------------------------------------------------------------------
    // .to_sql(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::string to_sql(const storage_t<Tables...>& db) const
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Serialises the INSERT query to a parameterised SQL string.
         *
         *
         * Key types involved:
         *   - serialize_context: accumulates $N params (sql_serialize.hpp).
         *   - detail::serialize_value<T>: converts each bound value to text.
         *   - table.for_each_column(): resolves column names in partial mode.
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
         *   - Full-object parameter order must follow table.for_each_column().
         *   - Partial mode: only explicitly set columns appear; DEFAULT applies
         *     to omitted columns.
         *
         * Hint:
         *   Use serialize_context as the single source of placeholder numbering.
         */

        // Step 1 — Resolve the table via db.get_table<Entity>().
        const auto& table = db.template get_table<Entity>();

        serialize_context ctx{};
        std::string cols;
        std::string vals;
        bool first = true;
        auto append_binding = [&](std::string_view col_name, std::string value) {
            if (!first) {
                cols += ", ";
                vals += ", ";
            }

            cols += col_name;
            vals += ctx.next_param(std::move(value));
            first = false;
        };

        //Step 2 — Determine the column list and VALUES placeholder list:
        if (full_object_mode) {
            // Full-object mode: iterate all columns via table.for_each_column()
            // and append each value through serialize_context::next_param().
            table.for_each_column([&](const auto& col) {
                append_binding(col.name, detail::serialize_value(col.get(*entity_val)));
            });
        } 
        else {
            //Partial mode: iterate SetClauses via std::apply and append
            // each literal through serialize_context::next_param().
            if constexpr (std::tuple_size_v<SetClauses> > 0) {
                std::apply([&](const auto&... clauses) {
                    auto append_clause = [&](const auto& clause) {
                        std::string col_name;
                        table.for_each_column([&](const auto& col) {
                            if constexpr (std::is_same_v<decltype(col.member_ptr), decltype(clause.col.ptr)>) {
                                if (col.member_ptr == clause.col.ptr && col_name.empty()) {
                                    col_name = std::string(col.name);
                                }
                            }
                        });
                        append_binding(col_name, detail::serialize_value(clause.val.value));
                    };

                    (append_clause(clauses), ...);
                }, set_clauses);
            }
        }

        // Step 3 — Build "INSERT INTO <table> (<cols>) VALUES (<params>)".
        return "INSERT INTO " + std::string(table.name) + " (" + cols + ") VALUES (" + vals + ")";
    }

    // -----------------------------------------------------------------------
    // .params(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::vector<std::string> params(const storage_t<Tables...>& db) const
    {
        /*
         * Returns the parameter value strings corresponding to the $N
         * placeholders in to_sql(db). Full-object mode requires db to walk
         * table columns in declaration order; partial mode ignores db.
         */
        if (full_object_mode) {
            const auto& table = db.template get_table<Entity>();
            return to_params(*entity_val, table);
        }

        serialize_context ctx{};
        std::apply([&](const auto&... clauses) {
            (ctx.next_param(detail::serialize_value(clauses.val.value)), ...);
        }, set_clauses);
        return std::move(ctx.params);
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
     * What this function does:
     *   Returns a default-constructed insert_query ready for .value() or
     *   .set() calls.
     *
     */
    return insert_query<Entity>{};
}

} // namespace atlas
