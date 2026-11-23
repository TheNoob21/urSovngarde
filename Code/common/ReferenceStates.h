#pragma once

// The state of a world reference that one player's game changed, for the others: plain values, so the rules are unit-tested.
//
// Each kind follows the game's own reset:
// - A destructible's damage stage. An ore vein's depleted look is one: MineOreScript damages the vein by half and sets it
//   destroyed, and clears both when its cell resets (OnReset). Only veins and the masters' destructibles without scripts
//   or a harmful explosion are shared: setting a stage in another game runs the reference's stage scripts there (a quest's
//   urn, a siege's barricade), and its explosions (an oil pool's fire).
// - A bear trap armed or sprung. The game arms one each time it loads (TrapBear.onLoad), so this goes only to the players
//   who have it loaded, and is never kept.
// - A reference used up: a plant picked (flora, a tree with produce, a nirnroot) or a rune trap set off. The game brings it
//   back when its cell resets.
//
// A kept state is the shared world's only while that world could not have reset it: it goes to a player who comes later
// while the game hours since no player had its place loaded stay under the game's respawn time (iHoursToRespawnCell).
// That is the game's own rule, a cell resets once it has been unloaded that long, with "unloaded" meaning in every game. A
// game that reports a reference back as new ends what is kept of it, unless another player has its place loaded, and a game
// that has just reset one itself is not given another game's state for it (Resets). The respawn time is Skyrim's 240 hours:
// it is a game setting, which nothing here reads, so a plugin that changes it is not weighed; a cleared dungeon's longer
// time is not weighed either, so its states end sooner.
//
// Each yield stays with the player who took it: a picked plant's ingredient and a vein's ore are given only in the game of
// the player who picked or mined. Another game sets the plant picked without an item, and the vein depleted and destroyed
// as MineOreScript leaves an emptied one, without ore.
//
// Known limits: a bear trap a player loads later is armed by its own game while the others may see it sprung, until someone
// uses or springs it. The server takes where a reference is from the first game that reports it; the cell check and the
// sender's interaction bound what a wrong place can do.

