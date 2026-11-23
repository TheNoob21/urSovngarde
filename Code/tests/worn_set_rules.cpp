#include <catch2/catch.hpp>

#include <Services/Generic/WornSetRules.h>

using namespace WornSetRules;

namespace
{
constexpr uint32_t kHead = 0x1;
constexpr uint32_t kBody = 0x4;
constexpr uint32_t kHands = 0x8;
constexpr uint32_t kShield = 0x200;
} // namespace

TEST_CASE("A copy that wears what its owner wears changes nothing", "[wornset]")
{
    const auto cChanges = Compare({{0x10, kBody}, {0x11, kHead}, {0x12, kHands}}, {{0x12, kHands}, {0x10, kBody}, {0x11, kHead}});
    REQUIRE(cChanges.Remove.empty());
    REQUIRE(cChanges.Add.empty());
}

TEST_CASE("A piece changed out of range is taken off and the new one put on", "[wornset]")
{
    const auto cChanges = Compare({{0x10, kBody}, {0x21, kHead}}, {{0x10, kBody}, {0x20, kHead}});
    REQUIRE(cChanges.Remove == std::vector<uint32_t>{0x20});
    REQUIRE(cChanges.Add == std::vector<uint32_t>{0x21});
}

TEST_CASE("A piece the copy wears stays when nothing the owner wears takes its slot", "[wornset]")
{
    // The owner may wear a piece here from a plugin this game cannot name: it never reached the snapshot.
    const auto cChanges = Compare({{0x10, kBody}}, {{0x10, kBody}, {0x30, kHead}, {0x31, kHands}});
    REQUIRE(cChanges.Remove.empty());
    REQUIRE(cChanges.Add.empty());
}

TEST_CASE("A copy with nothing on gets the owner's pieces", "[wornset]")
{
    const auto cChanges = Compare({{0x40, kBody}, {0x41, kShield}}, {});
    REQUIRE(cChanges.Remove.empty());
    REQUIRE(cChanges.Add == std::vector<uint32_t>{0x40, 0x41});
}

TEST_CASE("Only a piece whose slot is taken comes off", "[wornset]")
{
    const auto cChanges = Compare({{0x50, kBody | kHands}}, {{0x60, kHands}, {0x61, kHead}, {0x62, kShield}});
    REQUIRE(cChanges.Remove == std::vector<uint32_t>{0x60});
    REQUIRE(cChanges.Add == std::vector<uint32_t>{0x50});
}

TEST_CASE("A repeated id is listed once", "[wornset]")
{
    const auto cChanges = Compare({{0x70, kBody}, {0x70, kBody}}, {{0x71, kBody}, {0x71, kBody}});
    REQUIRE(cChanges.Remove == std::vector<uint32_t>{0x71});
    REQUIRE(cChanges.Add == std::vector<uint32_t>{0x70});
}

TEST_CASE("A piece its owner wore before and took off comes off with nothing in its place", "[wornset]")
{
    // A helmet, a ring and a cuirass taken off: the owner's last list had them, its new one does not.
    const auto cChanges = Compare({{0x10, kBody}}, {{0x10, kBody}, {0x30, kHead}, {0x31, kHands}}, {0x10, 0x30, 0x31});
    REQUIRE(cChanges.Remove == std::vector<uint32_t>{0x30, 0x31});
    REQUIRE(cChanges.Add.empty());
}

TEST_CASE("A piece its owner was never known to wear stays when nothing takes its slot", "[wornset]")
{
    // An NPC's own outfit here, where its owner wears a piece this game cannot name: the piece never was on the owner's list.
    const auto cChanges = Compare({{0x10, kHead}}, {{0x10, kHead}, {0x80, kBody}}, {0x10});
    REQUIRE(cChanges.Remove.empty());
    REQUIRE(cChanges.Add.empty());
}

TEST_CASE("Taking everything off leaves the copy with nothing its owner wore", "[wornset]")
{
    const auto cChanges = Compare({}, {{0x90, kBody}, {0x91, kHead}, {0x92, kShield}}, {0x90, 0x91});
    REQUIRE(cChanges.Remove == std::vector<uint32_t>{0x90, 0x91});
    REQUIRE(cChanges.Add.empty());
}

TEST_CASE("A piece put back on is not taken off for having been on the last list", "[wornset]")
{
    const auto cChanges = Compare({{0xA0, kHead}}, {}, {0xA0});
    REQUIRE(cChanges.Remove.empty());
    REQUIRE(cChanges.Add == std::vector<uint32_t>{0xA0});
}
