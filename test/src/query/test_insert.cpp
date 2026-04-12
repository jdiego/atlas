// Tests for atlas/query/insert.hpp
//
// Covers full-object mode (.value()), partial mode (.set()), to_sql(), and
// params().  Link-time failure expected until implementations are provided.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/insert.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

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

// ---------------------------------------------------------------------------
// TEST: atlas::insert<Entity>() factory
// ---------------------------------------------------------------------------

TEST_CASE("insert() factory returns a default insert_query", "[insert][factory]") {

    SECTION("happy path — initial state") {
        auto q = atlas::insert<User>();

        using expected_t = atlas::insert_query<User>;
        static_assert(std::is_same_v<decltype(q), expected_t>);

        CHECK(q.full_object_mode == false);
        CHECK(std::tuple_size_v<decltype(q.set_clauses)> == 0u);
    }
}

// ---------------------------------------------------------------------------
// TEST: .value(entity) — full-object mode
// ---------------------------------------------------------------------------

TEST_CASE("insert().value(e) enables full-object mode", "[insert][value]") {

    SECTION("happy path — stores entity and sets flag") {
        User u{1, "Alice", "alice@corp.com", 30};
        auto q = atlas::insert<User>().value(u);

        CHECK(q.full_object_mode == true);
    }

    SECTION("edge case — default-constructed entity") {
        User empty{};
        auto q = atlas::insert<User>().value(empty);
        CHECK(q.full_object_mode == true);
    }
}

// ---------------------------------------------------------------------------
// TEST: .set() — partial mode
// ---------------------------------------------------------------------------

TEST_CASE("insert().set() accumulates set_clauses", "[insert][set]") {

    SECTION("happy path — one column") {
        auto q = atlas::insert<User>()
            .set(&User::name, std::string{"Bob"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 1u);

        const auto& clause = std::get<0>(q.set_clauses);
        CHECK(clause.col.ptr == &User::name);
        CHECK(clause.val.value == "Bob");
    }

    SECTION("happy path — two columns") {
        auto q = atlas::insert<User>()
            .set(&User::name,  std::string{"Bob"})
            .set(&User::email, std::string{"bob@corp.com"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 2u);
    }

    SECTION("edge case — integer column") {
        auto q = atlas::insert<User>().set(&User::age, 25);
        const auto& clause = std::get<0>(q.set_clauses);
        CHECK(clause.val.value == 25);
    }
}

// ---------------------------------------------------------------------------
// TEST: to_sql() + params() — full-object mode
// ---------------------------------------------------------------------------

TEST_CASE("insert().value(e).to_sql() produces full INSERT statement", "[insert][to_sql]") {
    auto db = make_db();
    User u{1, "Alice", "alice@corp.com", 30};

    SECTION("happy path") {
        auto q   = atlas::insert<User>().value(u);
        auto sql = q.to_sql(db);
        auto prm = q.params();

        CHECK(sql == "INSERT INTO users (id, name, email, age) VALUES ($1, $2, $3, $4)");
        CHECK(prm == std::vector<std::string>{"1", "Alice", "alice@corp.com", "30"});
    }

    SECTION("edge case — default-constructed entity (all zero / empty)") {
        User empty{};
        auto q   = atlas::insert<User>().value(empty);
        auto prm = q.params();

        REQUIRE(prm.size() == 4u);
        CHECK(prm[0] == "0");   // id
        CHECK(prm[1].empty());  // name
        CHECK(prm[2].empty());  // email
        CHECK(prm[3] == "0");   // age
    }
}

// ---------------------------------------------------------------------------
// TEST: to_sql() + params() — partial mode
// ---------------------------------------------------------------------------

TEST_CASE("insert().set().to_sql() produces partial INSERT statement", "[insert][to_sql_partial]") {
    auto db = make_db();

    SECTION("happy path — two columns") {
        auto q   = atlas::insert<User>()
                       .set(&User::name,  std::string{"Bob"})
                       .set(&User::email, std::string{"bob@corp.com"});
        auto sql = q.to_sql(db);
        auto prm = q.params();

        CHECK(sql == "INSERT INTO users (name, email) VALUES ($1, $2)");
        CHECK(prm == std::vector<std::string>{"Bob", "bob@corp.com"});
    }

    SECTION("edge case — single column") {
        auto q   = atlas::insert<User>().set(&User::name, std::string{"Anon"});
        auto sql = q.to_sql(db);
        auto prm = q.params();

        CHECK(sql == "INSERT INTO users (name) VALUES ($1)");
        REQUIRE(prm.size() == 1u);
        CHECK(prm[0] == "Anon");
    }
}
