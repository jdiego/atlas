// Tests for atlas/query/join.hpp + join integration in select_query.
//
// Verifies join_clause struct, is_join_clause concept, and select_query
// inner_join / left_join methods.

#include <boost/ut.hpp>

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
#include <vector>

namespace ut = boost::ut;

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

ut::suite<"query/join"> join_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // join_clause struct
    // -----------------------------------------------------------------------

    "join_clause stores inner join kind with eq ON predicate"_test = [] {
        using on_pred_t = atlas::eq_expr<
            atlas::column_ref<Post, int32_t>,
            atlas::column_ref<User, int32_t>>;

        on_pred_t on_pred{
            atlas::column_ref<Post, int32_t>{&Post::user_id},
            atlas::column_ref<User, int32_t>{&User::id}
        };

        atlas::join_clause<Post, on_pred_t, atlas::join_kind::inner> jc{on_pred};

        static_assert(std::remove_cvref_t<decltype(jc)>::kind == atlas::join_kind::inner);
        expect(jc.on.lhs.ptr == &Post::user_id);
        expect(jc.on.rhs.ptr == &User::id);
    };

    "join_clause stores left join kind"_test = [] {
        using on_pred_t = atlas::eq_expr<
            atlas::column_ref<Post, int32_t>,
            atlas::column_ref<User, int32_t>>;

        atlas::join_clause<Post, on_pred_t, atlas::join_kind::left> jc{
            on_pred_t{
                atlas::column_ref<Post, int32_t>{&Post::user_id},
                atlas::column_ref<User, int32_t>{&User::id}
            }
        };

        static_assert(std::remove_cvref_t<decltype(jc)>::kind == atlas::join_kind::left);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // is_join_clause concept
    // -----------------------------------------------------------------------

    "is_join_clause accepts join_clause and rejects others"_test = [] {
        using on_pred_t = atlas::eq_expr<
            atlas::column_ref<Post, int32_t>,
            atlas::column_ref<User, int32_t>>;

        static_assert(atlas::is_join_clause<
            atlas::join_clause<Post, on_pred_t, atlas::join_kind::inner>>);
        static_assert(atlas::is_join_clause<
            atlas::join_clause<Post, on_pred_t, atlas::join_kind::left>>);

        static_assert(!atlas::is_join_clause<int>);
        static_assert(!atlas::is_join_clause<on_pred_t>);

        expect(true);
    };

    // -----------------------------------------------------------------------
    // select_query.inner_join()
    // -----------------------------------------------------------------------

    "inner_join() appends one join_clause"_test = [] {
        auto q = atlas::select(&User::name, &Post::title)
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));

        static_assert(std::tuple_size_v<decltype(q.joins)> == 1u);

        const auto& jc = std::get<0>(q.joins);
        static_assert(std::remove_cvref_t<decltype(jc)>::kind == atlas::join_kind::inner);
        expect(jc.on.lhs.ptr == &Post::user_id);
    };

    "inner_join() can be chained twice"_test = [] {
        struct Comment {
            int32_t id{};
            int32_t post_id{};
        };

        auto q = atlas::select(&User::name)
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id))
            .inner_join<Comment>(atlas::eq(&Comment::post_id, &Post::id));

        static_assert(std::tuple_size_v<decltype(q.joins)> == 2u);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // select_query.left_join()
    // -----------------------------------------------------------------------

    "left_join() appends a left join_clause"_test = [] {
        auto q = atlas::select(&User::name, &Post::title)
            .from<User>()
            .left_join<Post>(atlas::eq(&Post::user_id, &User::id));

        const auto& jc = std::get<0>(q.joins);
        static_assert(std::remove_cvref_t<decltype(jc)>::kind == atlas::join_kind::left);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // to_sql() with JOIN
    // -----------------------------------------------------------------------

    "INNER JOIN with ON produces correct SQL"_test = [] {
        auto db  = make_db();
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));
        auto sql = q.to_sql(db);

        expect(sql == "SELECT u.name, p.title FROM users u INNER JOIN posts p ON p.user_id = u.id");
    };

    "LEFT JOIN produces LEFT JOIN keyword"_test = [] {
        auto db  = make_db();
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .left_join<Post>(atlas::eq(&Post::user_id, &User::id));
        auto sql = q.to_sql(db);

        expect(sql.find("LEFT JOIN") != std::string::npos);
    };

    "JOIN combined with WHERE produces both clauses"_test = [] {
        auto db   = make_db();
        auto q    = atlas::select(&User::name, &Post::title)
                        .from<User>()
                        .inner_join<Post>(atlas::eq(&Post::user_id, &User::id))
                        .where(atlas::eq(&User::id, 5));
        auto sql  = q.to_sql(db);
        auto prm  = q.params(db);

        expect(sql.find("INNER JOIN") != std::string::npos);
        expect(sql.find("WHERE u.id = $1") != std::string::npos);
        expect(prm == std::vector<std::string>{"5"});
    };

    "JOIN ON params come before WHERE params"_test = [] {
        auto db  = make_db();
        auto q   = atlas::select(&User::name, &Post::title)
                       .from<User>()
                       .inner_join<Post>(atlas::and_(
                           atlas::eq(&Post::user_id, &User::id),
                           atlas::gt(&Post::id, 10)
                       ))
                       .where(atlas::eq(&User::name, std::string{"Alice"}));
        auto sql = q.to_sql(db);
        auto prm = q.params(db);

        expect(sql == "SELECT u.name, p.title FROM users u INNER JOIN posts p ON (p.user_id = u.id AND p.id > $1) WHERE u.name = $2");
        expect(prm == std::vector<std::string>{"10", "Alice"});
    };
};
