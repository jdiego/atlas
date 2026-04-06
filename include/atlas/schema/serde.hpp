#pragma once

// atlas/schema/serde.hpp
//
// Converts between Entity instances and the text-format parameter arrays
// used by PQsendQueryParams, and parses result rows back into entities.
//
// ResultT is a template parameter — NOT a concrete type — so this header
// never #includes atlas/core/result.hpp and stays linkable without libpq.
// Any type that satisfies:
//   std::optional<std::string_view> get(int row, int col) const noexcept;
// works as ResultT, including the mock_result used in unit tests.
//
// TODO (locale): float/double serialization uses std::to_string, which is
// locale-dependent on some platforms. Replace with std::format("{}", v) or
// a custom dtoa when locale-independence is required.
//
// TODO (nullopt): When ResultT::get() returns nullopt for a non-optional
// member, behavior is currently implementation-defined. Two sensible choices:
//   1. Value-initialize the member (current behaviour — silently zero/empty).
//   2. Return std::expected<Entity, error> and propagate the null as an error.
// Adapt once std::optional<T> member support is added to column_t.

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/schema/table.hpp"
#include "atlas/schema/type_traits.hpp"

namespace atlas {

// ---------------------------------------------------------------------------
// Serialization helpers (entity member → PostgreSQL text protocol string)
// ---------------------------------------------------------------------------

namespace detail {

inline std::string serialize_value(bool v) {
    return v ? "t" : "f";
}

inline std::string serialize_value(int16_t v) { return std::to_string(v); }
inline std::string serialize_value(int32_t v) { return std::to_string(v); }
inline std::string serialize_value(int64_t v) { return std::to_string(v); }

// TODO (locale): std::to_string for floats is locale-dependent.
inline std::string serialize_value(float v)  { return std::to_string(v); }
inline std::string serialize_value(double v) { return std::to_string(v); }

// std::string passes through as-is; PQsendQueryParams handles quoting.
inline std::string serialize_value(const std::string& v) { return v; }

// pg_uuid → 8-4-4-4-12 lowercase hex (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
inline std::string serialize_value(const pg_uuid& u) {
    const auto& b = u.bytes;
    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-"
        "{:02x}{:02x}-"
        "{:02x}{:02x}-"
        "{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

// time_point → ISO 8601 UTC string "YYYY-MM-DDTHH:MM:SSZ"
inline std::string serialize_value(
    const std::chrono::system_clock::time_point& tp)
{
    // std::format with %FT%TZ (C++20 chrono format)
    return std::format("{:%FT%TZ}", tp);
}

// vector<uint8_t> → PostgreSQL hex-escaped bytea "\\xDEADBEEF"
inline std::string serialize_value(const std::vector<uint8_t>& v) {
    std::string out;
    out.reserve(2 + v.size() * 2);
    out += "\\x";
    for (uint8_t b : v) {
        out += std::format("{:02X}", b);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Deserialization helpers (std::string_view → member type)
// ---------------------------------------------------------------------------

template<typename T>
T deserialize_value(std::string_view sv);

template<>
inline bool deserialize_value<bool>(std::string_view sv) {
    return sv == "t" || sv == "true" || sv == "1";
}

template<>
inline int16_t deserialize_value<int16_t>(std::string_view sv) {
    return static_cast<int16_t>(std::stoi(std::string(sv)));
}

template<>
inline int32_t deserialize_value<int32_t>(std::string_view sv) {
    return std::stoi(std::string(sv));
}

template<>
inline int64_t deserialize_value<int64_t>(std::string_view sv) {
    return std::stoll(std::string(sv));
}

template<>
inline float deserialize_value<float>(std::string_view sv) {
    return std::stof(std::string(sv));
}

template<>
inline double deserialize_value<double>(std::string_view sv) {
    return std::stod(std::string(sv));
}

template<>
inline std::string deserialize_value<std::string>(std::string_view sv) {
    return std::string(sv);
}

template<>
inline pg_uuid deserialize_value<pg_uuid>(std::string_view sv) {
    // Parse 8-4-4-4-12 hex UUID string.
    pg_uuid u{};
    std::size_t byte_idx = 0;
    for (std::size_t i = 0; i < sv.size() && byte_idx < 16; ++i) {
        char c = sv[i];
        if (c == '-') continue;
        uint8_t hi = 0;
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        ++i;
        char c2 = sv[i];
        uint8_t lo = 0;
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
        u.bytes[byte_idx++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return u;
}

template<>
inline std::vector<uint8_t> deserialize_value<std::vector<uint8_t>>(
    std::string_view sv)
{
    // Strip leading "\\x" or "\x".
    if (sv.size() >= 2 && sv[0] == '\\' && sv[1] == 'x') sv.remove_prefix(2);
    else if (sv.size() >= 2 && sv[0] == 'x')              sv.remove_prefix(1);

    std::vector<uint8_t> out;
    out.reserve(sv.size() / 2);
    for (std::size_t i = 0; i + 1 < sv.size(); i += 2) {
        uint8_t hi = 0, lo = 0;
        auto hc = sv[i];
        auto lc = sv[i+1];
        if (hc >= '0' && hc <= '9') hi = hc - '0';
        else if (hc >= 'a' && hc <= 'f') hi = hc - 'a' + 10;
        else if (hc >= 'A' && hc <= 'F') hi = hc - 'A' + 10;
        if (lc >= '0' && lc <= '9') lo = lc - '0';
        else if (lc >= 'a' && lc <= 'f') lo = lc - 'a' + 10;
        else if (lc >= 'A' && lc <= 'F') lo = lc - 'A' + 10;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

template<>
inline std::chrono::system_clock::time_point
deserialize_value<std::chrono::system_clock::time_point>(std::string_view sv)
{
    // TODO: implement ISO 8601 UTC parsing. std::chrono::parse is C++20 but
    // not universally available. For now, value-initialize.
    (void)sv;
    return std::chrono::system_clock::time_point{};
}

} // namespace detail

// ---------------------------------------------------------------------------
// to_params: entity → vector<string> in column declaration order
// ---------------------------------------------------------------------------

template<typename Entity, typename... Columns>
[[nodiscard]] std::vector<std::string>
to_params(const Entity& e, const table_t<Entity, Columns...>& table) {
    std::vector<std::string> params;
    params.reserve(table.column_count);
    table.for_each_column([&](const auto& col) {
        params.push_back(detail::serialize_value(col.get(e)));
    });
    return params;
}

// ---------------------------------------------------------------------------
// from_result: result row → entity
//
// ResultT must expose:
//   std::optional<std::string_view> get(int row, int col) const noexcept;
//
// TODO (nullopt): if get() returns nullopt for a non-optional member the
// member is currently value-initialized (zero / empty string). Future work:
// return std::expected<Entity, error> and surface the null to the caller.
// ---------------------------------------------------------------------------

template<typename Entity, typename ResultT, typename... Columns>
[[nodiscard]] Entity
from_result(const ResultT& res, int row,
            const table_t<Entity, Columns...>& table)
{
    Entity e{};
    int col_idx = 0;
    table.for_each_column([&](const auto& col) {
        using member_type = typename std::remove_cvref_t<decltype(col)>::member_type;
        auto sv = res.get(row, col_idx++);
        if (sv.has_value()) {
            col.set(e, detail::deserialize_value<member_type>(*sv));
        }
        // else: value-initialize (TODO: surface nullopt as an error)
    });
    return e;
}

} // namespace atlas
