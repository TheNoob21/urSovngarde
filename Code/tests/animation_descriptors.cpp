#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>

#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.h>

namespace
{
// The master (humanoid) graph's synced variables as release 6 sent them, by graph variable number, in slot order.
// A value travels by its slot, so a build that moves one of these slots reads the other game's values as other variables.
constexpr uint32_t kReleaseSixBooleans[] = {
    129, 41,  205, 186, 130, 120, 76,  67,  68,  52,  21,  25,  51,  70,  71,  72,  73,  75,  80,  81, 82,  85, 89, 90,  92,  93,  98,  108, 116, 121,
    123, 125, 126, 137, 151, 152, 164, 169, 177, 185, 198, 200, 202, 206, 210, 211, 212, 214, 215, 36, 128, 83, 84, 110, 111, 291, 165, 204, 28,  251,
};

constexpr uint32_t kReleaseSixFloats[] = {
    2, 1, 40, 59, 0, 69, 54, 183, 16, 176, 62, 63, 91,
};

constexpr uint32_t kReleaseSixIntegers[] = {
    179, 180, 77, 94, 60, 35, 3, 255, 48, 112, 37, 171, 181, 113,
};

constexpr uint32_t kInSwimState = 280;

// The graph key of each creature graph added in release 7: a hash of its variable names, read from the game's behavior file.
constexpr uint64_t kNewCreatureKeys[] = {
    15308051897993541273ULL, // AtronachFlame
    6930025242429736649ULL,  // BenthicLurker
    17742465010135092947ULL, // DwarvenSphere
    2547439886223928659ULL,  // DwarvenBallista
    5077838081812273209ULL,  // IceWraith
    17787828040730466272ULL, // Netch
    2401375257788400503ULL,  // Riekling
    4699000156716604007ULL,  // Wisp
    16841759923128033557ULL, // Slaughterfish
    10814878731587440876ULL, // ChaurusFlyer
    2345404321706051729ULL,  // Seeker
    6076589067610934853ULL,  // Witchlight
    5375154407281404432ULL,  // DragonPriest
    4668141266727947797ULL,  // BoarRieklingBearMain
    13070820648827025441ULL, // BoarRieklingBoarMain
};

template <size_t N> bool StartsWith(const TiltedPhoques::Vector<uint32_t>& acList, const uint32_t (&acPrefix)[N])
{
    return acList.size() >= N && std::equal(std::begin(acPrefix), std::end(acPrefix), acList.begin());
}
} // namespace

TEST_CASE("The master graph keeps every slot it had and adds the swim state after them", "[animvars]")
{
    const AnimationGraphDescriptor* pMaster = AnimationGraphDescriptorManager::Get().GetDescriptor(AnimationGraphDescriptor_Master_Behavior::m_key);
    REQUIRE(pMaster);

    REQUIRE(StartsWith(pMaster->BooleanLookUpTable, kReleaseSixBooleans));
    REQUIRE(StartsWith(pMaster->FloatLookupTable, kReleaseSixFloats));
    REQUIRE(StartsWith(pMaster->IntegerLookupTable, kReleaseSixIntegers));

    REQUIRE(pMaster->BooleanLookUpTable.size() == std::size(kReleaseSixBooleans) + 1);
    REQUIRE(pMaster->BooleanLookUpTable.back() == kInSwimState);
    REQUIRE(pMaster->FloatLookupTable.size() == std::size(kReleaseSixFloats));
    REQUIRE(pMaster->IntegerLookupTable.size() == std::size(kReleaseSixIntegers));

    // The counts the sender announces are the lists' sizes, so a game with a longer list than its peer applies only the slots both have.
    REQUIRE(pMaster->SharedBooleans == pMaster->BooleanLookUpTable.size());
    REQUIRE(pMaster->SharedFloats == pMaster->FloatLookupTable.size());
    REQUIRE(pMaster->SharedIntegers == pMaster->IntegerLookupTable.size());
}

TEST_CASE("Creatures that had no descriptor have one, each under its own key", "[animvars]")
{
    std::set<uint64_t> seen;
    for (const uint64_t cKey : kNewCreatureKeys)
    {
        REQUIRE(seen.insert(cKey).second);
        REQUIRE(cKey != AnimationGraphDescriptor_Master_Behavior::m_key);

        const AnimationGraphDescriptor* pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(cKey);
        REQUIRE(pDescriptor);
        // A creature graph syncs how it moves: at least a speed or direction, and the idle and locomotion state.
        REQUIRE_FALSE(pDescriptor->FloatLookupTable.empty());
        REQUIRE_FALSE(pDescriptor->IntegerLookupTable.empty());
        REQUIRE(pDescriptor->SharedBooleans == pDescriptor->BooleanLookUpTable.size());
        REQUIRE(pDescriptor->SharedFloats == pDescriptor->FloatLookupTable.size());
        REQUIRE(pDescriptor->SharedIntegers == pDescriptor->IntegerLookupTable.size());
    }
}
