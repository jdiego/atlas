#include <catch2/catch_test_macros.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <vector>

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
// Shared table descriptors (reused across tests)
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

TEST_CASE("make_table stores name correctly", "[table]") {
    auto t = make_user_table();
    REQUIRE(t.name == "users");
}

TEST_CASE("make_table column_count is correct", "[table]") {
    auto t = make_user_table();
    REQUIRE(t.column_count == 4u);
}

TEST_CASE("for_each_column visits all columns in declaration order", "[table]") {
    auto t = make_user_table();

    std::vector<std::string_view> names;
    t.for_each_column([&](const auto& col) {
        names.push_back(col.name);
    });

    REQUIRE(names.size() == 4u);
    REQUIRE(names[0] == "id");
    REQUIRE(names[1] == "name");
    REQUIRE(names[2] == "email");
    REQUIRE(names[3] == "score");
}

TEST_CASE("find_column returns the right column by member pointer", "[table]") {
    auto t = make_user_table();
    const auto& email_col = atlas::find_column(t, &User::email);
    REQUIRE(email_col.name == "email");
    REQUIRE(email_col.has_constraint<atlas::unique_t>());
}

// Note on static_assert for non-column arguments:
// Passing a non-column type to make_table causes a static_assert:
//   atlas::make_table<User>("users", 42);  // fails to compile:
//   "make_table: all arguments after name must satisfy is_column"
// This cannot be verified with REQUIRE — verify by uncommenting the line
// above and confirming a compile error is produced.

TEST_CASE("is_table concept is satisfied for table_t", "[table]") {
    auto t = make_user_table();
    STATIC_REQUIRE(atlas::is_table<decltype(t)>);
    STATIC_REQUIRE(!atlas::is_table<int>);
    STATIC_REQUIRE(!atlas::is_table<User>);
}

TEST_CASE("storage get_table<User> returns users table", "[table][storage]") {
    auto db = atlas::make_storage(make_user_table(), make_post_table());

    const auto& ut = db.get_table<User>();
    REQUIRE(ut.name == "users");
    REQUIRE(ut.column_count == 4u);
}

TEST_CASE("storage get_table<Post> returns posts table", "[table][storage]") {
    auto db = atlas::make_storage(make_user_table(), make_post_table());

    const auto& pt = db.get_table<Post>();
    REQUIRE(pt.name == "posts");
    REQUIRE(pt.column_count == 3u);
}

TEST_CASE("storage for_each_table visits both tables", "[table][storage]") {
    auto db = atlas::make_storage(make_user_table(), make_post_table());

    std::vector<std::string_view> table_names;
    db.for_each_table([&](const auto& tbl) {
        table_names.push_back(tbl.name);
    });

    REQUIRE(table_names.size() == 2u);
    REQUIRE(table_names[0] == "users");
    REQUIRE(table_names[1] == "posts");
}

// Note on static_assert for unregistered entity:
// Calling get_table<T>() for an entity not in the storage causes:
//   "get_table<Entity>: Entity is not registered in this storage"
// To verify: db.get_table<double>(); — this will not compile.
