#include <catch2/catch_test_macros.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"

#include <cstdint>
#include <string>

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

TEST_CASE("make_column stores name and member pointer", "[column]") {
    auto col = atlas::make_column("id", &Widget::id);
    REQUIRE(col.name == "id");
    REQUIRE(col.member_ptr == &Widget::id);
}

TEST_CASE("has_constraint returns true/false correctly", "[column]") {
    auto col = atlas::make_column("id", &Widget::id, atlas::primary_key());

    REQUIRE( col.has_constraint<atlas::primary_key_t>());
    REQUIRE(!col.has_constraint<atlas::not_null_t>());
    REQUIRE(!col.has_constraint<atlas::unique_t>());
}

TEST_CASE("get_constraint returns the stored constraint value", "[column]") {
    auto col = atlas::make_column("weight", &Widget::weight,
                                  atlas::default_value(3.14));

    REQUIRE(col.has_constraint<atlas::default_value_t<double>>());
    const auto& dv = col.get_constraint<atlas::default_value_t<double>>();
    REQUIRE(dv.value == 3.14);
}

TEST_CASE("column::get reads the correct member from an entity", "[column]") {
    auto col = atlas::make_column("label", &Widget::label);
    Widget w;
    w.label = "hello";
    REQUIRE(col.get(w) == "hello");
}

TEST_CASE("column::set writes the correct member to an entity", "[column]") {
    auto col = atlas::make_column("id", &Widget::id);
    Widget w{};
    col.set(w, 42);
    REQUIRE(w.id == 42);
}

TEST_CASE("column with multiple constraints composes without error", "[column]") {
    auto col = atlas::make_column("label", &Widget::label,
                                  atlas::not_null(),
                                  atlas::unique());

    REQUIRE( col.has_constraint<atlas::not_null_t>());
    REQUIRE( col.has_constraint<atlas::unique_t>());
    REQUIRE(!col.has_constraint<atlas::primary_key_t>());
}

TEST_CASE("is_column concept is satisfied for column_t", "[column]") {
    auto col = atlas::make_column("active", &Widget::active);
    STATIC_REQUIRE(atlas::is_column<decltype(col)>);
    STATIC_REQUIRE(!atlas::is_column<int>);
    STATIC_REQUIRE(!atlas::is_column<Widget>);
}

TEST_CASE("column member_type deduced correctly", "[column]") {
    auto col_id    = atlas::make_column("id",     &Widget::id);
    auto col_label = atlas::make_column("label",  &Widget::label);
    auto col_wt    = atlas::make_column("weight", &Widget::weight);

    STATIC_REQUIRE(std::is_same_v<decltype(col_id)::member_type,    int32_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(col_label)::member_type, std::string>);
    STATIC_REQUIRE(std::is_same_v<decltype(col_wt)::member_type,    double>);
}
