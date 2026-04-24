// Tests for atlas/query/predicate.hpp
//
// Verifies factory return types, node field values, is_predicate concept, and
// composition via and_/or_/not_.

#include <boost/ut.hpp>

#include "atlas/query/predicate.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ut = boost::ut;

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

ut::suite<"query/predicate"> predicate_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // eq factory
    // -----------------------------------------------------------------------

    "eq() produces eq_expr with int column and int value"_test = [] {
        auto pred = atlas::eq(&User::id, 42);

        using expected_t = atlas::eq_expr<
            atlas::column_ref<User, int32_t>,
            atlas::literal<int>>;
        static_assert(std::is_same_v<decltype(pred), expected_t>);

        expect(pred.lhs.ptr == &User::id);
        expect(pred.rhs.value == 42);
    };

    "eq() produces eq_expr with string column and string value"_test = [] {
        auto pred = atlas::eq(&User::email, std::string{"a@b.com"});

        expect(pred.lhs.ptr == &User::email);
        expect(pred.rhs.value == "a@b.com");
    };

    "eq() supports zero value"_test = [] {
        auto pred = atlas::eq(&User::age, 0);
        expect(pred.rhs.value == 0);
    };

    // -----------------------------------------------------------------------
    // Comparison factories
    // -----------------------------------------------------------------------

    "ne() produces ne_expr"_test = [] {
        auto p = atlas::ne(&User::age, 18);
        static_assert(std::is_same_v<decltype(p),
            atlas::ne_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        expect(p.lhs.ptr == &User::age);
    };

    "lt() produces lt_expr"_test = [] {
        auto p = atlas::lt(&User::age, 30);
        static_assert(std::is_same_v<decltype(p),
            atlas::lt_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        expect(true);
    };

    "gt() produces gt_expr with double column"_test = [] {
        auto p = atlas::gt(&User::score, 0.5);
        static_assert(std::is_same_v<decltype(p),
            atlas::gt_expr<atlas::column_ref<User, double>, atlas::literal<double>>>);
        expect(p.rhs.value == 0.5);
    };

    "lte() produces lte_expr"_test = [] {
        auto p = atlas::lte(&User::age, 65);
        static_assert(std::is_same_v<decltype(p),
            atlas::lte_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        expect(true);
    };

    "gte() produces gte_expr"_test = [] {
        auto p = atlas::gte(&User::id, 1);
        static_assert(std::is_same_v<decltype(p),
            atlas::gte_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        expect(true);
    };

    "like() produces like_expr with pattern string"_test = [] {
        auto p = atlas::like(&User::email, std::string{"%@corp.com"});
        static_assert(std::is_same_v<decltype(p),
            atlas::like_expr<
                atlas::column_ref<User, std::string>,
                atlas::literal<std::string>>>);
        expect(p.rhs.value == "%@corp.com");
    };

    // -----------------------------------------------------------------------
    // is_null / is_not_null
    // -----------------------------------------------------------------------

    "is_null() produces is_null_expr"_test = [] {
        auto p = atlas::is_null(&User::name);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_null_expr<atlas::column_ref<User, std::string>>>);
        expect(p.col.ptr == &User::name);
    };

    "is_not_null() produces is_not_null_expr"_test = [] {
        auto p = atlas::is_not_null(&User::name);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_not_null_expr<atlas::column_ref<User, std::string>>>);
        expect(p.col.ptr == &User::name);
    };

    "is_null() works on integer column"_test = [] {
        auto p = atlas::is_null(&User::id);
        static_assert(std::is_same_v<decltype(p),
            atlas::is_null_expr<atlas::column_ref<User, int32_t>>>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // in factory
    // -----------------------------------------------------------------------

    "in() stores column ref and vector of ints"_test = [] {
        std::vector<int32_t> ids{1, 2, 3};
        auto p = atlas::in(&User::id, ids);

        static_assert(std::is_same_v<decltype(p),
            atlas::in_expr<
                atlas::column_ref<User, int32_t>,
                std::vector<int32_t>>>);

        expect(p.col.ptr == &User::id);
        expect(p.values.size() == 3_u);
    };

    "in() accepts empty container"_test = [] {
        std::vector<int32_t> empty;
        auto p = atlas::in(&User::id, empty);
        expect(p.values.empty());
    };

    "in() accepts single-element container"_test = [] {
        std::vector<int32_t> one{42};
        auto p = atlas::in(&User::id, std::move(one));
        expect(p.values.size() == 1_u);
        expect(p.values[0] == 42);
    };

    // -----------------------------------------------------------------------
    // and_ / or_ / not_ combinators
    // -----------------------------------------------------------------------

    "and_() combines two leaf predicates"_test = [] {
        auto p = atlas::and_(
            atlas::gt(&User::age, 18),
            atlas::like(&User::email, std::string{"%@corp.com"})
        );

        static_assert(atlas::is_predicate<decltype(p)>);
        static_assert(std::is_same_v<
            decltype(p.lhs),
            atlas::gt_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        expect(true);
    };

    "and_() supports nested and_"_test = [] {
        auto p = atlas::and_(
            atlas::and_(atlas::eq(&User::id, 1), atlas::eq(&User::age, 30)),
            atlas::eq(&User::id, 2)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
        expect(true);
    };

    "or_() combines two predicates"_test = [] {
        auto p = atlas::or_(
            atlas::eq(&User::id, 1),
            atlas::eq(&User::id, 2)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
        expect(true);
    };

    "or_() accepts and_ children"_test = [] {
        auto p = atlas::or_(
            atlas::and_(atlas::eq(&User::id, 1), atlas::eq(&User::age, 20)),
            atlas::eq(&User::id, 99)
        );
        static_assert(atlas::is_predicate<decltype(p)>);
        expect(true);
    };

    "not_() wraps a predicate"_test = [] {
        auto p = atlas::not_(atlas::eq(&User::id, 0));
        static_assert(atlas::is_predicate<decltype(p)>);
        expect(true);
    };

    "not_() supports double negation"_test = [] {
        auto p = atlas::not_(atlas::not_(atlas::eq(&User::id, 1)));
        static_assert(atlas::is_predicate<decltype(p)>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // is_predicate concept
    // -----------------------------------------------------------------------

    "is_predicate accepts node types and rejects non-nodes"_test = [] {
        static_assert(atlas::is_predicate<
            atlas::eq_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>);
        static_assert(atlas::is_predicate<atlas::and_expr<
            atlas::eq_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>,
            atlas::eq_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>>);
        static_assert(atlas::is_predicate<atlas::not_expr<
            atlas::eq_expr<atlas::column_ref<User, int32_t>, atlas::literal<int>>>>);

        static_assert(!atlas::is_predicate<int>);
        static_assert(!atlas::is_predicate<atlas::column_ref<User, int32_t>>);
        static_assert(!atlas::is_predicate<atlas::literal<int>>);

        expect(true);
    };
};
