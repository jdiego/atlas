#include <catch2/catch_test_macros.hpp>

#include "atlas/schema/column.hpp"
#include "atlas/schema/constraints.hpp"
#include "atlas/schema/serde.hpp"
#include "atlas/schema/table.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

TEST_CASE("to_params serializes int32_t correctly", "[serde]") {
    auto t = make_user_table();
    User u{ 42, "Alice", "alice@example.com", 9.5 };
    auto p = atlas::to_params(u, t);
    REQUIRE(p[0] == "42");
}

TEST_CASE("to_params serializes std::string correctly", "[serde]") {
    auto t = make_user_table();
    User u{ 1, "Bob", "bob@example.com", 0.0 };
    auto p = atlas::to_params(u, t);
    REQUIRE(p[1] == "Bob");
    REQUIRE(p[2] == "bob@example.com");
}

TEST_CASE("to_params serializes double correctly", "[serde]") {
    auto t = make_user_table();
    User u{ 1, "X", "x@x.com", 3.14 };
    auto p = atlas::to_params(u, t);
    // std::to_string produces a fixed decimal — just verify it starts with "3.14"
    REQUIRE(p[3].substr(0, 4) == "3.14");
}

TEST_CASE("to_params serializes bool as t/f", "[serde]") {
    struct Flags { bool enabled{}; };
    auto t = atlas::make_table<Flags>("flags",
        atlas::make_column("enabled", &Flags::enabled));

    Flags f_true{true};
    Flags f_false{false};

    auto pt = atlas::to_params(f_true, t);
    auto pf = atlas::to_params(f_false, t);

    REQUIRE(pt[0] == "t");
    REQUIRE(pf[0] == "f");
}

TEST_CASE("to_params column order matches table declaration order", "[serde]") {
    auto t = make_user_table();
    User u{ 7, "Carol", "carol@x.com", 1.5 };
    auto p = atlas::to_params(u, t);

    REQUIRE(p.size() == 4u);
    // Order: id, name, email, score
    REQUIRE(p[0] == "7");
    REQUIRE(p[1] == "Carol");
    REQUIRE(p[2] == "carol@x.com");
    REQUIRE(p[3].substr(0, 3) == "1.5");
}

// ---------------------------------------------------------------------------
// Round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("from_result round-trips a User", "[serde]") {
    auto t = make_user_table();

    User original{ 1, "Alice", "alice@x.com", 9.5 };
    auto params = atlas::to_params(original, t);

    mock_result res;
    res.rows.push_back({params[0], params[1], params[2], params[3]});

    User restored = atlas::from_result<User>(res, 0, t);

    REQUIRE(restored.id    == original.id);
    REQUIRE(restored.name  == original.name);
    REQUIRE(restored.email == original.email);
    // double round-trip through std::to_string / std::stod loses no significant digits
    // Round-trip through std::to_string / std::stod; check within tolerance.
    REQUIRE(restored.score >= original.score - 0.001);
    REQUIRE(restored.score <= original.score + 0.001);
}

TEST_CASE("from_result round-trips a Post", "[serde]") {
    auto t = make_post_table();

    Post original{ 10, 3, "Hello World" };
    auto params = atlas::to_params(original, t);

    mock_result res;
    res.rows.push_back({params[0], params[1], params[2]});

    Post restored = atlas::from_result<Post>(res, 0, t);

    REQUIRE(restored.id      == original.id);
    REQUIRE(restored.user_id == original.user_id);
    REQUIRE(restored.title   == original.title);
}
