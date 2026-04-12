// Tests for atlas/query/sql_serialize.hpp + sql_serialize.cpp
//
// Uses mock storage (no libpq).
// serialize_context::next_param tests will fail at link time until
// sql_serialize.cpp is implemented.
// serialize_column_ref and serialize_predicate tests require implementation.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/sql_serialize.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/query/aggregate.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test entity & schema
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
    double      score{};
};

struct Post {
    int32_t     id{};
    int32_t     user_id{};
    std::string title{};
};

static auto make_db() {
    auto user_table = atlas::make_table<User>("users",
        atlas::make_column("id",    &User::id,    atlas::primary_key()),
        atlas::make_column("name",  &User::name,  atlas::not_null()),
        atlas::make_column("email", &User::email, atlas::not_null()),
        atlas::make_column("age",   &User::age,   atlas::not_null()),
        atlas::make_column("score", &User::score, atlas::not_null())
    );
    auto post_table = atlas::make_table<Post>("posts",
        atlas::make_column("id",      &Post::id,      atlas::primary_key()),
        atlas::make_column("user_id", &Post::user_id, atlas::not_null()),
        atlas::make_column("title",   &Post::title,   atlas::not_null())
    );
    return atlas::make_storage(user_table, post_table);
}

// ---------------------------------------------------------------------------
// TEST: serialize_context::next_param
// ---------------------------------------------------------------------------

TEST_CASE("serialize_context::next_param returns $N placeholder and stores value",
          "[sql_serialize][next_param]") {

    SECTION("happy path — first call returns $1") {
        atlas::serialize_context ctx;
        auto placeholder = ctx.next_param("42");

        CHECK(placeholder == "$1");
        REQUIRE(ctx.params.size() == 1u);
        CHECK(ctx.params[0] == "42");
        CHECK(ctx.param_counter == 2);
    }

    SECTION("happy path — sequential calls return $1, $2, $3") {
        atlas::serialize_context ctx;

        auto p1 = ctx.next_param("first");
        auto p2 = ctx.next_param("second");
        auto p3 = ctx.next_param("third");

        CHECK(p1 == "$1");
        CHECK(p2 == "$2");
        CHECK(p3 == "$3");
        REQUIRE(ctx.params.size() == 3u);
        CHECK(ctx.params[0] == "first");
        CHECK(ctx.params[1] == "second");
        CHECK(ctx.params[2] == "third");
    }

    SECTION("edge case — empty string value is accepted") {
        atlas::serialize_context ctx;
        auto p = ctx.next_param("");
        CHECK(p == "$1");
        CHECK(ctx.params[0].empty());
    }

    SECTION("edge case — param_counter increments monotonically") {
        atlas::serialize_context ctx;
        ctx.next_param("a");
        ctx.next_param("b");
        ctx.next_param("c");
        CHECK(ctx.param_counter == 4);
    }
}

// ---------------------------------------------------------------------------
// TEST: serialize_column_ref
// ---------------------------------------------------------------------------

TEST_CASE("serialize_column_ref returns alias.column_name", "[sql_serialize][column_ref]") {
    auto db = make_db();

    SECTION("happy path — users.id → u.id") {
        atlas::column_ref<User, int32_t> ref{&User::id};
        auto sql = atlas::serialize_column_ref(ref, db);
        CHECK(sql == "u.id");
    }

    SECTION("happy path — users.email → u.email") {
        atlas::column_ref<User, std::string> ref{&User::email};
        auto sql = atlas::serialize_column_ref(ref, db);
        CHECK(sql == "u.email");
    }

    SECTION("edge case — post column → p.user_id") {
        atlas::column_ref<Post, int32_t> ref{&Post::user_id};
        auto sql = atlas::serialize_column_ref(ref, db);
        CHECK(sql == "p.user_id");
    }
}

// ---------------------------------------------------------------------------
// TEST: serialize_predicate — leaf nodes
// ---------------------------------------------------------------------------