#include <ActivationCatchUp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace ReferenceStates
{
enum class Kind : uint8_t
{
    //! Value: the destruction stage as GetCurrentDestructionStage gives it; 0 is undamaged.
    Destruction = 0,
    //! Value: 1 armed, 0 sprung. A bear trap.
    Trap = 1,
    //! Value: 1 used up, 0 as new again.
    Used = 2,
};

//! A stage past this is refused: Skyrim.esm's destructibles have at most five.
inline constexpr uint8_t kMaxStage = 32;

[[nodiscard]] constexpr bool IsKnown(uint8_t aKind) noexcept
{
    return aKind <= static_cast<uint8_t>(Kind::Used);
}

[[nodiscard]] constexpr bool IsValid(uint8_t aKind, uint8_t aValue) noexcept
{
    switch (aKind)
    {
    case static_cast<uint8_t>(Kind::Destruction): return aValue <= kMaxStage;
    case static_cast<uint8_t>(Kind::Trap):
    case static_cast<uint8_t>(Kind::Used): return aValue <= 1;
    default: return false;
    }
}

//! Kept for a player who comes later: all but a bear trap, which its game arms again as it loads.
[[nodiscard]] constexpr bool IsKept(Kind aKind) noexcept
{
    return aKind != Kind::Trap;
}

//! A kept kind back as new, as its cell reset makes it: the kept state goes, and nothing is passed on.
[[nodiscard]] constexpr bool IsReset(Kind aKind, uint8_t aValue) noexcept
{
    return IsKept(aKind) && aValue == 0;
}

//! A use of the reference this recent counts as the sender's interaction: a vein is depleted some seconds after its use.
inline constexpr std::chrono::seconds kUseWindow{60};

//! How near the sender's own character must be to a reference it did not use: a bear trap is stepped on, a destructible
//! or a rune trap can be hit with an arrow or a spell from a bow's range (a plant is picked, a use the server sees).
[[nodiscard]] constexpr float ReachOf(Kind aKind) noexcept
{
    return aKind == Kind::Trap ? 1024.f : 4096.f;
}

struct Point
{
    float X{};
    float Y{};
    float Z{};
};

[[nodiscard]] constexpr bool IsWithin(const Point& acFrom, const Point& acTo, float aReach) noexcept
{
    const float cX = acFrom.X - acTo.X;
    const float cY = acFrom.Y - acTo.Y;
    const float cZ = acFrom.Z - acTo.Z;
    return cX * cX + cY * cY + cZ * cZ <= aReach * aReach;
}

enum class Refusal : uint8_t
{
    None,
    UnknownKind,
    BadValue,
    OffCell,
    NoInteraction
};

// Whether the server takes a player's report. aInCell: the sender stands in the reference's cell, or outdoors has its grid
// loaded. aUsedRecently: the sender's own character used the reference within kUseWindow. aNear: it stands within ReachOf.
// A reference back as new needs no interaction: the cell reset it, wherever the player stands, and the report only ends
// what is kept.
[[nodiscard]] constexpr Refusal Check(uint8_t aKind, uint8_t aValue, bool aInCell, bool aUsedRecently, bool aNear) noexcept
{
    if (!IsKnown(aKind))
        return Refusal::UnknownKind;
    if (!IsValid(aKind, aValue))
        return Refusal::BadValue;
    if (!aInCell)
        return Refusal::OffCell;
    if (!aUsedRecently && !aNear && !IsReset(static_cast<Kind>(aKind), aValue))
        return Refusal::NoInteraction;
    return Refusal::None;
}

//! Skyrim's iHoursToRespawnCell (a game setting in Skyrim.esm), the respawn time a kept state is weighed against.
inline constexpr int32_t kDefaultRespawnHours = 240;
//! A year: a longer respawn time is taken as this.
inline constexpr int32_t kMaxRespawnHours = 24 * 365;
//! Idle hours past this go over the wire as this; any respawn time is shorter.
inline constexpr uint32_t kMaxIdleHours = 0xFFFF;
static_assert(kMaxRespawnHours < static_cast<int32_t>(kMaxIdleHours));

[[nodiscard]] constexpr int32_t RespawnHours(int32_t aSetting) noexcept
{
    if (aSetting <= 0)
        return kDefaultRespawnHours;
    return aSetting > kMaxRespawnHours ? kMaxRespawnHours : aSetting;
}

//! Whole game hours from aLastSeen to aNow, on the server's calendar; never negative, at most kMaxIdleHours.
[[nodiscard]] constexpr uint32_t IdleHours(double aNow, double aLastSeen) noexcept
{
    const double cIdle = aNow - aLastSeen;
    if (!(cIdle > 0.0))
        return 0;
    if (cIdle >= static_cast<double>(kMaxIdleHours))
        return kMaxIdleHours;
    return static_cast<uint32_t>(cIdle);
}

//! Whether a kept state still holds in a game whose iHoursToRespawnCell is aSetting: the shared world had its cell unloaded
//! for aIdleHours. At the respawn time itself the game resets the cell.
[[nodiscard]] constexpr bool IsFresh(uint32_t aIdleHours, int32_t aSetting) noexcept
{
    return static_cast<int64_t>(aIdleHours) < RespawnHours(aSetting);
}

// The bases were found in the game's masters by the scripts on them (TrapBear, NirnrootACTIVATORScript,
// MineOreFurnitureScript) and by the placed projectiles in their cells (PGRE).

//! BearTrap01, the only base with TrapBear in Skyrim.esm and its DLCs.
inline constexpr uint32_t kBearTrap = 0x0007144D;

//! The rune traps in Skyrim.esm's and Dragonborn.esm's dungeons are placed projectiles of these Skyrim.esm bases.
inline constexpr std::array<uint32_t, 6> kRuneProjectiles{
    0x0007326E, // TrapRuneFrostProjectile
    0x00073323, // TrapRuneFireProjectile
    0x00073324, // TrapRuneLightningProjectile
    0x0005DB91, // RuneFireProjectile
    0x0005FBBF, // RuneFrostProjectile
    0x000665BA, // RuneLightningProjectile
};

//! Picked by NirnrootACTIVATORScript, which disables them; OnReset enables them again.
inline constexpr std::array<uint32_t, 2> kNirnroots{
    0x000E1FB2, // TreeFloraNirnroot01
    0x000B6FB9, // TreeFloraNirnrootRed01
};

//! The furniture an ore vein links to (MineOreFurnitureScript).
inline constexpr std::array<uint32_t, 6> kSkyrimMiningFurniture{
    0x000613A6, // PickaxeMiningFloorMarker
    0x000613A7, // PickaxeMiningTableMarker
    0x000E2BC7, // PickaxeMiningWallMarker
    0x000BD15C, // MS02PickaxeMiningWallMarker
    0x000BD15D, // MS02PickaxeMiningFloorMarker
    0x000F9AF8, // MS02PickaxeMiningTableMarker
};

inline constexpr std::array<uint32_t, 4> kDragonbornMiningFurniture{
    0x0003A491, // DLC2PickaxeMiningFloorMarker_Stalhrim
    0x0003A492, // DLC2PickaxeMiningTableMarker_Stalhrim
    0x0003A493, // DLC2PickaxeMiningWallMarker_Stalhrim
    0x0003C71A, // PickaxeMiningTableMarker_IgnoredBySandbox
};

//! The masters' destructibles without scripts whose stages move and hurt nothing (a web's strip explosion does no
//! damage); an oil pool's fire, a statue's push and the scripted ones are left out.
inline constexpr std::array<uint32_t, 22> kSkyrimPlainDestructibles{
    0x00022F0D, // DA10BoethiahStatue
    0x0002267B, // MS04Hearth
    0x00027EF9, // GuardMarker
    0x0002AA49, // dunFrostmereCryptPaleBladeBase
    0x000327C7, // MG05Rubble
    0x0003985D, // StockadeBarricade01DONOTUSE
    0x00061501, // FXSpiderwebHitStripACT
    0x0007D901, // TrapFallingOilLampExtender
    0x00092492, // DA01ShrineofAzura
    0x00096FEB, // DA07DisplayRobes
    0x00096FEC, // DA07DisplayCommentaries
    0x00096FED, // DA07DisplayMysteriumXarxes
    0x00096FEE, // DA07DisplayTapestries
    0x0009C6DF, // GrainMill
    0x000A05C2, // dunRebelsCairnSwordBase
    0x000C1907, // dunRebelsCairnSwordUpgraded
    0x000D2656, // FarmhouseWindMill
    0x000EC3DE, // FXspiderWebKitDestructible
    0x000ECEBC, // dunReachwaterRockGauldurAmuletFragmentSaarthalACT
    0x000ECEBD, // dunReachwaterRockGauldurAmuletFragmentFolgunthurACT
    0x000ECEBE, // dunReachwaterRockGauldurAmuletFragmentGeirmundACT
    0x0010B161, // FarmhouseWindMillWR
};

inline constexpr std::array<uint32_t, 3> kDragonbornPlainDestructibles{
    0x00027E53, // ApoControlRingColumn01
    0x00027E54, // ApoControlRingTop01
    0x0003A9A9, // DLC2FXspiderWebKitDestructibleNoObs
};

// aBaseFormId: as this game has it; Skyrim.esm is always loaded first.
[[nodiscard]] constexpr bool IsBearTrap(uint32_t aBaseFormId) noexcept
{
    return aBaseFormId == kBearTrap;
}

[[nodiscard]] constexpr bool IsRuneProjectile(uint32_t aBaseFormId) noexcept
{
    return std::find(kRuneProjectiles.begin(), kRuneProjectiles.end(), aBaseFormId) != kRuneProjectiles.end();
}

[[nodiscard]] constexpr bool IsNirnroot(uint32_t aBaseFormId) noexcept
{
    return std::find(kNirnroots.begin(), kNirnroots.end(), aBaseFormId) != kNirnroots.end();
}

// aObjectId: the base form id within its plugin (the low 24 bits).
[[nodiscard]] constexpr bool IsPlainDestructible(ActivationCatchUp::Plugin aPlugin, uint32_t aObjectId) noexcept
{
    const auto cContains = [aObjectId](const auto& acBases)
    {
        return std::find(acBases.begin(), acBases.end(), aObjectId) != acBases.end();
    };
    switch (aPlugin)
    {
    case ActivationCatchUp::Plugin::Skyrim: return cContains(kSkyrimPlainDestructibles);
    case ActivationCatchUp::Plugin::Dragonborn: return cContains(kDragonbornPlainDestructibles);
    default: return false;
    }
}

// A destructible linked to this furniture is an ore vein, which MineOreScript also sets destroyed as it depletes.
// aObjectId: the furniture's base form id within its plugin (the low 24 bits).
[[nodiscard]] constexpr bool IsMiningFurniture(ActivationCatchUp::Plugin aPlugin, uint32_t aObjectId) noexcept
{
    const auto cContains = [aObjectId](const auto& acBases)
    {
        return std::find(acBases.begin(), acBases.end(), aObjectId) != acBases.end();
    };
    switch (aPlugin)
    {
    case ActivationCatchUp::Plugin::Skyrim: return cContains(kSkyrimMiningFurniture);
    case ActivationCatchUp::Plugin::Dragonborn: return cContains(kDragonbornMiningFurniture);
    default: return false;
    }
}
} // namespace ReferenceStates
