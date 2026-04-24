// Tests for atlas/query/expr.hpp
//
// Tests compile against the declared interfaces. They will fail at link time
// until the developer fills in the function bodies.

#include <boost/ut.hpp>

#include "atlas/query/expr.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace ut = boost::ut;

// ---------------------------------------------------------------------------
// Test entity
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    int32_t     age{};
};

ut::suite<"query/expr"> expr_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // col() factory
    // -----------------------------------------------------------------------

    "col() wraps int32_t member pointer in column_ref"_test = [] {
        auto c = atlas::col(&User::id);

        static_assert(
            std::is_same_v<decltype(c), atlas::column_ref<User, int32_t>>,
            "col() must return column_ref<Entity, T>");

        expect(c.ptr == &User::id);
    };

    "col() wraps std::string member pointer in column_ref"_test = [] {
        auto c = atlas::col(&User::email);

        static_assert(
            std::is_same_v<decltype(c), atlas::column_ref<User, std::string>>,
            "col() must deduce T = std::string");

        expect(c.ptr == &User::email);
    };

    "col() produces different column_refs for different members"_test = [] {
        auto c1 = atlas::col(&User::id);
        auto c2 = atlas::col(&User::age);

        expect(c1.ptr != c2.ptr);
    };

    // -----------------------------------------------------------------------
    // lit() factory
    // -----------------------------------------------------------------------

    "lit() wraps int value in literal<int>"_test = [] {
        auto l = atlas::lit(42);

        static_assert(
            std::is_same_v<decltype(l), atlas::literal<int>>,
            "lit() must return literal<remove_cvref_t<T>>");

        expect(l.value == 42);
    };

    "lit() wraps string_view in literal<string_view>"_test = [] {
        auto l = atlas::lit(std::string_view{"hello"});

        static_assert(
            std::is_same_v<decltype(l), atlas::literal<std::string_view>>);

        expect(l.value == "hello");
    };

    "lit() decays const ref via remove_cvref_t"_test = [] {
        const int x = 7;
        auto l = atlas::lit(x);

        static_assert(std::is_same_v<decltype(l.value), int>);
        expect(l.value == 7);
    };

    "lit() moves rvalue string correctly"_test = [] {
        auto l = atlas::lit(std::string{"world"});

        static_assert(std::is_same_v<decltype(l), atlas::literal<std::string>>);
        expect(l.value == "world");
    };

    // -----------------------------------------------------------------------
    // is_column_ref concept
    // -----------------------------------------------------------------------

    "is_column_ref accepts column_ref"_test = [] {
        static_assert(atlas::is_column_ref<atlas::column_ref<User, int32_t>>);
        expect(true);
    };

    "is_column_ref rejects literal<int>"_test = [] {
        static_assert(!atlas::is_column_ref<atlas::literal<int>>);
        expect(true);
    };

    "is_column_ref rejects raw member pointer"_test = [] {
        static_assert(!atlas::is_column_ref<decltype(&User::id)>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // is_literal concept
    // -----------------------------------------------------------------------

    "is_literal accepts literal<int>"_test = [] {
        static_assert(atlas::is_literal<atlas::literal<int>>);
        expect(true);
    };

    "is_literal rejects column_ref"_test = [] {
        static_assert(!atlas::is_literal<atlas::column_ref<User, int32_t>>);
        expect(true);
    };

    "is_literal rejects int"_test = [] {
        static_assert(!atlas::is_literal<int>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // column_eq_ref
    // -----------------------------------------------------------------------

    "column_eq_ref stores cross-table column equality"_test = [] {
        struct Post {
            int32_t id{};
            int32_t user_id{};
        };

        atlas::column_eq_ref<Post, int32_t, User, int32_t> ref{
            &Post::user_id, &User::id
        };

        expect(ref.lhs == &Post::user_id);
        expect(ref.rhs == &User::id);
    };

    "column_eq_ref supports same-entity column comparison"_test = [] {
        atlas::column_eq_ref<User, int32_t, User, int32_t> ref{
            &User::id, &User::age
        };

        expect(ref.lhs != ref.rhs);
    };
};
