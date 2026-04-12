#pragma once
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

// Primary template — left undefined. Compiler error = unsupported type.
// To add a new type: specialize value_serializer<T> with a static
// `std::string to_string(const T&)` method.
template<typename T>
struct value_serializer;

template<>
struct value_serializer<bool> {
    static std::string to_string(bool v) { return v ? "t" : "f"; }
};
 
template<>
struct value_serializer<int16_t> {
    static std::string to_string(int16_t v) { return std::to_string(v); }
};
 
template<>
struct value_serializer<int32_t> {
    static std::string to_string(int32_t v) { return std::to_string(v); }
};
 
template<>
struct value_serializer<int64_t> {
    static std::string to_string(int64_t v) { return std::to_string(v); }
};
 
// TODO (locale): std::to_string for floats is locale-dependent.
template<>
struct value_serializer<float> {
    static std::string to_string(float v) { return std::to_string(v); }
};
 
template<>
struct value_serializer<double> {
    static std::string to_string(double v) { return std::to_string(v); }
};
 
// std::string passes through as-is; PQsendQueryParams handles quoting.
template<>
struct value_serializer<std::string> {
    static std::string to_string(const std::string& v) { return v; }
};
 
// pg_uuid → 8-4-4-4-12 lowercase hex
template<>
struct value_serializer<pg_uuid> {
    static std::string to_string(const pg_uuid& u) {
        const auto& b = u.bytes;
        return std::format(
            "{:02x}{:02x}{:02x}{:02x}-"
            "{:02x}{:02x}-{:02x}{:02x}-"
            "{:02x}{:02x}-"
            "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
            b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
    }
};
 
// time_point → ISO 8601 UTC "YYYY-MM-DDTHH:MM:SSZ"
template<>
struct value_serializer<std::chrono::system_clock::time_point> {
    static std::string to_string(
        const std::chrono::system_clock::time_point& tp)
    {
        return std::format("{:%FT%TZ}", tp);
    }
};
 
// vector<uint8_t> → PostgreSQL hex-escaped bytea "\\xDEADBEEF"
template<>
struct value_serializer<std::vector<uint8_t>> {
    static std::string to_string(const std::vector<uint8_t>& v) {
        std::string out;
        out.reserve(2 + v.size() * 2);
        out += "\\x";
        for (uint8_t b : v) out += std::format("{:02X}", b);
        return out;
    }
};
 
// ---------------------------------------------------------------------------
// Deserialization: same class-template pattern for symmetry and ODR safety.
// Primary template — left undefined. Compiler error = unsupported type.
// ---------------------------------------------------------------------------
 
template<typename T>
struct value_deserializer;
 
template<>
struct value_deserializer<bool> {
    static bool from_string(std::string_view sv) {
        return sv == "t" || sv == "true" || sv == "1";
    }
};
 
template<>
struct value_deserializer<int16_t> {
    static int16_t from_string(std::string_view sv) {
        return static_cast<int16_t>(std::stoi(std::string(sv)));
    }
};
 
template<>
struct value_deserializer<int32_t> {
    static int32_t from_string(std::string_view sv) {
        return std::stoi(std::string(sv));
    }
};
 
template<>
struct value_deserializer<int64_t> {
    static int64_t from_string(std::string_view sv) {
        return std::stoll(std::string(sv));
    }
};
 
template<>
struct value_deserializer<float> {
    static float from_string(std::string_view sv) {
        return std::stof(std::string(sv));
    }
};
 
template<>
struct value_deserializer<double> {
    static double from_string(std::string_view sv) {
        return std::stod(std::string(sv));
    }
};
 
template<>
struct value_deserializer<std::string> {
    static std::string from_string(std::string_view sv) {
        return std::string(sv);
    }
};
 
template<>
struct value_deserializer<pg_uuid> {
    static pg_uuid from_string(std::string_view sv) {
        pg_uuid u{};
        std::size_t byte_idx = 0;
        for (std::size_t i = 0; i < sv.size() && byte_idx < 16; ++i) {
            if (sv[i] == '-') continue;
            auto hex = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            uint8_t hi = hex(sv[i]);
            uint8_t lo = hex(sv[++i]);
            u.bytes[byte_idx++] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return u;
    }
};
 
template<>
struct value_deserializer<std::vector<uint8_t>> {
    static std::vector<uint8_t> from_string(std::string_view sv) {
        if (sv.size() >= 2 && sv[0] == '\\' && sv[1] == 'x') sv.remove_prefix(2);
        else if (!sv.empty() && sv[0] == 'x')                 sv.remove_prefix(1);
        std::vector<uint8_t> out;
        out.reserve(sv.size() / 2);
        auto hex = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        for (std::size_t i = 0; i + 1 < sv.size(); i += 2)
            out.push_back(static_cast<uint8_t>((hex(sv[i]) << 4) | hex(sv[i+1])));
        return out;
    }
};
 
template<>
struct value_deserializer<std::chrono::system_clock::time_point> {
    static std::chrono::system_clock::time_point from_string(std::string_view sv) {
        // TODO: implement ISO 8601 UTC parsing.
        // std::chrono::parse is C++20 but not universally available yet.
        // Current behaviour: value-initialize (epoch) when parsing fails.
        (void)sv;
        return {};
    }
};
 
// ---------------------------------------------------------------------------
// Public forwarding helpers — thin wrappers over the class templates above.
// These are the functions called by to_params() and from_result().
// ---------------------------------------------------------------------------
 
template<typename T>
inline std::string serialize_value(const T& v) {
    return value_serializer<T>::to_string(v);
}
 
template<typename T>
inline T deserialize_value(std::string_view sv) {
    return value_deserializer<T>::from_string(sv);
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
[[nodiscard]] Entity from_result(const ResultT& res, int row, const table_t<Entity, Columns...>& table)
{
    Entity e{};
    int col_idx = 0;
    table.for_each_column([&](const auto& col) {
        using member_type = typename std::remove_cvref_t<decltype(col)>::member_type;
        auto sv = res.get(row, col_idx++);
        if (sv.has_value()) {
            // std::move on the temporary is explicit — matches set(Entity&, member_type&&)
            col.set(e, detail::deserialize_value<member_type>(*sv));
        }
        // else: value-initialize (TODO: surface nullopt as an error)
    });
    return e;
}

} // namespace atlas
