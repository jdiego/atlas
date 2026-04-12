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
#include "atlas/schema/storage.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// remove_query
// ---------------------------------------------------------------------------

template<typename Entity, typename Predicate = std::monostate>
struct remove_query {

    Predicate where_pred;

    // -----------------------------------------------------------------------
    // .where(predicate)
    // -----------------------------------------------------------------------
    template<typename P>
        requires is_predicate<P>
    auto where(P&& p) &&
        -> remove_query<Entity, std::remove_cvref_t<P>>
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
         *   Serialises the DELETE query to a parameterised SQL string.
         *
         * Step 1 — Resolve the table name: db.get_table<Entity>().name.
         * Step 2 — Create serialize_context ctx{}.
         * Step 3 — Build "DELETE FROM <table>".
         * Step 4 — If Predicate != std::monostate, append
         *           " WHERE " + serialize_predicate(where_pred, db, ctx).
         * Step 5 — Return the SQL string.
         *
         * Key types involved:
         *   - serialize_predicate: sql_serialize.hpp.
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
         * Hint:
         *   std::string sql = "DELETE FROM ";
         *   sql += db.get_table<Entity>().name;
         *   if constexpr (!std::is_same_v<Predicate, std::monostate>) {
         *       serialize_context ctx{};
         *       sql += " WHERE " + serialize_predicate(where_pred, db, ctx);
         *   }
         *   return sql;
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
         *   Returns the WHERE clause parameter strings.
         *
         * Step 1 — If Predicate == std::monostate, return an empty vector.
         * Step 2 — Otherwise replicate the WHERE traversal from to_sql()
         *           and return ctx.params.
         *
         * Hint: share a private helper or duplicate the small traversal.
         */
    }
};

// ---------------------------------------------------------------------------
// Factory: atlas::remove<Entity>()
// ---------------------------------------------------------------------------

template<typename Entity>
constexpr auto remove() -> remove_query<Entity>
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this function does:
     *   Returns a default-constructed remove_query with no WHERE predicate.
     *
     * Step 1 — Return remove_query<Entity>{}.
     *
     * Hint: return remove_query<Entity>{};
     */
}

} // namespace atlas
