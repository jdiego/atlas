// Tests for atlas/query/aggregate.hpp
//
// Verifies aggregate node types, factories, is_aggregate concept, and
// integration with atlas::select().

#include <boost/ut.hpp>

#include "atlas/query/aggregate.hpp"
#include "atlas/query/select.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ut = boost::ut;

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

ut::suite<"query/aggregate"> aggregate_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // count(member_ptr)
    // -----------------------------------------------------------------------

    "count(col) wraps column_ref in count_expr"_test = [] {
        auto agg = atlas::count(&User::id);
        using expected_t = atlas::count_expr<atlas::column_ref<User, int32_t>>;
        static_assert(std::is_same_v<decltype(agg), expected_t>);
        expect(agg.col.ptr == &User::id);
    };

    "count() works on string column"_test = [] {
        auto agg = atlas::count(&User::name);
        static_assert(std::is_same_v<decltype(agg),
            atlas::count_expr<atlas::column_ref<User, std::string>>>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // count() — count star
    // -----------------------------------------------------------------------

    "count() with no args returns count_star_expr"_test = [] {
        auto agg = atlas::count();
        static_assert(std::is_same_v<decltype(agg), atlas::count_star_expr>);
        expect(true);
    };

    "count_star_expr satisfies is_aggregate"_test = [] {
        static_assert(atlas::is_aggregate<atlas::count_star_expr>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // sum, avg, min, max
    // -----------------------------------------------------------------------

    "sum() produces sum_expr"_test = [] {
        auto agg = atlas::sum(&User::score);
        static_assert(std::is_same_v<decltype(agg),
            atlas::sum_expr<atlas::column_ref<User, double>>>);
        expect(agg.col.ptr == &User::score);
    };

    "avg() produces avg_expr"_test = [] {
        auto agg = atlas::avg(&User::score);
        static_assert(std::is_same_v<decltype(agg),
            atlas::avg_expr<atlas::column_ref<User, double>>>);
        expect(true);
    };

    "min() produces min_expr"_test = [] {
        auto agg = atlas::min(&User::age);
        static_assert(std::is_same_v<decltype(agg),
            atlas::min_expr<atlas::column_ref<User, int32_t>>>);
        expect(true);
    };

    "max() produces max_expr"_test = [] {
        auto agg = atlas::max(&User::age);
        static_assert(std::is_same_v<decltype(agg),
            atlas::max_expr<atlas::column_ref<User, int32_t>>>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // is_aggregate concept
    // -----------------------------------------------------------------------

    "is_aggregate accepts node types and rejects others"_test = [] {
        static_assert(atlas::is_aggregate<atlas::count_expr<atlas::column_ref<User, int32_t>>>);
        static_assert(atlas::is_aggregate<atlas::sum_expr<atlas::column_ref<User, double>>>);
        static_assert(atlas::is_aggregate<atlas::avg_expr<atlas::column_ref<User, double>>>);
        static_assert(atlas::is_aggregate<atlas::min_expr<atlas::column_ref<User, int32_t>>>);
        static_assert(atlas::is_aggregate<atlas::max_expr<atlas::column_ref<User, int32_t>>>);
        static_assert(atlas::is_aggregate<atlas::count_star_expr>);

        static_assert(!atlas::is_aggregate<int>);
        static_assert(!atlas::is_aggregate<atlas::column_ref<User, int32_t>>);
        static_assert(!atlas::is_aggregate<atlas::literal<int>>);

        expect(true);
    };

    // -----------------------------------------------------------------------
    // aggregate in select()
    // -----------------------------------------------------------------------

    "select(count()) accepts COUNT(*) expression"_test = [] {
        auto q = atlas::select(atlas::count())
            .from<User>()
            .where(atlas::gt(&User::age, 18));

        static_assert(atlas::is_aggregate<
            std::tuple_element_t<0, decltype(q.selected)>>);
        expect(true);
    };

    "select(count(col)) accepts COUNT(col) expression"_test = [] {
        auto q = atlas::select(atlas::count(&User::id))
            .from<User>();

        static_assert(atlas::is_aggregate<
            std::tuple_element_t<0, decltype(q.selected)>>);
        expect(true);
    };

    "to_sql() emits COUNT(*) with WHERE"_test = [] {
        auto db = make_db();
        auto q  = atlas::select(atlas::count())
                      .from<User>()
                      .where(atlas::gt(&User::age, 18));

        std::string sql = q.to_sql(db);
        auto prm        = q.params();

        expect(sql == "SELECT COUNT(*) FROM users u WHERE u.age > $1");
        expect(prm == std::vector<std::string>{"18"});
    };
};
