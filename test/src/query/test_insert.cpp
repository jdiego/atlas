// Tests for atlas/query/insert.hpp
//
// Covers full-object mode (.value()), partial mode (.set()), to_sql(), and
// params().  Link-time failure expected until implementations are provided.

#include <boost/ut.hpp>

#include "atlas/query/insert.hpp"
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

ut::suite<"query/insert"> insert_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // atlas::insert<Entity>() factory
    // -----------------------------------------------------------------------

    "insert() returns a default insert_query"_test = [] {
        auto q = atlas::insert<User>();

        using expected_t = atlas::insert_query<User>;
        static_assert(std::is_same_v<decltype(q), expected_t>);

        expect(q.full_object_mode == false);
        expect(std::tuple_size_v<decltype(q.set_clauses)> == 0_u);
    };

    // -----------------------------------------------------------------------
    // .value(entity) — full-object mode
    // -----------------------------------------------------------------------

    "value() stores entity and enables full-object mode"_test = [] {
        User u{1, "Alice", "alice@corp.com", 30};
        auto q = atlas::insert<User>().value(u);

        expect(q.full_object_mode == true);
    };

    "value() accepts default-constructed entity"_test = [] {
        User empty{};
        auto q = atlas::insert<User>().value(empty);
        expect(q.full_object_mode == true);
    };

    // -----------------------------------------------------------------------
    // .set() — partial mode
    // -----------------------------------------------------------------------

    "set() accumulates one SET clause"_test = [] {
        auto q = atlas::insert<User>()
            .set(&User::name, std::string{"Bob"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 1u);

        const auto& clause = std::get<0>(q.set_clauses);
        expect(clause.col.ptr == &User::name);
        expect(clause.val.value == "Bob");
    };

    "set() accumulates two SET clauses"_test = [] {
        auto q = atlas::insert<User>()
            .set(&User::name,  std::string{"Bob"})
            .set(&User::email, std::string{"bob@corp.com"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 2u);
        expect(true);
    };

    "set() accepts integer column"_test = [] {
        auto q = atlas::insert<User>().set(&User::age, 25);
        const auto& clause = std::get<0>(q.set_clauses);
        expect(clause.val.value == 25);
    };

    // -----------------------------------------------------------------------
    // to_sql() + params() — full-object mode
    // -----------------------------------------------------------------------

    "full-object insert produces complete INSERT statement"_test = [] {
        auto db = make_db();
        User u{1, "Alice", "alice@corp.com", 30};

        auto q   = atlas::insert<User>().value(u);
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "INSERT INTO users (id, name, email, age) VALUES ($1, $2, $3, $4)");
        expect(prm == std::vector<std::string>{"1", "Alice", "alice@corp.com", "30"});
    };

    "full-object insert with default-constructed entity emits zero/empty params"_test = [] {
        auto db = make_db();
        User empty{};
        auto q   = atlas::insert<User>().value(empty);
        expect(q.to_sql(db) == "INSERT INTO users (id, name, email, age) VALUES ($1, $2, $3, $4)");
        auto prm = q.params();

        expect(prm.size() == 4_u);
        expect(prm[0] == "0");   // id
        expect(prm[1].empty());  // name
        expect(prm[2].empty());  // email
        expect(prm[3] == "0");   // age
    };

    // -----------------------------------------------------------------------
    // to_sql() + params() — partial mode
    // -----------------------------------------------------------------------

    "partial insert with two columns produces partial INSERT"_test = [] {
        auto db  = make_db();
        auto q   = atlas::insert<User>()
                       .set(&User::name,  std::string{"Bob"})
                       .set(&User::email, std::string{"bob@corp.com"});
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "INSERT INTO users (name, email) VALUES ($1, $2)");
        expect(prm == std::vector<std::string>{"Bob", "bob@corp.com"});
    };

    "partial insert with single column"_test = [] {
        auto db  = make_db();
        auto q   = atlas::insert<User>().set(&User::name, std::string{"Anon"});
        auto sql = q.to_sql(db);
        auto prm = q.params();

        expect(sql == "INSERT INTO users (name) VALUES ($1)");
        expect(prm.size() == 1_u);
        expect(prm[0] == "Anon");
    };
};
