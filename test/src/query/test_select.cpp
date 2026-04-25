// Tests for atlas/query/select.hpp
//
// Uses mock storage (no libpq). to_sql() / params() calls are present and
// will fail at link time until implementations are provided.

#include <boost/ut.hpp>

#include "atlas/query/select.hpp"
#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/storage.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ut = boost::ut;

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

struct Employee {
    int32_t     id{};
    int32_t     manager_id{};
    std::string name{};
};

struct employee_alias {
    static constexpr std::string_view alias = "e";
};

struct manager_alias {
    static constexpr std::string_view alias = "m";
};

using employee_instance = atlas::table_instance<Employee, employee_alias>;
using manager_instance = atlas::table_instance<Employee, manager_alias>;

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

static auto make_employee_db() {
    auto employee_table = atlas::make_table<Employee>("employees",
        atlas::make_column("id",         &Employee::id,         atlas::primary_key()),
        atlas::make_column("manager_id", &Employee::manager_id, atlas::not_null()),
        atlas::make_column("name",       &Employee::name,       atlas::not_null())
    );
    return atlas::make_storage(employee_table);
}

ut::suite<"query/select"> select_suite = [] {
    using namespace ut;

    // -----------------------------------------------------------------------
    // atlas::select() factory
    // -----------------------------------------------------------------------

    "select() wraps two member pointers into column_refs"_test = [] {
        auto q = atlas::select(&User::id, &User::name);

        using expected_selected = std::tuple<
            atlas::column_ref<User, int32_t>,
            atlas::column_ref<User, std::string>>;
        static_assert(std::is_same_v<decltype(q.selected), expected_selected>);
        expect(true);
    };

    "select() wraps a single member pointer"_test = [] {
        auto q = atlas::select(&User::email);
        static_assert(std::is_same_v<
            decltype(q.selected),
            std::tuple<atlas::column_ref<User, std::string>>>);
        expect(true);
    };

    "select() initial state has monostate predicate and no limit/offset"_test = [] {
        auto q = atlas::select(&User::id);
        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, std::monostate>);
        expect(!q.limit_n.has_value());
        expect(!q.offset_n.has_value());
    };

    // -----------------------------------------------------------------------
    // .from<Entity>()
    // -----------------------------------------------------------------------

    "from<Entity>() stamps the FromEntity type parameter"_test = [] {
        auto q = atlas::select(&User::id).from<User>();

        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, std::monostate>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // .where()
    // -----------------------------------------------------------------------

    "where() attaches a simple eq predicate"_test = [] {
        auto pred = atlas::eq(&User::id, 1);
        auto q    = atlas::select(&User::id).from<User>().where(pred);

        using expected_pred = atlas::eq_expr<
            atlas::column_ref<User, int32_t>,
            atlas::literal<int>>;
        static_assert(std::is_same_v<
            std::remove_cvref_t<decltype(q.where_pred)>, expected_pred>);
        expect(true);
    };

    "where() accepts and_ combinator predicate"_test = [] {
        auto q = atlas::select(&User::name)
            .from<User>()
            .where(atlas::and_(
                atlas::gt(&User::age, 18),
                atlas::like(&User::email, std::string{"%@corp.com"})
            ));
        static_assert(atlas::is_predicate<decltype(q.where_pred)>);
        expect(true);
    };

    // -----------------------------------------------------------------------
    // .order_by()
    // -----------------------------------------------------------------------

    "order_by() appends a single ascending order clause"_test = [] {
        auto q = atlas::select(&User::name)
            .from<User>()
            .order_by(&User::name);

        static_assert(std::tuple_size_v<decltype(q.order_cols)> == 1u);
        expect(true);
    };

    "order_by() chains two clauses"_test = [] {
        auto q = atlas::select(&User::id, &User::name)
            .from<User>()
            .order_by(&User::name)
            .order_by(&User::id, false);

        static_assert(std::tuple_size_v<decltype(q.order_cols)> == 2u);
        expect(true);
    };

    "order_by() stores descending flag correctly"_test = [] {
        auto q = atlas::select(&User::age)
            .from<User>()
            .order_by(&User::age, false);

        const auto& clause = std::get<0>(q.order_cols);
        expect(clause.ascending == false);
        expect(clause.col == &User::age);
    };

    // -----------------------------------------------------------------------
    // .limit() and .offset()
    // -----------------------------------------------------------------------

    "limit() and offset() store optional values"_test = [] {
        auto q = atlas::select(&User::id)
            .from<User>()
            .limit(50)
            .offset(10);

        expect(q.limit_n.has_value());
        expect(q.offset_n.has_value());
        expect(*q.limit_n  == 50_u);
        expect(*q.offset_n == 10_u);
    };

    "limit() only, without offset"_test = [] {
        auto q = atlas::select(&User::id).from<User>().limit(1);
        expect(q.limit_n.has_value());
        expect(!q.offset_n.has_value());
    };

    "limit(0) is stored"_test = [] {
        auto q = atlas::select(&User::id).from<User>().limit(0);
        expect(q.limit_n.has_value());
        expect(*q.limit_n == 0_u);
    };

    // -----------------------------------------------------------------------
    // .to_sql() and .params()
    // -----------------------------------------------------------------------

    "to_sql() emits WHERE + LIMIT + OFFSET with bound params"_test = [] {
        auto db = make_db();
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
        auto        params = q.params(db);

        expect(sql == "SELECT u.id, u.name FROM users u WHERE (u.age > $1 AND u.email LIKE $2) ORDER BY u.name ASC LIMIT 50 OFFSET 10");
        expect(params == std::vector<std::string>{"18", "%@corp.com"});
    };

    "to_sql() omits WHERE when no predicate is attached"_test = [] {
        auto db = make_db();
        auto q = atlas::select(&User::id).from<User>();
        std::string sql = q.to_sql(db);
        expect(sql.find("WHERE") == std::string::npos);
    };

    "to_sql() omits LIMIT when not set"_test = [] {
        auto db = make_db();
        auto q = atlas::select(&User::id).from<User>().where(atlas::eq(&User::id, 1));
        std::string sql = q.to_sql(db);
        expect(sql.find("LIMIT") == std::string::npos);
    };

    // -----------------------------------------------------------------------
    // atlas::select_all<Entity>() and atlas::all<Entity>()
    // -----------------------------------------------------------------------

    "all<Entity>() returns the all_columns_t marker"_test = [] {
        auto m = atlas::all<User>();
        static_assert(std::is_same_v<decltype(m), atlas::all_columns_t<User>>);
        expect(true);
    };

    "select_all<Entity>() carries an all_columns marker as the only selected expression"_test = [] {
        auto q = atlas::select_all<User>();
        static_assert(std::is_same_v<
            decltype(q.selected),
            std::tuple<atlas::all_columns_t<User>>>);
        expect(true);
    };

    "select_all<Entity>() pre-stamps FromEntity so .from<>() is unnecessary"_test = [] {
        auto db = make_db();
        auto q  = atlas::select_all<User>();

        // No .from<User>() call: should already serialize end-to-end.
        std::string sql = q.to_sql(db);
        expect(sql == "SELECT u.id, u.name, u.email, u.age FROM users u");
    };

    "select_all<Entity>() expands every mapped column in declaration order"_test = [] {
        auto db = make_db();
        auto q  = atlas::select_all<User>();

        std::string sql = q.to_sql(db);
        // Declaration order on the table_t drives serialization order.
        auto idx_id    = sql.find("u.id");
        auto idx_name  = sql.find("u.name");
        auto idx_email = sql.find("u.email");
        auto idx_age   = sql.find("u.age");
        expect(idx_id    != std::string::npos);
        expect(idx_name  != std::string::npos);
        expect(idx_email != std::string::npos);
        expect(idx_age   != std::string::npos);
        expect(idx_id < idx_name);
        expect(idx_name < idx_email);
        expect(idx_email < idx_age);
    };

    "select_all<Entity>() chains where/order/limit"_test = [] {
        auto db = make_db();
        auto q  = atlas::select_all<User>()
            .where(atlas::gt(&User::age, 21))
            .order_by(&User::name)
            .limit(10);

        std::string sql    = q.to_sql(db);
        auto        params = q.params(db);
        expect(sql == "SELECT u.id, u.name, u.email, u.age FROM users u WHERE u.age > $1 ORDER BY u.name ASC LIMIT 10");
        expect(params == std::vector<std::string>{"21"});
    };

    "select(all<Entity>()) expands inline when used as the only column"_test = [] {
        auto db = make_db();
        auto q  = atlas::select(atlas::all<User>()).from<User>();
        std::string sql = q.to_sql(db);
        expect(sql == "SELECT u.id, u.name, u.email, u.age FROM users u");
    };

    "select() expands all<Entity>() when it is the leading expression"_test = [] {
        auto db = make_db();
        auto q  = atlas::select(atlas::all<User>(), &Post::title)
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));
        std::string sql = q.to_sql(db);
        expect(sql == "SELECT u.id, u.name, u.email, u.age, p.title FROM users u INNER JOIN posts p ON p.user_id = u.id");
    };

    "select() expands all<Entity>() when it follows another column"_test = [] {
        // Regression guard: previous implementation emitted a stray ", " when
        // all_columns_t appeared after a sibling column in the SELECT list.
        auto db = make_db();
        auto q  = atlas::select(&Post::title, atlas::all<User>())
            .from<User>()
            .inner_join<Post>(atlas::eq(&Post::user_id, &User::id));
        std::string sql = q.to_sql(db);
        expect(sql == "SELECT p.title, u.id, u.name, u.email, u.age FROM users u INNER JOIN posts p ON p.user_id = u.id");
        expect(sql.find(", ,") == std::string::npos);
    };

    "all<table_instance>() expands columns with the instance alias"_test = [] {
        auto db = make_employee_db();
        auto q = atlas::select(atlas::all<employee_instance>(), atlas::all<manager_instance>())
            .from<employee_instance>()
            .inner_join<manager_instance>(
                atlas::eq(
                    atlas::col<employee_instance>(&Employee::manager_id),
                    atlas::col<manager_instance>(&Employee::id)
                )
        );

        auto sql = q.to_sql(db);
        expect(sql == "SELECT e.id, e.manager_id, e.name, m.id, m.manager_id, m.name "
                      "FROM employees e INNER JOIN employees m ON e.manager_id = m.id");
    };

    "select_all<table_instance>() pre-stamps the tagged FROM instance"_test = [] {
        auto db = make_employee_db();
        auto q = atlas::select_all<manager_instance>()
            .where(atlas::eq(atlas::col<manager_instance>(&Employee::name), std::string{"Ada"}))
            .order_by(atlas::col<manager_instance>(&Employee::id), false);

        auto sql = q.to_sql(db);
        auto params = q.params(db);
        expect(sql == "SELECT m.id, m.manager_id, m.name FROM employees m WHERE m.name = $1 ORDER BY m.id DESC");
        expect(params == std::vector<std::string>{"Ada"});
    };
};
