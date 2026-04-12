#include <boost/ut.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"

#include <cstdint>
#include <string>
#include <type_traits>

namespace ut = boost::ut;

// ---------------------------------------------------------------------------
// Test entity
// ---------------------------------------------------------------------------

struct Widget {
    int32_t     id{};
    std::string label{};
    double      weight{};
    bool        active{};
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

ut::suite<"schema/column"> column_suite = [] {
    using namespace ut;

    "make_column stores name and member pointer"_test = [] {
        auto col = atlas::make_column("id", &Widget::id);
        expect(col.name == "id");
        expect(col.member_ptr == &Widget::id);
    };

    "has_constraint returns true/false correctly"_test = [] {
        auto col = atlas::make_column("id", &Widget::id, atlas::primary_key());

        expect(col.has_constraint<atlas::primary_key_t>());
        expect(!col.has_constraint<atlas::not_null_t>());
        expect(!col.has_constraint<atlas::unique_t>());
    };

    "get_constraint returns the stored constraint value"_test = [] {
        auto col = atlas::make_column("weight", &Widget::weight,
                                      atlas::default_value(3.14));

        expect(col.has_constraint<atlas::default_value_t<double>>());
        const auto& dv = col.get_constraint<atlas::default_value_t<double>>();
        expect(dv.value == 3.14);
    };

    "column::get reads the correct member from an entity"_test = [] {
        auto col = atlas::make_column("label", &Widget::label);
        Widget w;
        w.label = "hello";
        expect(col.get(w) == "hello");
    };

    "column::set writes the correct member to an entity"_test = [] {
        auto col = atlas::make_column("id", &Widget::id);
        Widget w{};
        col.set(w, 42);
        expect(w.id == 42_i);
    };

    "column with multiple constraints composes without error"_test = [] {
        auto col = atlas::make_column("label", &Widget::label,
                                      atlas::not_null(),
                                      atlas::unique());

        expect(col.has_constraint<atlas::not_null_t>());
        expect(col.has_constraint<atlas::unique_t>());
        expect(!col.has_constraint<atlas::primary_key_t>());
    };

    "is_column concept is satisfied for column_t"_test = [] {
        auto col = atlas::make_column("active", &Widget::active);
        static_assert(atlas::is_column<decltype(col)>);
        static_assert(!atlas::is_column<int>);
        static_assert(!atlas::is_column<Widget>);
        expect(true);
    };

    "column member_type deduced correctly"_test = [] {
        auto col_id    = atlas::make_column("id",     &Widget::id);
        auto col_label = atlas::make_column("label",  &Widget::label);
        auto col_wt    = atlas::make_column("weight", &Widget::weight);

        static_assert(std::is_same_v<decltype(col_id)::member_type, int32_t>);
        static_assert(std::is_same_v<decltype(col_label)::member_type, std::string>);
        static_assert(std::is_same_v<decltype(col_wt)::member_type, double>);
        expect(true);
    };
};
