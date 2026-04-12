// Tests for atlas/query/join.hpp + join integration in select_query.
//
// Verifies join_clause struct, is_join_clause concept, and select_query
// inner_join / left_join methods.
// Link-time failure expected until implementations are provided.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/join.hpp"
#include "atlas/query/predicate.hpp"
#include "atlas/query/select.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>

// ---------------------------------------------------------------------------
// Test entities & schema
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
};

struct Post {
    int32_t     id{};
    int32_t     user_id{};
    std::string title{};
};

static auto make_db() {
    auto user_table = atlas::make_table<User>("users",
        atlas::make_column("id",   &User::id,   atlas::primary_key()),
        atlas::make_column("name", &User::name, atlas::not_null())
    );
    auto post_table = atlas::make_table<Post>("posts",
        atlas::make_column("id",      &Post::id,      atlas::primary_key()),
        atlas::make_column("user_id", &Post::user_id, atlas::not_null()),
        atlas::make_column("title",   &Post::title,   atlas::not_null())
    );
    return atlas::make_storage(user_table, post_table);
}

// ---------------------------------------------------------------------------
// TEST: join_clause struct
// ---------------------------------------------------------------------------

TEST_CASE("join_clause stores join kind and ON predicate", "[join][join_clause]") {

    SECTION("happy path — inner join with eq predicate") {
        using on_pred_t = atlas::eq_expr<
            atlas::column_ref<Post, int32_t>,
            atlas::column_ref<User, int32_t>>;

        on_pred_t on_pred{
            atlas::column_ref<Post, int32_t>{&Post::user_id},
            atlas::column_ref<User, int32_t>{&User::id}
        };

        atlas::join_clause<Post, on_pred_t, atlas::join_kind::inner> jc{on_pred};

        static_assert(jc.kind == atlas::join_kind::inner);
        CHECK(jc.on.lhs.ptr == &Post::user_id);
        CHECK(jc.on.rhs.ptr == &User::id);
    }

    SECTION("edge case — left join kind") {
        using on_pred_t = atlas::eq_expr<
            atlas::column_ref<Post, int32_t>,
            atlas::column_ref<User, int32_t>>;

        atlas::join_clause<Post, on_pred_t, atlas::join_kind::left> jc{
            on_pred_t{
                atlas::column_ref<Post, int32_t>{&Post::user_id},
                atlas::column_ref<User, int32_t>{&User::id}
            }
        };

        static_assert(jc.kind == atlas::join_kind::left);
    }
}

// ---------------------------------------------------------------------------
// TEST: is_join_clause concept
// ---------------------------------------------------------------------------

TEST_CASE("is_join_clause concept accepts join_clause and rejects others",
          "[join][concept]") {

    using on_pred_t = atlas::eq_expr<
        atlas::column_ref<Post, int32_t>,
        atlas::column_ref<User, int32_t>>;

    static_assert(atlas::is_join_clause<
        atlas::join_clause<Post, on_pred_t, atlas::join_kind::inner>>);
    static_assert(atlas::is_join_clause<
        atlas::join_clause<Post, on_pred_t, atlas::join_kind::left>>);

    static_assert(!atlas::is_join_clause<int>);
    static_assert(!atlas::is_join_clause<on_pred_t>);

    CHECK(true);
}

// ---------------------------------------------------------------------------
// TEST: select_query.inner_join()
// ---------------------------------------------------------------------------

TEST_CASE("inner_join() appends a join_clause to JoinsTuple", "[join][inner_join]") {

    SECTION("happy path — one INNER JOIN") {
        auto q = atlas::select(&User::name, &Post::title)
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));

        static_assert(std::tuple_size_v<decltype(q.joins)> == 1u);

        const auto& jc = std::get<0>(q.joins);
        static_assert(jc.kind == atlas::join_kind::inner);
        CHECK(jc.on.lhs.ptr == &Post::user_id);
    }

    SECTION("edge case — two JOINs chained") {
        struct Comment {
            int32_t id{};
            int32_t post_id{};
        };

        auto q = atlas::select(&User::name)
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id))
            .inner_join<Comment>(atlas::eq(&Comment::post_id, &Post::id));

        static_assert(std::tuple_size_v<decltype(q.joins)> == 2u);
    }
}

// ---------------------------------------------------------------------------
// TEST: select_query.left_join()
// ---------------------------------------------------------------------------

TEST_CASE("left_join() appends a left join_clause", "[join][left_join]") {

    SECTION("happy path") {
        auto q = atlas::select(&User::name, &Post::title)
            .from<User>()
            .left_join<Post>(atlas::eq(&Post::user_id, &User::id));

        const auto& jc = std::get<0>(q.joins);
        static_assert(jc.kind == atlas::join_kind::left);
    }
}

// ---------------------------------------------------------------------------
// TEST: to_sql() with JOIN (link-time stub)
// ---------------------------------------------------------------------------

TEST_CASE("select with inner_join produces correct SQL", "[join][to_sql]") {
    auto db = make_db();

    SECTION("happy path — INNER JOIN with ON") {
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));
        auto sql = q.to_sql(db);

        CHECK(sql == "SELECT u.name, p.title FROM users u INNER JOIN posts p ON p.user_id = u.id");
    }

    SECTION("edge case — LEFT JOIN produces LEFT JOIN keyword") {
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .left_join<Post>(atlas::eq(&Post::user_id, &User::id));
        auto sql = q.to_sql(db);

        CHECK(sql.find("LEFT JOIN") != std::string::npos);
    }

    SECTION("edge case — JOIN + WHERE clause") {
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .inner_join<Post>(atlas::eq(&Post::user_id, &User::id))
                       .where(atlas::eq(&User::id, 5));
        auto sql  = q.to_sql(db);
        auto prm  = q.params();

        CHECK(sql.find("INNER JOIN") != std::string::npos);
        CHECK(sql.find("WHERE u.id = $1") != std::string::npos);
        CHECK(prm == std::vector<std::string>{"5"});
    }
}
