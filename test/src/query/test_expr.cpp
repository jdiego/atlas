// Tests for atlas/query/expr.hpp
//
// Tests compile against the declared interfaces. They will fail at link time
// until the developer fills in the function bodies.

#include <catch2/catch_test_macros.hpp>

#include "atlas/query/expr.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

// ---------------------------------------------------------------------------
// Test entity
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
};

// ---------------------------------------------------------------------------
// TEST: col() factory
// ---------------------------------------------------------------------------

TEST_CASE("col() wraps a member pointer in column_ref", "[expr][col]") {

    SECTION("happy path — int32_t member") {
        auto c = atlas::col(&User::id);

        static_assert(
            std::is_same_v<decltype(c), atlas::column_ref<User, int32_t>>,
            "col() must return column_ref<Entity, T>");

        REQUIRE(c.ptr == &User::id);
    }

    SECTION("happy path — std::string member") {
        auto c = atlas::col(&User::email);

        static_assert(
            std::is_same_v<decltype(c), atlas::column_ref<User, std::string>>,
            "col() must deduce T = std::string");

        REQUIRE(c.ptr == &User::email);
    }

    SECTION("edge case — two different members produce different column_refs") {
        auto c1 = atlas::col(&User::id);
        auto c2 = atlas::col(&User::age);

        REQUIRE(c1.ptr != c2.ptr);
    }
}

// ---------------------------------------------------------------------------
// TEST: lit() factory
// ---------------------------------------------------------------------------

TEST_CASE("lit() wraps a value in literal<T>", "[expr][lit]") {

    SECTION("happy path — int literal") {
        auto l = atlas::lit(42);

        static_assert(
            std::is_same_v<decltype(l), atlas::literal<int>>,
            "lit() must return literal<remove_cvref_t<T>>");

        REQUIRE(l.value == 42);
    }

    SECTION("happy path — string_view decays to std::string_view") {
        auto l = atlas::lit(std::string_view{"hello"});

        static_assert(
            std::is_same_v<decltype(l), atlas::literal<std::string_view>>);

        REQUIRE(l.value == "hello");
    }

    SECTION("edge case — const ref decays (remove_cvref_t)") {
        const int x = 7;
        auto l = atlas::lit(x);

        // T should be int, not const int
        static_assert(std::is_same_v<decltype(l.value), int>);
        REQUIRE(l.value == 7);
    }

    SECTION("edge case — rvalue string moves correctly") {
        auto l = atlas::lit(std::string{"world"});

        static_assert(std::is_same_v<decltype(l), atlas::literal<std::string>>);
        REQUIRE(l.value == "world");
    }
}

// ---------------------------------------------------------------------------
// TEST: column_ref concept
// ---------------------------------------------------------------------------

TEST_CASE("is_column_ref concept detects column_ref", "[expr][concept]") {

    SECTION("column_ref<User,int> satisfies is_column_ref") {
        static_assert(atlas::is_column_ref<atlas::column_ref<User, int32_t>>);
    }

    SECTION("literal<int> does not satisfy is_column_ref") {
        static_assert(!atlas::is_column_ref<atlas::literal<int>>);
    }

    SECTION("raw member pointer does not satisfy is_column_ref") {
        static_assert(!atlas::is_column_ref<decltype(&User::id)>);
    }
}

// ---------------------------------------------------------------------------
// TEST: is_literal concept
// ---------------------------------------------------------------------------

TEST_CASE("is_literal concept detects literal", "[expr][concept]") {

    SECTION("literal<int> satisfies is_literal") {
        static_assert(atlas::is_literal<atlas::literal<int>>);
    }

    SECTION("column_ref does not satisfy is_literal") {
        static_assert(!atlas::is_literal<atlas::column_ref<User, int32_t>>);
    }

    SECTION("int does not satisfy is_literal") {
        static_assert(!atlas::is_literal<int>);
    }
}

// ---------------------------------------------------------------------------
// TEST: column_eq_ref carries both member pointers
// ---------------------------------------------------------------------------

TEST_CASE("column_eq_ref stores cross-table column equality", "[expr][column_eq_ref]") {

    struct Post {
        int32_t id{};
        int32_t user_id{};
    };

    SECTION("happy path — construct directly") {
        atlas::column_eq_ref<Post, int32_t, User, int32_t> ref{
            &Post::user_id, &User::id
        };

        REQUIRE(ref.lhs == &Post::user_id);
        REQUIRE(ref.rhs == &User::id);
    }

    SECTION("edge case — same type, different entity") {
        atlas::column_eq_ref<User, int32_t, User, int32_t> ref{
            &User::id, &User::age
        };

        REQUIRE(ref.lhs != ref.rhs);
    }
}
