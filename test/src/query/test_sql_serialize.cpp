// Tests for atlas/query/sql_serialize.hpp + sql_serialize.cpp
//
// Uses mock storage (no libpq).
// serialize_context::next_param tests will fail at link time until
// sql_serialize.cpp is implemented.
// serialize_column_ref and serialize_predicate tests require implementation.

#include <boost/ut.hpp>

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

namespace ut = boost::ut;

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

ut::suite<"query/sql_serialize"> sql_serialize_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // serialize_context::next_param
    // -----------------------------------------------------------------------

    "next_param first call returns $1 and stores value"_test = [] {
        atlas::serialize_context ctx;
        auto placeholder = ctx.next_param("42");

        expect(placeholder == "$1");
        expect(ctx.params.size() == 1_u);
        expect(ctx.params[0] == "42");
        expect(ctx.param_counter == 2);
    };

    "next_param sequential calls return $1, $2, $3"_test = [] {
        atlas::serialize_context ctx;

        auto p1 = ctx.next_param("first");
        auto p2 = ctx.next_param("second");
        auto p3 = ctx.next_param("third");

        expect(p1 == "$1");
        expect(p2 == "$2");
        expect(p3 == "$3");
        expect(ctx.params.size() == 3_u);
        expect(ctx.params[0] == "first");
        expect(ctx.params[1] == "second");
        expect(ctx.params[2] == "third");
    };

    "next_param accepts empty string value"_test = [] {
        atlas::serialize_context ctx;
        auto p = ctx.next_param("");
        expect(p == "$1");
        expect(ctx.params[0].empty());
    };

    "next_param increments param_counter monotonically"_test = [] {
        atlas::serialize_context ctx;
        ctx.next_param("a");
        ctx.next_param("b");
        ctx.next_param("c");
        expect(ctx.param_counter == 4);
    };

    // -----------------------------------------------------------------------
    // serialize_column_ref
    // -----------------------------------------------------------------------

    "serialize_column_ref emits u.id for users.id"_test = [] {
        auto db = make_db();
        atlas::column_ref<User, int32_t> ref{&User::id};
        auto sql = atlas::serialize_column_ref(ref, db);
        expect(sql == "u.id");
    };

    "serialize_column_ref emits u.email for users.email"_test = [] {
        auto db = make_db();
        atlas::column_ref<User, std::string> ref{&User::email};
        auto sql = atlas::serialize_column_ref(ref, db);
        expect(sql == "u.email");
    };

    "serialize_column_ref emits p.user_id for posts.user_id"_test = [] {
        auto db = make_db();
        atlas::column_ref<Post, int32_t> ref{&Post::user_id};
        auto sql = atlas::serialize_column_ref(ref, db);
        expect(sql == "p.user_id");
    };

    // -----------------------------------------------------------------------
    // serialize_predicate — leaf nodes
    // -----------------------------------------------------------------------

    "serialize_predicate eq emits 'alias.col = $1'"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto pred = atlas::eq(&User::id, 1);
        auto sql  = atlas::serialize_predicate(pred, db, ctx);

        expect(sql == "u.id = $1");
        expect(ctx.params.size() == 1_u);
        expect(ctx.params[0] == "1");
    };

    "serialize_predicate ne"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto pred = atlas::ne(&User::age, 18);
        auto sql  = atlas::serialize_predicate(pred, db, ctx);
        expect(sql == "u.age != $1");
    };

    "serialize_predicate lt"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::lt(&User::age, 30), db, ctx);
        expect(sql == "u.age < $1");
    };

    "serialize_predicate gt"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::gt(&User::age, 18), db, ctx);
        expect(sql == "u.age > $1");
    };

    "serialize_predicate lte"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::lte(&User::age, 65), db, ctx);
        expect(sql == "u.age <= $1");
    };

    "serialize_predicate gte"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::gte(&User::score, 0.0), db, ctx);
        expect(sql == "u.score >= $1");
    };

    "serialize_predicate like"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::like(&User::email, std::string{"%@corp.com"}), db, ctx);
        expect(sql == "u.email LIKE $1");
        expect(ctx.params[0] == "%@corp.com");
    };

    "serialize_predicate is_null"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::is_null(&User::name), db, ctx);
        expect(sql == "u.name IS NULL");
        expect(ctx.params.empty());
    };

    "serialize_predicate is_not_null"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(atlas::is_not_null(&User::name), db, ctx);
        expect(sql == "u.name IS NOT NULL");
        expect(ctx.params.empty());
    };

    // -----------------------------------------------------------------------
    // serialize_predicate — in_expr
    // -----------------------------------------------------------------------

    "serialize_predicate in with three values"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        std::vector<int32_t> ids{1, 2, 3};
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, ids), db, ctx);
        expect(sql == "u.id IN ($1, $2, $3)");
        expect(ctx.params.size() == 3_u);
    };

    "serialize_predicate in with single value"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        std::vector<int32_t> single{42};
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, single), db, ctx);
        expect(sql == "u.id IN ($1)");
    };

    "serialize_predicate in with empty container does not crash"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        std::vector<int32_t> empty;
        auto sql = atlas::serialize_predicate(atlas::in(&User::id, empty), db, ctx);
        expect(!sql.empty());
    };

    // -----------------------------------------------------------------------
    // serialize_predicate — boolean combinators
    // -----------------------------------------------------------------------

    "serialize_predicate and_ wraps in parentheses"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::and_(atlas::gt(&User::age, 18),
                        atlas::like(&User::email, std::string{"%@corp.com"})),
            db, ctx);

        expect(sql == "(u.age > $1 AND u.email LIKE $2)");
        expect(ctx.params.size() == 2_u);
        expect(ctx.params[0] == "18");
        expect(ctx.params[1] == "%@corp.com");
    };

    "serialize_predicate or_ wraps in parentheses"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::or_(atlas::eq(&User::id, 1), atlas::eq(&User::id, 2)),
            db, ctx);

        expect(sql == "(u.id = $1 OR u.id = $2)");
    };

    "serialize_predicate not_ emits NOT (...)"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::not_(atlas::eq(&User::id, 0)), db, ctx);

        expect(sql == "NOT (u.id = $1)");
    };

    "serialize_predicate nested and_/or_"_test = [] {
        auto db = make_db();
        atlas::serialize_context ctx;
        auto sql = atlas::serialize_predicate(
            atlas::and_(
                atlas::or_(atlas::eq(&User::id, 1), atlas::eq(&User::id, 2)),
                atlas::gt(&User::age, 18)
            ),
            db, ctx);

        expect(sql == "((u.id = $1 OR u.id = $2) AND u.age > $3)");
        expect(ctx.params.size() == 3_u);
    };

    // -----------------------------------------------------------------------
    // serialize_aggregate
    // -----------------------------------------------------------------------

    "serialize_aggregate COUNT(col)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::count(&User::id), db);
        expect(sql == "COUNT(u.id)");
    };

    "serialize_aggregate COUNT(*)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::count(), db);
        expect(sql == "COUNT(*)");
    };

    "serialize_aggregate SUM(score)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::sum(&User::score), db);
        expect(sql == "SUM(u.score)");
    };

    "serialize_aggregate AVG(score)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::avg(&User::score), db);
        expect(sql == "AVG(u.score)");
    };

    "serialize_aggregate MIN(age)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::min(&User::age), db);
        expect(sql == "MIN(u.age)");
    };

    "serialize_aggregate MAX(age)"_test = [] {
        auto db = make_db();
        auto sql = atlas::serialize_aggregate(atlas::max(&User::age), db);
        expect(sql == "MAX(u.age)");
    };
};
