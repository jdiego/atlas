#pragma once
//
// UPDATE query builder.
//
// Usage:
//   atlas::update<User>()
//       .set(&User::name, "Alice2")
//       .where(atlas::eq(&User::id, 1))
//
// Serialises to:
//   "UPDATE users SET name = $1 WHERE id = $2"
//   params: {"Alice2", "1"}
//
// SetClausesTuple accumulates detail::set_clause<ColRef, Lit> nodes as .set()
// is called; WherePred is std::monostate until .where() is called.

#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "atlas/query/expr.hpp"
#include "atlas/query/insert.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// update_query_impl
// ---------------------------------------------------------------------------

template<typename Entity,
         typename SetClausesTuple = std::tuple<>,
         typename WherePred       = std::monostate>
struct update_query_impl {

    SetClausesTuple set_clauses;
    WherePred       where_pred;

    // -----------------------------------------------------------------------
    // .set(col, val)
    // -----------------------------------------------------------------------
    template<typename T>
    auto set(T Entity::* col, T val) &&
        -> update_query_impl<
               Entity,
               detail::tuple_append_t<
                   SetClausesTuple,
                   detail::set_clause<
                       column_ref<Entity, T>,
                       literal<T>>>,
               WherePred>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Appends a SET clause to the UPDATE, pairing the column reference
         *   with its new value.
         *
         * Step 1 — Construct detail::set_clause<column_ref<Entity,T>, literal<T>>
         *           {column_ref<Entity,T>{col}, literal<T>{std::move(val)}}.
         * Step 2 — Append to set_clauses via std::tuple_cat.
         * Step 3 — Move where_pred unchanged.
         * Step 4 — Return the new update_query_impl with extended SetClausesTuple.
         *
         * Key types involved:
         *   - detail::set_clause: defined in insert.hpp; reused here.
         *   - detail::tuple_append_t: grows the tuple (expr.hpp).
         *
         * Preconditions:
         *   - At least one .set() call is required before to_sql().
         *   - col must be registered in the table for Entity.
         *
         * Postconditions:
         *   - SetClausesTuple has one additional element.
         *
         * Hint:
         *   using SC = detail::set_clause<column_ref<Entity,T>, literal<T>>;
         *   auto new_clauses = std::tuple_cat(
         *       std::move(set_clauses),
         *       std::make_tuple(SC{column_ref<Entity,T>{col}, literal<T>{std::move(val)}}));
         *   return {std::move(new_clauses), std::move(where_pred)};
         */
    }

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template<typename Predicate>
        requires is_predicate<Predicate>
    auto where(Predicate&& p) &&
        -> update_query_impl<Entity, SetClausesTuple, std::remove_cvref_t<Predicate>>
    {
        /*
         * IMPLEMENTATION GUIDE:
         *
         * What this function does:
         *   Attaches a WHERE predicate to the UPDATE.  Replaces any existing
         *   predicate (updating without WHERE updates all rows — log a warning
         *   in the serialiser if WherePred is std::monostate).
         *
         * Step 1 — Forward p into the new query's where_pred field.
         * Step 2 — Move set_clauses unchanged.
         * Step 3 — Return the new update_query_impl with updated WherePred.
         *
         * Preconditions:
         *   - p must satisfy is_predicate<Predicate>.
         *
         * Postconditions:
         *   - WherePred == std::remove_cvref_t<Predicate> in the returned query.
         *
         * Pitfalls:
         *   - Omitting .where() produces "UPDATE … SET … " with no WHERE clause,
         *     which updates every row.  Consider a compile-time static_assert or
         *     runtime assertion to guard against accidental full-table updates.
         *
         * Hint:
         *   return {std::move(set_clauses), std::forward<Predicate>(p)};
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
         *   Serialises the UPDATE query to a parameterised SQL string.
         *
         * Step 1 — Resolve the table name via db.get_table<Entity>().name.
         * Step 2 — Create a serialize_context ctx{}.
         * Step 3 — Build the SET clause: iterate SetClausesTuple with std::apply;
         *           for each set_clause emit "col_name = $N" where $N comes from
         *           ctx.next_param(serialize_value(clause.val.value)).
         *           Join with ", ".
         * Step 4 — If WherePred != std::monostate, append
         *           " WHERE " + serialize_predicate(where_pred, db, ctx).
         * Step 5 — Return "UPDATE <table> SET <set_list>[WHERE <pred>]".
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
         * Hint:
         *   Use if constexpr (!std::is_same_v<WherePred, std::monostate>) to
         *   conditionally emit the WHERE clause.
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
         *   Returns the ordered parameter value strings for $1, $2, …
         *
         * Step 1 — Replicate the SET clause traversal from to_sql to collect
         *           values in the same order.
         * Step 2 — If WherePred != std::monostate, append WHERE predicate params.
         * Step 3 — Return the accumulated vector.
         *
         * Hint: share a private helper with to_sql().
         */
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
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Returns a default-constructed update_query ready for .set() and
     *   .where() calls.
     *
     * Hint: return update_query<Entity>{};
     */
}

} // namespace atlas