TEST_CASE("serialize_predicate handles all leaf comparison nodes",
          "[sql_serialize][predicate][leaf]") {
    auto db = make_db();

    SECTION("eq — emits 'alias.col = $1'") {
        atlas::serialize_context ctx;
        auto pred = atlas::eq(&User::id, 1);
        auto sql  = atlas::serialize_predicate(pred, db, ctx);

        CHECK(sql == "u.id = $1");
        REQUIRE(ctx.params.size() == 1u);
        CHECK(ctx.params[0] == "1");
    }

    SECTION("ne") {
        atlas::serialize_context ctx;
        auto pred = atlas::ne(&User::age, 18);
        auto sql  = atlas::serialize_predicate(pred, db, ctx);
        CHECK(sql == "u.age != $1");
    }

    SECTION("lt") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::lt(&User::age, 30), db, ctx);
        CHECK(sql == "u.age < $1");
    }

    SECTION("gt") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::gt(&User::age, 18), db, ctx);
        CHECK(sql == "u.age > $1");
    }

    SECTION("lte") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::lte(&User::age, 65), db, ctx);
        CHECK(sql == "u.age <= $1");
    }

    SECTION("gte") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::gte(&User::score, 0.0), db, ctx);
        CHECK(sql == "u.score >= $1");
    }

    SECTION("like") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::like(&User::email, std::string{"%@corp.com"}), db, ctx);
        CHECK(sql == "u.email LIKE $1");
        CHECK(ctx.params[0] == "%@corp.com");
    }

    SECTION("is_null") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::is_null(&User::name), db, ctx);
        CHECK(sql == "u.name IS NULL");
        CHECK(ctx.params.empty());
    }

    SECTION("is_not_null") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::is_not_null(&User::name), db, ctx);
        CHECK(sql == "u.name IS NOT NULL");
        CHECK(ctx.params.empty());
    }
}

// ---------------------------------------------------------------------------
// TEST: serialize_predicate — in_expr
// ---------------------------------------------------------------------------

TEST_CASE("serialize_predicate handles in_expr", "[sql_serialize][predicate][in]") {
    auto db = make_db();

    SECTION("happy path — three values") {
        atlas::serialize_context ctx;
        std::vector<int32_t> ids{1, 2, 3};
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, ids), db, ctx);
        CHECK(sql == "u.id IN ($1, $2, $3)");
        REQUIRE(ctx.params.size() == 3u);
    }

    SECTION("edge case — single value") {
        atlas::serialize_context ctx;
        std::vector<int32_t> single{42};
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, single), db, ctx);
        CHECK(sql == "u.id IN ($1)");
    }

    SECTION("edge case — empty container") {
        atlas::serialize_context ctx;
        std::vector<int32_t> empty;
        // Serialiser must not crash; exact output is implementation-defined
        // but must be syntactically valid or contain an error marker.
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, empty), db, ctx);
        CHECK(!sql.empty());
    }
}

// ---------------------------------------------------------------------------
// TEST: serialize_predicate — boolean combinators
// ---------------------------------------------------------------------------

TEST_CASE("serialize_predicate handles and_, or_, not_",
          "[sql_serialize][predicate][combinators]") {
    auto db = make_db();

    SECTION("and_ wraps in parentheses") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::and_(atlas::gt(&User::age, 18),
                        atlas::like(&User::email, std::string{"%@corp.com"})),
            db, ctx);

        CHECK(sql == "(u.age > $1 AND u.email LIKE $2)");
        REQUIRE(ctx.params.size() == 2u);
        CHECK(ctx.params[0] == "18");
        CHECK(ctx.params[1] == "%@corp.com");
    }

    SECTION("or_ wraps in parentheses") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::or_(atlas::eq(&User::id, 1), atlas::eq(&User::id, 2)),
            db, ctx);

        CHECK(sql == "(u.id = $1 OR u.id = $2)");
    }

    SECTION("not_ emits NOT (...)") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::not_(atlas::eq(&User::id, 0)), db, ctx);

        CHECK(sql == "NOT (u.id = $1)");
    }

    SECTION("nested and_ + or_") {
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::and_(
                atlas::or_(atlas::eq(&User::id,1), atlas::eq(&User::id,2)),
                atlas::gt(&User::age, 18)
            ),
            db, ctx);

        CHECK(sql == "((u.id = $1 OR u.id = $2) AND u.age > $3)");
        REQUIRE(ctx.params.size() == 3u);
    }
}

// ---------------------------------------------------------------------------
// TEST: serialize_aggregate
// ---------------------------------------------------------------------------

TEST_CASE("serialize_aggregate produces correct SQL function fragments",
          "[sql_serialize][aggregate]") {
    auto db = make_db();

    SECTION("COUNT(col)") {
        auto sql = atlas::serialize_aggregate(atlas::count(&User::id), db);
        CHECK(sql == "COUNT(u.id)");
    }

    SECTION("COUNT(*)") {
        auto sql = atlas::serialize_aggregate(atlas::count(), db);
        CHECK(sql == "COUNT(*)");
    }

    SECTION("SUM(score)") {
        auto sql = atlas::serialize_aggregate(atlas::sum(&User::score), db);
        CHECK(sql == "SUM(u.score)");
    }

    SECTION("AVG(score)") {
        auto sql = atlas::serialize_aggregate(atlas::avg(&User::score), db);
        CHECK(sql == "AVG(u.score)");
    }

    SECTION("MIN(age)") {
        auto sql = atlas::serialize_aggregate(atlas::min(&User::age), db);
        CHECK(sql == "MIN(u.age)");
    }

    SECTION("MAX(age)") {
        auto sql = atlas::serialize_aggregate(atlas::max(&User::age), db);
        CHECK(sql == "MAX(u.age)");
    }
}
