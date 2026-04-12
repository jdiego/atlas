// Tests for atlas/query/predicate.hpp
//
// Verifies factory return types, node field values, is_predicate concept, and
// composition via and_/or_/not_.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/predicate.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// Test entity
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
    double      score{};
};

// ---------------------------------------------------------------------------
// TEST: eq factory
// ---------------------------------------------------------------------------

TEST_CASE("eq() produces eq_expr with correct lhs/rhs types", "[predicate][eq]") {

    SECTION("happy path — int column, int value") {
        auto pred = atlas::eq(&User::id, 42);

        using expected_t = atlas::eq_expr<
            atlas::column_ref<User, int32_t>,
            atlas::literal<int>>;
        static_assert(std::is_same_v<decltype(pred), expected_t>);

        REQUIRE(pred.lhs.ptr == &User::id);
        REQUIRE(pred.rhs.value == 42);
    }

    SECTION("happy path — string column, string value") {
        auto pred = atlas::eq(&User::email, std::string{"a@b.com"});

        REQUIRE(pred.lhs.ptr == &User::email);
        REQUIRE(pred.rhs.value == "a@b.com");
    }

    SECTION("edge case — value zero") {
        auto pred = atlas::eq(&User::age, 0);
        REQUIRE(pred.rhs.value == 0);
    }
}

// ---------------------------------------------------------------------------
// TEST: ne, lt, gt, lte, gte, like factories
// ---------------------------------------------------------------------------

TEST_CASE("comparison factory functions produce the correct node types", "[predicate][comparison]") {

    SECTION("ne") {
        auto p = atlas::ne(&User::age, 18);
        static_assert(std::is_same_v<decltype(p),
            atlas::ne_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
        REQUIRE(p.lhs.ptr == &User::age);
    }

    SECTION("lt") {
        auto p = atlas::lt(&User::age, 30);
        static_assert(std::is_same_v<decltype(p),
            atlas::lt_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
    }

    SECTION("gt") {
        auto p = atlas::gt(&User::score, 0.5);
        static_assert(std::is_same_v<decltype(p),
            atlas::gt_expr<atlas::column_ref<User,double>, atlas::literal<double>>>);
        REQUIRE(p.rhs.value == 0.5);
    }

    SECTION("lte") {
        auto p = atlas::lte(&User::age, 65);
        static_assert(std::is_same_v<decltype(p),
            atlas::lte_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
    }

    SECTION("gte") {
        auto p = atlas::gte(&User::id, 1);
        static_assert(std::is_same_v<decltype(p),
            atlas::gte_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
    }

    SECTION("like — pattern string") {
        auto p = atlas::like(&User::email, std::string{"%@corp.com"});
        static_assert(std::is_same_v<decltype(p),
            atlas::like_expr<
                atlas::column_ref<User,std::string>,
                atlas::literal<std::string>>>);
        REQUIRE(p.rhs.value == "%@corp.com");
    }
}

// ---------------------------------------------------------------------------
// TEST: is_null / is_not_null
// ---------------------------------------------------------------------------

TEST_CASE("is_null and is_not_null factories", "[predicate][null]") {

    SECTION("is_null — correct type") {
        auto p = atlas::is_null(&User::name);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_null_expr<atlas::column_ref<User, std::string>>>);
        REQUIRE(p.col.ptr == &User::name);
    }

    SECTION("is_not_null — correct type") {
        auto p = atlas::is_not_null(&User::name);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_not_null_expr<atlas::column_ref<User, std::string>>>);
        REQUIRE(p.col.ptr == &User::name);
    }

    SECTION("edge case — is_null on integer column") {
        auto p = atlas::is_null(&User::id);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_null_expr<atlas::column_ref<User, int32_t>>>);
    }
}

// ---------------------------------------------------------------------------
// TEST: in factory
// ---------------------------------------------------------------------------

TEST_CASE("in() factory stores column ref and values container", "[predicate][in]") {

    SECTION("happy path — vector of ints") {
        std::vector<int32_t> ids{1, 2, 3};
        auto p = atlas::in(&User::id, ids);

        static_assert(std::is_same_v<decltype(p),
            atlas::in_expr<
                atlas::column_ref<User, int32_t>,
                std::vector<int32_t>>>);

        REQUIRE(p.col.ptr == &User::id);
        REQUIRE(p.values.size() == 3u);
    }

    SECTION("edge case — empty container") {
        std::vector<int32_t> empty;
        auto p = atlas::in(&User::id, empty);
        REQUIRE(p.values.empty());
    }

    SECTION("edge case — single-element container") {
        std::vector<int32_t> one{42};
        auto p = atlas::in(&User::id, std::move(one));
        REQUIRE(p.values.size() == 1u);
        REQUIRE(p.values[0] == 42);
    }
}

// ---------------------------------------------------------------------------
// TEST: and_ / or_ / not_ combinators
// ---------------------------------------------------------------------------

TEST_CASE("and_() combines two predicates into and_expr", "[predicate][combinator]") {

    SECTION("happy path — two leaf predicates") {
        auto p = atlas::and_(
            atlas::gt(&User::age, 18),
            atlas::like(&User::email, std::string{"%@corp.com"})
        );

        static_assert(atlas::is_predicate<decltype(p)>);
        static_assert(std::is_same_v<
            decltype(p.lhs),
            atlas::gt_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
    }

    SECTION("edge case — nested and_") {
        auto p = atlas::and_(
            atlas::and_(atlas::eq(&User::id, 1), atlas::eq(&User::age, 30)),
            atlas::eq(&User::id, 2)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
    }
}

TEST_CASE("or_() combines two predicates into or_expr", "[predicate][combinator]") {

    SECTION("happy path") {
        auto p = atlas::or_(
            atlas::eq(&User::id, 1),
            atlas::eq(&User::id, 2)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
    }

    SECTION("edge case — or_ of and_ nodes") {
        auto p = atlas::or_(
            atlas::and_(atlas::eq(&User::id,1), atlas::eq(&User::age,20)),
            atlas::eq(&User::id, 99)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
    }
}

TEST_CASE("not_() wraps a predicate in not_expr", "[predicate][combinator]") {

    SECTION("happy path") {
        auto p = atlas::not_(atlas::eq(&User::id, 0));
        static_assert(atlas::is_predicate<decltype(p)>);
    }

    SECTION("edge case — double negation") {
        auto p = atlas::not_(atlas::not_(atlas::eq(&User::id, 1)));
        static_assert(atlas::is_predicate<decltype(p)>);
    }
}

// ---------------------------------------------------------------------------
// TEST: is_predicate concept
// ---------------------------------------------------------------------------

TEST_CASE("is_predicate concept accepts all node types and rejects non-nodes",
          "[predicate][concept]") {

    static_assert(atlas::is_predicate<atlas::eq_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>);
    static_assert(atlas::is_predicate<atlas::and_expr<
        atlas::eq_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>,
        atlas::eq_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>>);
    static_assert(atlas::is_predicate<atlas::not_expr<
        atlas::eq_expr<atlas::column_ref<User,int32_t>, atlas::literal<int>>>>);

    // Non-predicates
    static_assert(!atlas::is_predicate<int>);
    static_assert(!atlas::is_predicate<atlas::column_ref<User,int32_t>>);
    static_assert(!atlas::is_predicate<atlas::literal<int>>);

    CHECK(true); // all static_asserts passed
}
