#pragma once

// atlas/schema/type_traits.hpp
//
// Maps C++ types to PostgreSQL SQL type names and OIDs.
// This header is purely compile-time metadata — no I/O, no libpq calls.
//
// To add a new type mapping, add a new explicit specialization of pg_type<T>
// below the existing ones, following the same pattern.
//
// OID constants are hardcoded (not read from pg_type at runtime) because
// these core type OIDs are stable across all PostgreSQL versions and using
// them avoids a runtime catalog query during schema validation.
//
// pg_uuid exists as a thin alias type to avoid pulling in Boost.Uuid (or any
// other UUID library) into this header-only layer.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

using pg_oid = std::uint32_t;

// ---------------------------------------------------------------------------
// pg_uuid — lightweight UUID holder (16 raw bytes).
// Used instead of boost::uuids::uuid to keep this layer dependency-free.
// ---------------------------------------------------------------------------
struct pg_uuid {
    std::array<uint8_t, 16> bytes{};
};

// ---------------------------------------------------------------------------
// Primary template — left undefined intentionally.
// A compiler error on pg_type<T> means T has no PostgreSQL mapping yet.
// Add a specialization below to support a new type.
// ---------------------------------------------------------------------------
template<typename T>
struct pg_type;

// ---------------------------------------------------------------------------
// Specializations
// ---------------------------------------------------------------------------

template<>
struct pg_type<bool> {
    static constexpr std::string_view sql_name = "BOOLEAN";
    static constexpr pg_oid           oid       = 16;
};

template<>
struct pg_type<int16_t> {
    static constexpr std::string_view sql_name = "SMALLINT";
    static constexpr pg_oid           oid       = 21;
};

template<>
struct pg_type<int32_t> {
    static constexpr std::string_view sql_name = "INTEGER";
    static constexpr pg_oid           oid       = 23;
};

template<>
struct pg_type<int64_t> {
    static constexpr std::string_view sql_name = "BIGINT";
    static constexpr pg_oid           oid       = 20;
};

template<>
struct pg_type<float> {
    static constexpr std::string_view sql_name = "FLOAT4";
    static constexpr pg_oid           oid       = 700;
};

template<>
struct pg_type<double> {
    static constexpr std::string_view sql_name = "FLOAT8";
    static constexpr pg_oid           oid       = 701;
};

template<>
struct pg_type<std::string> {
    static constexpr std::string_view sql_name = "TEXT";
    static constexpr pg_oid           oid       = 25;
};

template<>
struct pg_type<std::vector<uint8_t>> {
    static constexpr std::string_view sql_name = "BYTEA";
    static constexpr pg_oid           oid       = 17;
};

template<>
struct pg_type<std::chrono::system_clock::time_point> {
    static constexpr std::string_view sql_name = "TIMESTAMPTZ";
    static constexpr pg_oid           oid       = 1184;
};

template<>
struct pg_type<pg_uuid> {
    static constexpr std::string_view sql_name = "UUID";
    static constexpr pg_oid           oid       = 2950;
};

// ---------------------------------------------------------------------------
// Concept: satisfied iff T has a pg_type specialization with sql_name + oid.
// ---------------------------------------------------------------------------
template<typename T>
concept pg_mappable = requires {
    { pg_type<T>::sql_name } -> std::convertible_to<std::string_view>;
    { pg_type<T>::oid      } -> std::convertible_to<pg_oid>;
};

} // namespace atlas
