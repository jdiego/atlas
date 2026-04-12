#include <boost/ut.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/ddl.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>

namespace ut = boost::ut;

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

ut::suite<"schema/ddl"> ddl_suite = [] {
    using namespace ut;

    "create_table_sql matches expected output for User"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);

        const std::string expected =
            "CREATE TABLE IF NOT EXISTS users (\n"
            "  id INTEGER NOT NULL PRIMARY KEY,\n"
            "  name TEXT NOT NULL,\n"
            "  email TEXT NOT NULL UNIQUE,\n"
            "  score FLOAT8 NOT NULL DEFAULT 0.000000\n"
            ");";

        expect(sql == expected);
    };

    "primary_key column contains PRIMARY KEY"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);
        expect(sql.find("PRIMARY KEY") != std::string::npos);
    };

    "not_null column contains NOT NULL"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);
        expect(sql.find("NOT NULL") != std::string::npos);
    };

    "unique column contains UNIQUE"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);
        expect(sql.find("UNIQUE") != std::string::npos);
    };

    "default_value column contains DEFAULT"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);
        expect(sql.find("DEFAULT") != std::string::npos);
    };

    "REFERENCES resolved when storage is provided"_test = [] {
        auto user_tbl = make_user_table();
        auto post_tbl = make_post_table();
        auto db       = atlas::make_storage(user_tbl, post_tbl);

        auto sql = atlas::create_table_sql(post_tbl, db);

        expect(sql.find("REFERENCES users(id)") != std::string::npos);
    };

    "unresolved overload emits TODO comment instead of REFERENCES"_test = [] {
        auto post_tbl = make_post_table();
        auto sql      = atlas::create_table_sql(post_tbl);

        expect(sql.find("REFERENCES users(id)") == std::string::npos);
        expect(sql.find("TODO") != std::string::npos);
    };

    "DDL has no trailing comma on last column"_test = [] {
        auto t   = make_user_table();
        auto sql = atlas::create_table_sql(t);
        auto close_paren = sql.rfind(");");
        auto segment = sql.substr(sql.rfind('\n', close_paren - 1));
        expect(segment.find(',') == std::string::npos);
    };
};
