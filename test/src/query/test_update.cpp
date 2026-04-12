// Tests for atlas/query/update.hpp
//
// Covers .set(), .where(), to_sql(), and params().
// Link-time failure expected until implementations are provided.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/update.hpp"
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
// TEST: atlas::update<Entity>() factory
// ---------------------------------------------------------------------------

TEST_CASE("update() factory returns default update_query", "[update][factory]") {

    SECTION("happy path — initial state") {
        auto q = atlas::update<User>();

        using expected_t = atlas::update_query<User>;
        static_assert(std::is_same_v<decltype(q), expected_t>);
        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 0u);
        static_assert(std::is_same_v<decltype(q.where_pred), std::monostate>);
    }
}

// ---------------------------------------------------------------------------
// TEST: .set()
// ---------------------------------------------------------------------------

TEST_CASE("update().set() accumulates SET clauses", "[update][set]") {

    SECTION("happy path — single column") {
        auto q = atlas::update<User>().set(&User::name, std::string{"Alice2"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 1u);

        const auto& c = std::get<0>(q.set_clauses);
        CHECK(c.col.ptr == &User::name);
        CHECK(c.val.value == "Alice2");
    }

    SECTION("happy path — two columns") {
        auto q = atlas::update<User>()
            .set(&User::name,  std::string{"Bob"})
            .set(&User::email, std::string{"b@c.com"});

        static_assert(std::tuple_size_v<decltype(q.set_clauses)> == 2u);
    }

    SECTION("edge case — integer column") {
        auto q = atlas::update<User>().set(&User::age, 99);
        const auto& c = std::get<0>(q.set_clauses);
        CHECK(c.val.value == 99);
    }
}

// ---------------------------------------------------------------------------
// TEST: .where()
// ---------------------------------------------------------------------------

TEST_CASE("update().where() attaches WHERE predicate", "[update][where]") {

    SECTION("happy path — eq predicate") {
        auto q = atlas::update<User>()
            .set(&User::name, std::string{"Alice2"})
            .where(atlas::eq(&User::id, 1));

        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
    }

    SECTION("edge case — compound predicate") {
        auto q = atlas::update<User>()
            .set(&User::age, 21)
            .where(atlas::and_(
                atlas::gte(&User::age, 18),
                atlas::eq(&User::email, std::string{"a@b.com"})
            ));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
    }
}

// ---------------------------------------------------------------------------
// TEST: to_sql() + params()
// ---------------------------------------------------------------------------

TEST_CASE("update().to_sql() produces correct UPDATE statement", "[update][to_sql]") {
    auto db = make_db();

    SECTION("happy path — one SET + WHERE") {
        auto q   = atlas::update<User>()
                       .set(&User::name, std::string{"Alice2"})
                       .where(atlas::eq(&User::id, 1));
        auto sql = q.to_sql(db);
        auto prm = q.params();

        CHECK(sql == "UPDATE users SET name = $1 WHERE id = $2");
        CHECK(prm == std::vector<std::string>{"Alice2", "1"});
    }

    SECTION("edge case — two SET columns + WHERE") {
        auto q   = atlas::update<User>()
                       .set(&User::name,  std::string{"Bob"})
                       .set(&User::email, std::string{"b@c.com"})
                       .where(atlas::eq(&User::id, 5));
        auto sql = q.to_sql(db);
        auto prm = q.params();

        CHECK(sql == "UPDATE users SET name = $1, email = $2 WHERE id = $3");
        REQUIRE(prm.size() == 3u);
        CHECK(prm[0] == "Bob");
        CHECK(prm[1] == "b@c.com");
        CHECK(prm[2] == "5");
    }

    SECTION("edge case — no WHERE (full-table update)") {
        auto q   = atlas::update<User>().set(&User::age, 0);
        auto sql = q.to_sql(db);

        CHECK(sql.find("WHERE") == std::string::npos);
        CHECK(sql.find("UPDATE users") != std::string::npos);
    }
}
