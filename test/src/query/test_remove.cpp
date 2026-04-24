// Tests for atlas/query/remove.hpp
//
// Covers atlas::remove<Entity>(), .where(), to_sql(), params().
// Link-time failure expected until implementations are provided.

#include <boost/ut.hpp>

#include "atlas/query/remove.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ut = boost::ut;

// ---------------------------------------------------------------------------
// Test entity & schema
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
};

static auto make_db() {
    return atlas::make_storage(
        atlas::make_table<User>("users",
            atlas::make_column("id",    &User::id,    atlas::primary_key()),
            atlas::make_column("name",  &User::name,  atlas::not_null()),
            atlas::make_column("email", &User::email, atlas::not_null()),
            atlas::make_column("age",   &User::age,   atlas::not_null())
        )
    );
}

ut::suite<"query/remove"> remove_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // atlas::remove<Entity>() factory
    // -----------------------------------------------------------------------

    "remove() returns default remove_query with monostate predicate"_test = [] {
        auto q = atlas::remove<User>();

        using expected_t = atlas::remove_query<User>;
        static_assert(std::is_same_v<decltype(q), expected_t>);
        static_assert(std::is_same_v<decltype(q.where_pred), std::monostate>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // .where()
    // -----------------------------------------------------------------------

    "where() with eq predicate changes predicate type"_test = [] {
        auto q = atlas::remove<User>().where(atlas::eq(&User::id, 1));

        using expected_pred_t = atlas::eq_expr<
            atlas::column_ref<User, int32_t>,
            atlas::literal<int>>;
        static_assert(std::is_same_v<decltype(q.where_pred), expected_pred_t>);

        expect(q.where_pred.lhs.ptr == &User::id);
        expect(q.where_pred.rhs.value == 1);
    };

    "where() accepts compound predicate"_test = [] {
        auto q = atlas::remove<User>()
            .where(atlas::and_(
                atlas::eq(&User::id, 42),
                atlas::eq(&User::name, std::string{"temp"})
            ));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
        expect(true);
    };

    "where() accepts not_ predicate"_test = [] {
        auto q = atlas::remove<User>()
            .where(atlas::not_(atlas::eq(&User::id, 0)));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // to_sql() + params()
    // -----------------------------------------------------------------------

    "DELETE with simple WHERE"_test = [] {
        auto db  = make_db();
        auto q   = atlas::remove<User>().where(atlas::eq(&User::id, 1));
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "DELETE FROM users WHERE id = $1");
        expect(prm == std::vector<std::string>{"1"});
    };

    "DELETE with compound WHERE"_test = [] {
        auto db  = make_db();
        auto q   = atlas::remove<User>()
                       .where(atlas::and_(
                           atlas::lt(&User::age, 0),
                           atlas::eq(&User::name, std::string{"ghost"})
                       ));
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "DELETE FROM users WHERE (age < $1 AND name = $2)");
        expect(prm == std::vector<std::string>{"0", "ghost"});
    };

    "DELETE without WHERE deletes full table"_test = [] {
        auto db  = make_db();
        auto q   = atlas::remove<User>();
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "DELETE FROM users");
        expect(prm.empty());
    };
};
