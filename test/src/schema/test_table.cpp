#include <boost/ut.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
        atlas::make_column("id", &Post::id, atlas::primary_key()),
        atlas::make_column("user_id", &Post::user_id, atlas::not_null(), atlas::references<User>(&User::id)),
        atlas::make_column("title",  &Post::title, atlas::not_null())
    );
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Note on static_assert for non-column arguments:
// Passing a non-column type to make_table causes a static_assert:
//   atlas::make_table<User>("users", 42);  // fails to compile:
//   "make_table: all arguments after name must satisfy is_column"
// This cannot be verified with REQUIRE — verify by uncommenting the line
// above and confirming a compile error is produced.

ut::suite<"schema/table"> table_suite = [] {
    using namespace ut;

    "make_table stores name correctly"_test = [] {
        auto t = make_user_table();
        expect(t.name == "users");
    };

    "make_table column_count is correct"_test = [] {
        auto t = make_user_table();
        expect(t.column_count == 4_u);
    };

    "for_each_column visits all columns in declaration order"_test = [] {
        auto t = make_user_table();

        std::vector<std::string_view> names;
        t.for_each_column([&](const auto& col) {
            names.push_back(col.name);
        });

        expect(names.size() == 4_u);
        expect(names[0] == "id");
        expect(names[1] == "name");
        expect(names[2] == "email");
        expect(names[3] == "score");
    };

    "find_column returns the right column by member pointer"_test = [] {
        auto t = make_user_table();
        const auto& email_col = atlas::find_column(t, &User::email);
        expect(email_col.name == "email");
        expect(email_col.has_constraint<atlas::unique_t>());
    };

    "is_table concept is satisfied for table_t"_test = [] {
        auto t = make_user_table();
        static_assert(atlas::is_table<decltype(t)>);
        static_assert(!atlas::is_table<int>);
        static_assert(!atlas::is_table<User>);
        expect(true);
    };

    "storage get_table<User> returns users table"_test = [] {
        auto db = atlas::make_storage(make_user_table(), make_post_table());

        const auto& user_table = db.get_table<User>();
        expect(user_table.name == "users");
        expect(user_table.column_count == 4_u);
    };

    "storage get_table<Post> returns posts table"_test = [] {
        auto db = atlas::make_storage(make_user_table(), make_post_table());

        const auto& post_table = db.get_table<Post>();
        expect(post_table.name == "posts");
        expect(post_table.column_count == 3_u);
    };

    "storage for_each_table visits both tables"_test = [] {
        auto db = atlas::make_storage(make_user_table(), make_post_table());

        std::vector<std::string_view> table_names;
        db.for_each_table([&](const auto& tbl) {
            table_names.push_back(tbl.name);
        });

        expect(table_names.size() == 2_u);
        expect(table_names[0] == "users");
        expect(table_names[1] == "posts");
    };
};

// Note on static_assert for unregistered entity:
// Calling get_table<T>() for an entity not in the storage causes:
//   "get_table<Entity>: Entity is not registered in this storage"
// To verify: db.get_table<double>(); — this will not compile.
