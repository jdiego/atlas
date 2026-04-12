#include <boost/ut.hpp>

#include "atlas/schema/type_traits.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ut = boost::ut;

// ---------------------------------------------------------------------------
// Compile-time checks
// ---------------------------------------------------------------------------

static_assert(atlas::pg_type<int32_t>::sql_name == "INTEGER");
static_assert(atlas::pg_type<std::string>::sql_name == "TEXT");
static_assert(atlas::pg_type<bool>::sql_name == "BOOLEAN");
static_assert(atlas::pg_type<int16_t>::sql_name == "SMALLINT");
static_assert(atlas::pg_type<int64_t>::sql_name == "BIGINT");
static_assert(atlas::pg_type<float>::sql_name == "FLOAT4");
static_assert(atlas::pg_type<double>::sql_name == "FLOAT8");
static_assert(atlas::pg_type<std::vector<uint8_t>>::sql_name == "BYTEA");
static_assert(atlas::pg_type<atlas::pg_uuid>::sql_name == "UUID");
static_assert(atlas::pg_type<std::chrono::system_clock::time_point>::sql_name == "TIMESTAMPTZ");

// pg_mappable concept satisfied for mapped types
static_assert(atlas::pg_mappable<double>);
static_assert(atlas::pg_mappable<int32_t>);
static_assert(atlas::pg_mappable<std::string>);
static_assert(atlas::pg_mappable<atlas::pg_uuid>);

// pg_mappable concept NOT satisfied for unmapped types
static_assert(!atlas::pg_mappable<std::vector<int>>);
static_assert(!atlas::pg_mappable<unsigned int>);
static_assert(!atlas::pg_mappable<char>);

// ---------------------------------------------------------------------------
// Runtime OID checks
// ---------------------------------------------------------------------------

ut::suite<"schema/type_traits"> type_traits_suite = [] {
    using namespace ut;

    "pg_type OIDs are correct"_test = [] {
        expect(atlas::pg_type<bool>::oid == 16_u);
        expect(atlas::pg_type<int16_t>::oid == 21_u);
        expect(atlas::pg_type<int32_t>::oid == 23_u);
        expect(atlas::pg_type<int64_t>::oid == 20_u);
        expect(atlas::pg_type<float>::oid == 700_u);
        expect(atlas::pg_type<double>::oid == 701_u);
        expect(atlas::pg_type<std::string>::oid == 25_u);
        expect(atlas::pg_type<std::vector<uint8_t>>::oid == 17_u);
        expect(atlas::pg_type<atlas::pg_uuid>::oid == 2950_u);
        expect(atlas::pg_type<std::chrono::system_clock::time_point>::oid == 1184_u);
    };
};
