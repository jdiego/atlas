// Tests for atlas/query/select.hpp
//
// Uses mock storage (no libpq). to_sql() / params() calls are present and
// will fail at link time until implementations are provided.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/select.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

// ---------------------------------------------------------------------------
// Test entities & schema
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
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
        atlas::make_column("age",   &User::age,   atlas::not_null())
    );
    auto post_table = atlas::make_table<Post>("posts",
        atlas::make_column("id",      &Post::id,      atlas::primary_key()),
        atlas::make_column("user_id", &Post::user_id, atlas::not_null()),
        atlas::make_column("title",   &Post::title,   atlas::not_null())
    );
    return atlas::make_storage(user_table, post_table);
}

// ---------------------------------------------------------------------------
// TEST: atlas::select() factory
// ---------------------------------------------------------------------------

TEST_CASE("select() factory wraps member pointers in column_refs", "[select][factory]") {

    SECTION("happy path — two columns") {
        auto q = atlas::select(&User::id, &User::name);

        using expected_col_refs = std::tuple<
            atlas::column_ref<User, int32_t>,
            atlas::column_ref<User, std::string>>;
        static_assert(std::is_same_v<
            decltype(q.col_refs), expected_col_refs>);
    }

    SECTION("happy path — single column") {
        auto q = atlas::select(&User::email);
        static_assert(std::is_same_v<
            decltype(q.col_refs),
            std::tuple<atlas::column_ref<User, std::string>>>);
    }

    SECTION("edge case — initial state has monostate for FromEntity and WherePred") {
        auto q = atlas::select(&User::id);
        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, std::monostate>);
        static_assert(!q.limit_n.has_value());
        static_assert(!q.offset_n.has_value());
    }
}

// ---------------------------------------------------------------------------
// TEST: .from<Entity>()
// ---------------------------------------------------------------------------

TEST_CASE("from() stamps the FromEntity type parameter", "[select][from]") {

    SECTION("happy path") {
        auto q = atlas::select(&User::id).from<User>();

        // FromEntity is now User; the select_query_impl type changes.
        // Verify via the type of the returned object's where_pred (monostate).
        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, std::monostate>);
    }
}

// ---------------------------------------------------------------------------
// TEST: .where()
// ---------------------------------------------------------------------------

TEST_CASE("where() attaches a predicate and changes WherePred type", "[select][where]") {

    SECTION("happy path — simple eq predicate") {
        auto pred = atlas::eq(&User::id, 1);
        auto q    = atlas::select(&User::id).from<User>().where(pred);

        using expected_pred = atlas::eq_expr<
            atlas::column_ref<User, int32_t>,
            atlas::literal<int>>;
        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, expected_pred>);
    }

    SECTION("edge case — and_ combinator as predicate") {
        auto q = atlas::select(&User::name)
            .from<User>()
            .where(atlas::and_(
                atlas::gt(&User::age, 18),
                atlas::like(&User::email, std::string{"%@corp.com"})
            ));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
    }
}

// ---------------------------------------------------------------------------
// TEST: .order_by()
// ---------------------------------------------------------------------------

TEST_CASE("order_by() appends an order_by_clause to the tuple", "[select][order_by]") {

    SECTION("happy path — single column ascending") {
        auto q = atlas::select(&User::name)
            .from<User>()
            .order_by(&User::name);

        // OrderByTuple should have one element
        static_assert(std::tuple_size_v<decltype(q.order_cols)> == 1u);
    }

    SECTION("happy path — two order-by columns") {
        auto q = atlas::select(&User::id, &User::name)
            .from<User>()
            .order_by(&User::name)
            .order_by(&User::id, false);  // descending

        static_assert(std::tuple_size_v<decltype(q.order_cols)> == 2u);
    }

    SECTION("edge case — descending flag stored correctly") {
        auto q = atlas::select(&User::age)
            .from<User>()
            .order_by(&User::age, false);

        const auto& clause = std::get<0>(q.order_cols);
        CHECK(clause.ascending == false);
        CHECK(clause.col == &User::age);
    }
}

// ---------------------------------------------------------------------------
// TEST: .limit() and .offset()
// ---------------------------------------------------------------------------

TEST_CASE("limit() and offset() store optional values", "[select][limit_offset]") {

    SECTION("happy path — both set") {
        auto q = atlas::select(&User::id)
            .from<User>()
            .limit(50)
            .offset(10);

        REQUIRE(q.limit_n.has_value());
        REQUIRE(q.offset_n.has_value());
        CHECK(*q.limit_n  == 50u);
        CHECK(*q.offset_n == 10u);
    }

    SECTION("edge case — only limit set") {
        auto q = atlas::select(&User::id).from<User>().limit(1);
        CHECK(q.limit_n.has_value());
        CHECK(!q.offset_n.has_value());
    }

    SECTION("edge case — limit(0) is stored even though semantically questionable") {
        auto q = atlas::select(&User::id).from<User>().limit(0);
        CHECK(q.limit_n.has_value());
        CHECK(*q.limit_n == 0u);
    }
}

// ---------------------------------------------------------------------------
// TEST: .to_sql() and .params() — specification tests
// (Link-time failure expected until implementation is provided.)
// ---------------------------------------------------------------------------

TEST_CASE("to_sql() produces correct parameterised SQL for SELECT", "[select][to_sql]") {
    auto db = make_db();

    SECTION("happy path — WHERE + LIMIT + OFFSET") {
        auto q = atlas::select(&User::id, &User::name)
            .from<User>()
            .where(atlas::and_(
                atlas::gt(&User::age, 18),
                atlas::like(&User::email, std::string{"%@corp.com"})
            ))
            .order_by(&User::name)
            .limit(50)
            .offset(10);

        std::string sql    = q.to_sql(db);
        auto        params = q.params();

        CHECK(sql    == "SELECT u.id, u.name FROM users u WHERE (u.age > $1 AND u.email LIKE $2) ORDER BY u.name ASC LIMIT 50 OFFSET 10");
        CHECK(params == std::vector<std::string>{"18", "%@corp.com"});
    }

    SECTION("edge case — no WHERE clause") {
        auto q = atlas::select(&User::id).from<User>();
        std::string sql = q.to_sql(db);
        CHECK(sql.find("WHERE") == std::string::npos);
    }

    SECTION("edge case — no LIMIT") {
        auto q = atlas::select(&User::id).from<User>().where(atlas::eq(&User::id, 1));
        std::string sql = q.to_sql(db);
        CHECK(sql.find("LIMIT") == std::string::npos);
    }
}
