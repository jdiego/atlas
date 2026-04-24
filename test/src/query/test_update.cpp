// Tests for atlas/query/update.hpp
//
// Covers .set(), .where(), to_sql(), and params().
// Link-time failure expected until implementations are provided.

#include <boost/ut.hpp>

#include "atlas/query/update.hpp"
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

ut::suite<"query/update"> update_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // atlas::update<Entity>() factory
    // -----------------------------------------------------------------------

    "update() returns default update_query"_test = [] {
        auto q = atlas::update<User>();

        using expected_t = atlas::update_query<User>;
        static_assert(std::is_same_v<decltype(q), expected_t>);
        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 0u);
        static_assert(std::is_same_v<decltype(q.where_pred), std::monostate>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // .set()
    // -----------------------------------------------------------------------

    "set() adds a single SET clause"_test = [] {
        auto q = atlas::update<User>().set(&User::name, std::string{"Alice2"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 1u);

        const auto& c = std::get<0>(q.set_clauses);
        expect(c.col.ptr == &User::name);
        expect(c.val.value == "Alice2");
    };

    "set() accumulates two SET clauses"_test = [] {
        auto q = atlas::update<User>()
            .set(&User::name,  std::string{"Bob"})
            .set(&User::email, std::string{"b@c.com"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 2u);
        expect(true);
    };

    "set() accepts integer column"_test = [] {
        auto q = atlas::update<User>().set(&User::age, 99);
        const auto& c = std::get<0>(q.set_clauses);
        expect(c.val.value == 99);
    };

    // -----------------------------------------------------------------------
    // .where()
    // -----------------------------------------------------------------------

    "where() attaches eq predicate"_test = [] {
        auto q = atlas::update<User>()
            .set(&User::name, std::string{"Alice2"})
            .where(atlas::eq(&User::id, 1));

        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
        expect(true);
    };

    "where() attaches compound predicate"_test = [] {
        auto q = atlas::update<User>()
            .set(&User::age, 21)
            .where(atlas::and_(
                atlas::gte(&User::age, 18),
                atlas::eq(&User::email, std::string{"a@b.com"})
            ));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // to_sql() + params()
    // -----------------------------------------------------------------------

    "update with one SET and WHERE produces correct SQL"_test = [] {
        auto db  = make_db();
        auto q   = atlas::update<User>()
                       .set(&User::name, std::string{"Alice2"})
                       .where(atlas::eq(&User::id, 1));
        auto sql = q.to_sql(db);
        auto prm = q.params(db);

        expect(sql == "UPDATE users SET name = $1 WHERE id = $2");
        expect(prm == std::vector<std::string>{"Alice2", "1"});
    };

    "update with two SET columns and WHERE"_test = [] {
        auto db  = make_db();
        auto q   = atlas::update<User>()
                       .set(&User::name,  std::string{"Bob"})
                       .set(&User::email, std::string{"b@c.com"})
                       .where(atlas::eq(&User::id, 5));
        auto sql = q.to_sql(db);
        auto prm = q.params(db);

        expect(sql == "UPDATE users SET name = $1, email = $2 WHERE id = $3");
        expect(prm.size() == 3_u);
        expect(prm[0] == "Bob");
        expect(prm[1] == "b@c.com");
        expect(prm[2] == "5");
    };

    "update without WHERE produces full-table UPDATE"_test = [] {
        auto db  = make_db();
        auto q   = atlas::update<User>().set(&User::age, 0);
        auto sql = q.to_sql(db);

        expect(sql.find("WHERE") == std::string::npos);
        expect(sql.find("UPDATE users") != std::string::npos);
    };
};
