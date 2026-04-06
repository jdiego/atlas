#include <catch2/catch_test_macros.hpp>

#include "atlas/schema/type_traits.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time checks
// ---------------------------------------------------------------------------

STATIC_REQUIRE(atlas::pg_type<int32_t>::sql_name == "INTEGER");
STATIC_REQUIRE(atlas::pg_type<std::string>::sql_name == "TEXT");
STATIC_REQUIRE(atlas::pg_type<bool>::sql_name == "BOOLEAN");
STATIC_REQUIRE(atlas::pg_type<int16_t>::sql_name == "SMALLINT");
STATIC_REQUIRE(atlas::pg_type<int64_t>::sql_name == "BIGINT");
STATIC_REQUIRE(atlas::pg_type<float>::sql_name == "FLOAT4");
STATIC_REQUIRE(atlas::pg_type<double>::sql_name == "FLOAT8");
STATIC_REQUIRE(atlas::pg_type<std::vector<uint8_t>>::sql_name == "BYTEA");
STATIC_REQUIRE(atlas::pg_type<atlas::pg_uuid>::sql_name == "UUID");
STATIC_REQUIRE(atlas::pg_type<std::chrono::system_clock::time_point>::sql_name == "TIMESTAMPTZ");

// pg_mappable concept satisfied for mapped types
STATIC_REQUIRE(atlas::pg_mappable<double>);
STATIC_REQUIRE(atlas::pg_mappable<int32_t>);
STATIC_REQUIRE(atlas::pg_mappable<std::string>);
STATIC_REQUIRE(atlas::pg_mappable<atlas::pg_uuid>);

// pg_mappable concept NOT satisfied for unmapped types
STATIC_REQUIRE(!atlas::pg_mappable<std::vector<int>>);
STATIC_REQUIRE(!atlas::pg_mappable<unsigned int>);
STATIC_REQUIRE(!atlas::pg_mappable<char>);

// ---------------------------------------------------------------------------
// Runtime OID checks
// ---------------------------------------------------------------------------

TEST_CASE("pg_type OIDs are correct", "[type_traits]") {
    CHECK(atlas::pg_type<bool>::oid                                      == 16u);
    CHECK(atlas::pg_type<int16_t>::oid                                   == 21u);
    CHECK(atlas::pg_type<int32_t>::oid                                   == 23u);
    CHECK(atlas::pg_type<int64_t>::oid                                   == 20u);
    CHECK(atlas::pg_type<float>::oid                                     == 700u);
    CHECK(atlas::pg_type<double>::oid                                    == 701u);
    CHECK(atlas::pg_type<std::string>::oid                               == 25u);
    CHECK(atlas::pg_type<std::vector<uint8_t>>::oid                      == 17u);
    CHECK(atlas::pg_type<atlas::pg_uuid>::oid                            == 2950u);
    CHECK(atlas::pg_type<std::chrono::system_clock::time_point>::oid     == 1184u);
}
