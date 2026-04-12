// Tests for atlas/query/aggregate.hpp
//
// Verifies aggregate node types, factories, is_aggregate concept, and
// integration with atlas::select().

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/aggregate.hpp"
#include "atlas/query/select.hpp"
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
    double      score{};
    int32_t     age{};
};

static auto make_db() {
    return atlas::make_storage(
        atlas::make_table<User>("users",
            atlas::make_column("id",    &User::id,    atlas::primary_key()),
            atlas::make_column("name",  &User::name,  atlas::not_null()),
            atlas::make_column("score", &User::score, atlas::not_null()),
            atlas::make_column("age",   &User::age,   atlas::not_null())
        )
    );
}

// ---------------------------------------------------------------------------
// TEST: count(member_ptr)
// ---------------------------------------------------------------------------

TEST_CASE("count(col) wraps column_ref in count_expr", "[aggregate][count]") {

    SECTION("happy path") {
        auto agg = atlas::count(&User::id);
        using expected_t = atlas::count_expr<atlas::column_ref<User, int32_t>>;
        static_assert(std::is_same_v<decltype(agg), expected_t>);
        CHECK(agg.col.ptr == &User::id);
    }

    SECTION("edge case — count on string column") {
        auto agg = atlas::count(&User::name);
        static_assert(std::is_same_v<decltype(agg),
            atlas::count_expr<atlas::column_ref<User, std::string>>>);
    }
}

// ---------------------------------------------------------------------------
// TEST: count() — count star
// ---------------------------------------------------------------------------

TEST_CASE("count() with no args returns count_star_expr", "[aggregate][count_star]") {

    SECTION("happy path") {
        auto agg = atlas::count();
        static_assert(std::is_same_v<decltype(agg), atlas::count_star_expr>);
    }

    SECTION("is_aggregate concept satisfied") {
        static_assert(atlas::is_aggregate<atlas::count_star_expr>);
    }
}

// ---------------------------------------------------------------------------
// TEST: sum, avg, min, max
// ---------------------------------------------------------------------------

TEST_CASE("sum/avg/min/max factories produce correct node types", "[aggregate][agg_fns]") {

    SECTION("sum") {
        auto agg = atlas::sum(&User::score);
        static_assert(std::is_same_v<decltype(agg),
            atlas::sum_expr<atlas::column_ref<User, double>>>);
        CHECK(agg.col.ptr == &User::score);
    }

    SECTION("avg") {
        auto agg = atlas::avg(&User::score);
        static_assert(std::is_same_v<decltype(agg),
            atlas::avg_expr<atlas::column_ref<User, double>>>);
    }

    SECTION("min") {
        auto agg = atlas::min(&User::age);
        static_assert(std::is_same_v<decltype(agg),
            atlas::min_expr<atlas::column_ref<User, int32_t>>>);
    }

    SECTION("max") {
        auto agg = atlas::max(&User::age);
        static_assert(std::is_same_v<decltype(agg),
            atlas::max_expr<atlas::column_ref<User, int32_t>>>);
    }
}

// ---------------------------------------------------------------------------
// TEST: is_aggregate concept
// ---------------------------------------------------------------------------

TEST_CASE("is_aggregate concept accepts all node types and rejects others",
          "[aggregate][concept]") {

    static_assert(atlas::is_aggregate<atlas::count_expr<atlas::column_ref<User,int32_t>>>);
    static_assert(atlas::is_aggregate<atlas::sum_expr<atlas::column_ref<User,double>>>);
    static_assert(atlas::is_aggregate<atlas::avg_expr<atlas::column_ref<User,double>>>);
    static_assert(atlas::is_aggregate<atlas::min_expr<atlas::column_ref<User,int32_t>>>);
    static_assert(atlas::is_aggregate<atlas::max_expr<atlas::column_ref<User,int32_t>>>);
    static_assert(atlas::is_aggregate<atlas::count_star_expr>);

    // Non-aggregates
    static_assert(!atlas::is_aggregate<int>);
    static_assert(!atlas::is_aggregate<atlas::column_ref<User, int32_t>>);
    static_assert(!atlas::is_aggregate<atlas::literal<int>>);

    CHECK(true);
}

// ---------------------------------------------------------------------------
// TEST: aggregate in select()
// ---------------------------------------------------------------------------

TEST_CASE("select() accepts aggregate expressions alongside column_refs",
          "[aggregate][select_integration]") {

    SECTION("happy path — COUNT(*) in select") {
        auto q = atlas::select(atlas::count())
            .from<User>()
            .where(atlas::gt(&User::age, 18));

        static_assert(atlas::is_aggregate<
            std::tuple_element_t<0, decltype(q.col_refs)>>);
    }

    SECTION("happy path — COUNT(col) in select") {
        auto q = atlas::select(atlas::count(&User::id))
            .from<User>();

        static_assert(atlas::is_aggregate<
            std::tuple_element_t<0, decltype(q.col_refs)>>);
    }

    SECTION("to_sql() — COUNT(*) with WHERE (link-time stub)") {
        auto db = make_db();
        auto q  = atlas::select(atlas::count())
                      .from<User>()
                      .where(atlas::gt(&User::age, 18));

        std::string sql = q.to_sql(db);
        auto prm        = q.params();

        CHECK(sql == "SELECT COUNT(*) FROM users u WHERE u.age > $1");
        CHECK(prm == std::vector<std::string>{"18"});
    }
}
