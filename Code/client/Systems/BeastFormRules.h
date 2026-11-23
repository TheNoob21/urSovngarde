#pragma once

#include <cstdint>

// Which races are the player's beast forms. Plain values in and out.
namespace BeastForm
{
enum class Kind
{
    None,
    Werewolf,
    VampireLord
};

// Where a plugin sits in this game's load order, or kNotLoaded.
constexpr int kNotLoaded = -1;

// Skyrim.esm is always first, so the werewolf race has one id everywhere.
constexpr uint32_t kWerewolfRace = 0x000CDD84;
// Dawnguard.esm records are known by their id inside the plugin; the load order index goes in front.
constexpr uint32_t kVampireLordRaceBase = 0x00283A;
constexpr uint32_t kVampireLordArmorBase = 0x011A84;

// The form id a Dawnguard.esm record has in this game, 0 when Dawnguard is not loaded (index 0 is Skyrim.esm).
constexpr uint32_t DawnguardFormId(int aDawnguardIndex, uint32_t aBaseId) noexcept
{
    return aDawnguardIndex > 0 && aDawnguardIndex < 0xFE ? (static_cast<uint32_t>(aDawnguardIndex) << 24) | aBaseId : 0;
}

constexpr Kind Classify(uint32_t aRaceFormId, int aDawnguardIndex) noexcept
{
    if (aRaceFormId == 0)
        return Kind::None;
    if (aRaceFormId == kWerewolfRace)
        return Kind::Werewolf;
    if (aRaceFormId == DawnguardFormId(aDawnguardIndex, kVampireLordRaceBase))
        return Kind::VampireLord;
    return Kind::None;
}

// What the race poll remembers between polls.
struct RacePoll
{
    uint32_t Last = 0;

    // True when the race just became a beast form. One Dawnguard lookup result is passed in per poll.
    bool Observe(uint32_t aRaceFormId, int aDawnguardIndex) noexcept
    {
        if (aRaceFormId == Last)
            return false;

        const Kind cKind = Classify(aRaceFormId, aDawnguardIndex);
        // A race that is no beast form while Dawnguard is not found may still be the vampire lord: forget it so the next poll looks again.
        Last = cKind == Kind::None && aDawnguardIndex <= 0 ? 0 : aRaceFormId;
        return cKind != Kind::None;
    }
};
} // namespace BeastForm
