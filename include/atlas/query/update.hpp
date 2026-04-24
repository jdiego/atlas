#pragma once
//
// UPDATE query builder.
//
// Usage:
//   atlas::update<User>().set(&User::name, "Alice2").where(atlas::eq(&User::id, 1))
// Serialises to:
//   "UPDATE users SET name = $1 WHERE id = $2"
//   params: {"Alice2", "1"}
//
// SetClauses accumulates detail::set_clause<ColRef, Lit> nodes as .set()
// is called; WherePred is std::monostate until .where() is called.

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/detail/type_utils.hpp"
#include "atlas/query/expr.hpp"
#include "atlas/query/insert.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/query/remove.hpp"
#include "atlas/query/sql_serialize.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

template<typename Entity, typename T>
using update_set_clause = detail::set_clause<column_ref<Entity, T>, literal<T>>;

template<typename Entity, typename SetClauses, typename T>
using update_set_clauses_t = detail::tuple_append_t<SetClauses, update_set_clause<Entity, T>>;

// ---------------------------------------------------------------------------
// update_query_impl
// ---------------------------------------------------------------------------

template<typename Entity, typename SetClauses = std::tuple<>, typename WherePred = std::monostate>
struct update_query_impl {

    SetClauses set_clauses;
    WherePred       where_pred;

    // -----------------------------------------------------------------------
    // .set(col, val)
    // -----------------------------------------------------------------------
    template<typename T>
    auto set(T Entity::* col, T val) && -> update_query_impl<Entity, update_set_clauses_t<Entity, SetClauses, T>, WherePred>
    {
        /*
         * What this function does:
         *   Appends a SET clause to the UPDATE, pairing the column reference
         *   with its new value.
         *
         * Key types involved:
         *   - detail::set_clause: defined in insert.hpp; reused here.
         *   - detail::tuple_append_t: grows the tuple (detail/type_utils.hpp).
         *
         * Preconditions:
         *   - At least one .set() call is required before to_sql().
         *   - col must be registered in the table for Entity.
         *
         * Postconditions:
         *   - SetClauses has one additional element.
         *
         */
        using SetClause = update_set_clause<Entity, T>;
        auto new_clauses = std::tuple_cat(
            std::move(set_clauses),
            std::make_tuple(SetClause{column_ref<Entity,T>{col}, literal<T>{std::move(val)}})
        );
        return {std::move(new_clauses), std::move(where_pred)};
    }

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template<typename Predicate> 
    requires is_predicate<Predicate>
    auto where(Predicate&& pred) && -> update_query_impl<Entity, SetClauses, std::remove_cvref_t<Predicate>>
    {
        /*
         * What this function does:
         *   Attaches a WHERE predicate to the UPDATE.  Replaces any existing
         *   predicate (updating without WHERE updates all rows — log a warning
         *   in the serialiser if WherePred is std::monostate).
         *
         * Preconditions:
         *   - pred must satisfy is_predicate<Predicate>.
         *
         * Postconditions:
         *   - WherePred == std::remove_cvref_t<Predicate> in the returned query.
         *
         * Pitfalls:
         *   - Omitting .where() produces "UPDATE … SET … " with no WHERE clause,
         *     which updates every row.  Consider a compile-time static_assert or
         *     runtime assertion to guard against accidental full-table updates.
         *
         */
        return {
            std::move(set_clauses),
            std::forward<Predicate>(pred)
        };
    }

    // -----------------------------------------------------------------------
    // .to_sql(db)
    // -----------------------------------------------------------------------
    template<typename... Tables>
    [[nodiscard]] std::string to_sql(const storage_t<Tables...>& db) const
    {
        /*
         * Serialises the UPDATE query to a parameterised SQL string.
         *
         * Key types involved:
         *   - serialize_context (sql_serialize.hpp): manages $N counter.
         *   - serialize_column_ref: resolves column name from column_ref.
         *   - detail::serialize_value<T>: converts literal value to string.
         *
         * Preconditions:
         *   - At least one .set() must have been called.
         *
         * Postconditions:
         *   - The returned string is a valid PostgreSQL UPDATE statement with
         *     $N placeholders.
         *
         * Pitfalls:
         *   - Column references in SET clauses must be resolved without a table
         *     alias: UPDATE does not use "u.name" — use only "name".
         *
         */
        static_assert(std::tuple_size_v<SetClauses> > 0, "update_query::to_sql() requires at least one .set() call");

        const auto& table = db.template get_table<Entity>();

        serialize_context ctx{};
        std::string set_list;
        bool first = true;

        bind_set_clauses(ctx, [&](const auto& clause, std::string placeholder) {
            if (!first) {
                set_list += ", ";
            }

            set_list += table.find_column(clause.col.ptr).name;
            set_list += " = ";
            set_list += std::move(placeholder);
            first = false;
        });

        std::string sql = "UPDATE " + std::string(table.name) + " SET " + set_list;
        // Optional WHERE clause
        if constexpr (!std::is_same_v<WherePred, std::monostate>) {
            sql += " WHERE " + detail::serialize_delete_predicate(where_pred, db, ctx);
        }

        return sql;
    }

    // -----------------------------------------------------------------------
    // .params()
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<std::string> params() const
    {
        /*
         * Returns the parameter list corresponding to the generated SQL.
         * Guarantees:
         *   - Ordering matches placeholder numbering ($1, $2, ...).
         *   - Includes both SET values and WHERE predicate values.
         */
        serialize_context ctx{};
        bind_set_clauses(ctx, [](const auto&, std::string) {});

        if constexpr (!std::is_same_v<WherePred, std::monostate>) {
            detail::collect_predicate_params(where_pred, ctx);
        }

        return std::move(ctx.params);
    }

private:
    /*
    * Iterates over all SET clauses, binding each value into the context
    * and invoking the provided callback with the generated placeholder.
    * This centralizes parameter ordering logic to ensure consistency
    * between SQL generation and parameter extraction.
    */
    template<typename F>
    void bind_set_clauses(serialize_context& ctx, F&& on_clause) const
    {
        std::apply([&](const auto&... clauses) {
            auto bind_clause = [&](const auto& clause) {
                on_clause(clause, ctx.next_param(detail::serialize_value(clause.val.value)));
            };

            (bind_clause(clauses), ...);
        }, set_clauses);
    }
};

// ---------------------------------------------------------------------------
// User-facing alias
// ---------------------------------------------------------------------------

template<typename Entity>
using update_query = update_query_impl<Entity, std::tuple<>, std::monostate>;

// ---------------------------------------------------------------------------
// Factory: atlas::update<Entity>()
// ---------------------------------------------------------------------------

template<typename Entity>
constexpr auto update() -> update_query<Entity>
{
    /*
     * What this function does:
     *   Returns a default-constructed update_query ready for .set() and
     *   .where() calls.
     */
    return update_query<Entity>{};
}

} // namespace atlas
