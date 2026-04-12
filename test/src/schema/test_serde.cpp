#include <boost/ut.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/serde.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ut = boost::ut;

// ---------------------------------------------------------------------------
// Test entities
// ---------------------------------------------------------------------------

struct User {
    int32_t     id{};
    std::string name{};
    std::string email{};
    double      score{};
};

struct Post {
    int32_t     id{};
    int32_t     user_id{};
    std::string title{};
};

// ---------------------------------------------------------------------------
// Mock result type — satisfies the ResultT contract without linking libpq.
// ---------------------------------------------------------------------------

struct mock_result {
    std::vector<std::vector<std::optional<std::string>>> rows;

    std::optional<std::string_view> get(int r, int c) const noexcept {
        if (r < 0 || static_cast<std::size_t>(r) >= rows.size()) return std::nullopt;
        const auto& row = rows[static_cast<std::size_t>(r)];
        if (c < 0 || static_cast<std::size_t>(c) >= row.size()) return std::nullopt;
        const auto& cell = row[static_cast<std::size_t>(c)];
        if (!cell) return std::nullopt;
        return std::string_view{*cell};
    }
};

// ---------------------------------------------------------------------------
// Shared table helpers
// ---------------------------------------------------------------------------

static auto make_user_table() {
    return atlas::make_table<User>("users",
        atlas::make_column("id",    &User::id),
        atlas::make_column("name",  &User::name),
        atlas::make_column("email", &User::email),
        atlas::make_column("score", &User::score)
    );
}

static auto make_post_table() {
    return atlas::make_table<Post>("posts",
        atlas::make_column("id",      &Post::id),
        atlas::make_column("user_id", &Post::user_id),
        atlas::make_column("title",   &Post::title)
    );
}

// ---------------------------------------------------------------------------
// Serialization tests
// ---------------------------------------------------------------------------

ut::suite<"schema/serde"> serde_suite = [] {
    using namespace ut;

    "to_params serializes int32_t correctly"_test = [] {
        auto t = make_user_table();
        User u{42, "Alice", "alice@example.com", 9.5};
        auto p = atlas::to_params(u, t);
        expect(p[0] == "42");
    };

    "to_params serializes std::string correctly"_test = [] {
        auto t = make_user_table();
        User u{1, "Bob", "bob@example.com", 0.0};
        auto p = atlas::to_params(u, t);
        expect(p[1] == "Bob");
        expect(p[2] == "bob@example.com");
    };

    "to_params serializes double correctly"_test = [] {
        auto t = make_user_table();
        User u{1, "X", "x@x.com", 3.14};
        auto p = atlas::to_params(u, t);
        expect(p[3].substr(0, 4) == "3.14");
    };

    "to_params serializes bool as t/f"_test = [] {
        struct Flags { bool enabled{}; };
        auto t = atlas::make_table<Flags>("flags",
            atlas::make_column("enabled", &Flags::enabled));

        Flags f_true{true};
        Flags f_false{false};

        auto pt = atlas::to_params(f_true, t);
        auto pf = atlas::to_params(f_false, t);

        expect(pt[0] == "t");
        expect(pf[0] == "f");
    };

    "to_params column order matches table declaration order"_test = [] {
        auto t = make_user_table();
        User u{7, "Carol", "carol@x.com", 1.5};
        auto p = atlas::to_params(u, t);

        expect(p.size() == 4_u);
        expect(p[0] == "7");
        expect(p[1] == "Carol");
        expect(p[2] == "carol@x.com");
        expect(p[3].substr(0, 3) == "1.5");
    };

    "from_result round-trips a User"_test = [] {
        auto t = make_user_table();

        User original{1, "Alice", "alice@x.com", 9.5};
        auto params = atlas::to_params(original, t);

        mock_result res;
        res.rows.push_back({params[0], params[1], params[2], params[3]});

        User restored = atlas::from_result<User>(res, 0, t);

        expect(restored.id == original.id);
        expect(restored.name == original.name);
        expect(restored.email == original.email);
        expect(restored.score >= original.score - 0.001);
        expect(restored.score <= original.score + 0.001);
    };

    "from_result round-trips a Post"_test = [] {
        auto t = make_post_table();

        Post original{10, 3, "Hello World"};
        auto params = atlas::to_params(original, t);

        mock_result res;
        res.rows.push_back({params[0], params[1], params[2]});

        Post restored = atlas::from_result<Post>(res, 0, t);

        expect(restored.id == original.id);
        expect(restored.user_id == original.user_id);
        expect(restored.title == original.title);
    };
};
