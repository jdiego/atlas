#include <catch2/catch_test_macros.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/ddl.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Test entities
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    double      score{};
};

struct Post {
    int32_t     id{};
    int32_t     user_id{};
    std::string title{};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static auto make_user_table() {
    return atlas::make_table<User>("users",
        atlas::make_column("id",    &User::id,    atlas::primary_key()),
        atlas::make_column("name",  &User::name,  atlas::not_null()),
        atlas::make_column("email", &User::email, atlas::not_null(), atlas::unique()),
        atlas::make_column("score", &User::score, atlas::default_value(0.0))
    );
}

static auto make_post_table() {
    return atlas::make_table<Post>("posts",
        atlas::make_column("id",      &Post::id,      atlas::primary_key()),
        atlas::make_column("user_id", &Post::user_id, atlas::not_null(),
                           atlas::references<User>(&User::id)),
        atlas::make_column("title",   &Post::title,   atlas::not_null())
    );
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("create_table_sql matches expected output for User", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);

    const std::string expected =
        "CREATE TABLE IF NOT EXISTS users (\n"
        "  id INTEGER NOT NULL PRIMARY KEY,\n"
        "  name TEXT NOT NULL,\n"
        "  email TEXT NOT NULL UNIQUE,\n"
        "  score FLOAT8 NOT NULL DEFAULT 0.000000\n"
        ");";

    REQUIRE(sql == expected);
}

TEST_CASE("primary_key column contains PRIMARY KEY", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);
    REQUIRE(sql.find("PRIMARY KEY") != std::string::npos);
}

TEST_CASE("not_null column contains NOT NULL", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);
    REQUIRE(sql.find("NOT NULL") != std::string::npos);
}

TEST_CASE("unique column contains UNIQUE", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);
    REQUIRE(sql.find("UNIQUE") != std::string::npos);
}

TEST_CASE("default_value column contains DEFAULT", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);
    REQUIRE(sql.find("DEFAULT") != std::string::npos);
}

TEST_CASE("REFERENCES resolved when storage is provided", "[ddl]") {
    auto user_tbl = make_user_table();
    auto post_tbl = make_post_table();
    auto db       = atlas::make_storage(user_tbl, post_tbl);

    auto sql = atlas::create_table_sql(post_tbl, db);

    REQUIRE(sql.find("REFERENCES users(id)") != std::string::npos);
}

TEST_CASE("unresolved overload emits TODO comment instead of REFERENCES", "[ddl]") {
    auto post_tbl = make_post_table();
    auto sql      = atlas::create_table_sql(post_tbl);

    // No REFERENCES keyword in the unresolved overload.
    REQUIRE(sql.find("REFERENCES") == std::string::npos);
    // A TODO comment should appear in its place.
    REQUIRE(sql.find("TODO") != std::string::npos);
}

TEST_CASE("DDL has no trailing comma on last column", "[ddl]") {
    auto t   = make_user_table();
    auto sql = atlas::create_table_sql(t);
    // The last column line must not end with a comma before the newline.
    auto last_col_end = sql.rfind('\n', sql.rfind("score"));
    auto close_paren  = sql.rfind(");");
    // Between the last column line and ");" there should be no ','
    auto segment = sql.substr(sql.rfind('\n', close_paren - 1));
    REQUIRE(segment.find(',') == std::string::npos);
}
