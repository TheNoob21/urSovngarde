#include <catch2/catch.hpp>
#include <Systems/ModIdMapping.h>

TEST_CASE("Mod IDs follow server mappings across different local load orders", "[mod_id_mapping]")
{
    ModIdMapping first, second;
    REQUIRE(first.Add(2, false, 7, false));
    REQUIRE(first.Add(0x123, true, 8, true));
    REQUIRE(second.Add(5, false, 7, false));
    REQUIRE(second.Add(0x456, true, 8, true));
    uint32_t mod = 99, base = 99;
    REQUIRE(first.ToServer(0x02012345, mod, base));
    CHECK(mod == 7); CHECK(base == 0x12345);
    CHECK(second.ToGame(mod, base) == 0x05012345);
    REQUIRE(first.ToServer(0xFE123ABC, mod, base));
    CHECK(mod == 8); CHECK(base == 0xABC);
    CHECK(second.ToGame(mod, base) == 0xFE456ABC);
}

TEST_CASE("Missing mappings fail closed and zero is a valid server mod", "[mod_id_mapping]")
{
    ModIdMapping mapping;
    uint32_t mod = 123, base = 456;
    CHECK_FALSE(mapping.ToServer(0x01001234, mod, base));
    CHECK(mod == 0); CHECK(base == 0);
    mod = 123; base = 456;
    CHECK_FALSE(mapping.ToServer(0xFE001123, mod, base));
    CHECK(mod == 0); CHECK(base == 0);
    CHECK(mapping.ToGame(123, 456) == 0);
    REQUIRE(mapping.Add(1, false, 0, false));
    REQUIRE(mapping.ToServer(0x01001234, mod, base));
    CHECK(mod == 0); CHECK(base == 0x1234);
    CHECK(mapping.ToGame(mod, base) == 0x01001234);
    CHECK(mapping.ToGame(0, 0) == 0);
    REQUIRE(mapping.ToServer(0, mod, base));
    CHECK(mod == 0); CHECK(base == 0);
}

TEST_CASE("Reconnect clears mappings while temporary IDs remain reserved", "[mod_id_mapping]")
{
    ModIdMapping mapping;
    REQUIRE(mapping.Add(1, false, 5, false));
    REQUIRE(mapping.Add(2, true, 6, true));
    mapping.Reset();
    uint32_t mod = 1, base = 1;
    CHECK_FALSE(mapping.ToServer(0x01001234, mod, base));
    CHECK_FALSE(mapping.ToServer(0xFE002123, mod, base));
    CHECK(mapping.ToGame(5, 0x1234) == 0);
    REQUIRE(mapping.ToServer(0xFF123456, mod, base));
    CHECK(mod == 0xFFFFFFFF); CHECK(base == 0x123456);
    CHECK(mapping.ToGame(mod, base) == 0);
    CHECK_FALSE(mapping.Add(0xFF, false, 3, false));
    CHECK_FALSE(mapping.Add(0xFE, false, 3, false));
    CHECK_FALSE(mapping.Add(0x1000, true, 3, true));
    CHECK_FALSE(mapping.Add(3, true, 3, false));
    CHECK_FALSE(mapping.Add(3, false, 0xFFFFFFFF, false));
    REQUIRE(mapping.Add(4, false, 5, false));
    CHECK(mapping.ToGame(5, 0x1234) == 0x04001234);
}

TEST_CASE("Missing base-plugin mappings stay distinct from null forms", "[mod_id_mapping]")
{
    ModIdMapping mapping;
    uint32_t mod = 9;
    uint32_t base = 9;
    REQUIRE_FALSE(mapping.ToServer(0x00001234, mod, base));
    CHECK(mod == 0);
    CHECK(base == 0);
    REQUIRE(mapping.ToServer(0, mod, base));
    REQUIRE(mapping.Add(0, false, 0, false));
    REQUIRE(mapping.ToServer(0x00001234, mod, base));
    CHECK(mod == 0);
    CHECK(base == 0x1234);
    CHECK(mapping.ToGame(mod, base) == 0x00001234);
}

TEST_CASE("Conflicting mod mappings are rejected without changing prior identities", "[mod_id_mapping]")
{
    ModIdMapping mapping;
    REQUIRE(mapping.Add(2, false, 7, false));
    CHECK_FALSE(mapping.Add(3, false, 7, false));
    CHECK_FALSE(mapping.Add(2, false, 8, false));
    CHECK_FALSE(mapping.Add(4, false, 9, true));
    CHECK(mapping.ToGame(7, 0x1234) == 0x02001234);
    CHECK(mapping.ToGame(8, 0x1234) == 0);
    REQUIRE(mapping.Add(4, false, 9, false));
    REQUIRE(mapping.Add(0xFFF, true, 10, true));
    CHECK_FALSE(mapping.Add(0xFFF, true, 11, true));
    CHECK_FALSE(mapping.Add(1, true, 10, true));
    uint32_t mod = 0;
    uint32_t base = 0;
    REQUIRE(mapping.ToServer(0xFEFFFFFF, mod, base));
    CHECK(mod == 10);
    CHECK(base == 0xFFF);
    CHECK(mapping.ToGame(mod, base) == 0xFEFFFFFF);
}
