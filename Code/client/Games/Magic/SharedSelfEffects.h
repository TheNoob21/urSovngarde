#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <Games/Magic/MagicSystem.h>

// Which of a player's own effects other players see on that player's copy (MagicStateService), and which never go to them.
namespace SharedSelfEffects
{
// ActiveEffect flags: its conditions fail for now, or it was dispelled and is going.
constexpr uint32_t kInactive = 1u << 15;
constexpr uint32_t kDispelled = 1u << 18;

// Effects other games show through their own sync, that only touch the caster's own screen or world, or that would act there.
constexpr bool IsShownArchetype(const EffectArchetypes::ArchetypeID aArchetype) noexcept
{
    using namespace EffectArchetypes;
    switch (aArchetype)
    {
    case kSummonCreature:
    case kBoundWeapon:
    case kWerewolf:
    case kWerewolfFeed:
    case kVampireLord:
    case kSlowTime:
    case kNightEye:
    case kDetectLife:
    case kReanimate:
    case kTelekinesis:
    case kGrabActor:
    case kCommandSummoned:
    case kSpawnHazard:
    case kSpawnScriptedRef:
    case kGuide:
    case kSoulTrap:
    case kDispel:
    case kCureDisease:
    case kCureParalysis:
    case kCureAddiction:
    case kCurePoison: return false;
    default: return true;
    }
}

// Something to see on the body: a shader, an art object, a light, or an effect that is its own look.
constexpr bool HasVisual(const bool aShader, const bool aArt, const bool aLight, const EffectArchetypes::ArchetypeID aArchetype) noexcept
{
    return aShader || aArt || aLight || aArchetype == EffectArchetypes::kInvisibility || aArchetype == EffectArchetypes::kEtherealize;
}

//! What the owner's game reads about one effect on its player.
struct Facts
{
    //! The player cast it on themselves (a spell, a power, a potion), not another player or an NPC.
    bool FromSelf{};
    float Elapsed{};
    float Duration{};
    uint32_t Flags{};
    EffectArchetypes::ArchetypeID Archetype{EffectArchetypes::kNone};
    bool Visual{};
    //! A shrine's blessing or a standing stone's: the owner decided these stay with the player who used them.
    bool Personal{};
    //! A VR control plugin's (VRIK, HIGGS, Conduit): it acts in its own game only.
    bool LocalControl{};
};

//! Whether the effect goes in the state other players see: a lasting, visible effect the player put on themselves.
//! Abilities, worn enchantments and diseases have no duration, so they never go.
constexpr bool IsShared(const Facts& acFacts) noexcept
{
    if (!acFacts.FromSelf || acFacts.Personal || acFacts.LocalControl || !acFacts.Visual)
        return false;
    if ((acFacts.Flags & (kInactive | kDispelled)) != 0)
        return false;
    if (!(acFacts.Duration > 0.f) || !(acFacts.Elapsed < acFacts.Duration))
        return false;
    return IsShownArchetype(acFacts.Archetype);
}

struct PluginForm
{
    std::string_view Plugin;
    uint32_t BaseId;
};

// The shrines' blessings all carry the shrine's cure (CureDiseaseEffect), the Daedric altars and the DLC ones too.
constexpr PluginForm kShrineCureEffect{"Skyrim.esm", 0x0FBFF5};

// What a standing stone gives the player who uses it: the Guardian stones' and the others' abilities, and the Solstheim
// stones' blessings. The stones' powers (Shadow, Serpent, Ritual) are cast by the player later, like any power, and are not here.
constexpr std::array<PluginForm, 18> kStandingStoneSpells{{
    {"Skyrim.esm", 0x0E5F45},     // Thief
    {"Skyrim.esm", 0x0E5F47},     // Mage
    {"Skyrim.esm", 0x0E5F4C},     // Warrior
    {"Skyrim.esm", 0x0E5F4E},     // Apprentice
    {"Skyrim.esm", 0x10E439},     // Apprentice, its weakness
    {"Skyrim.esm", 0x103A94},     // Apprentice, its magicka
    {"Skyrim.esm", 0x0E5F51},     // Atronach
    {"Skyrim.esm", 0x0E5F54},     // Lady
    {"Skyrim.esm", 0x0E5F58},     // Lord
    {"Skyrim.esm", 0x0E5F5A},     // Lover
    {"Skyrim.esm", 0x0E5F5E},     // Steed
    {"Skyrim.esm", 0x0E7328},     // Tower
    {"Dragonborn.esm", 0x01DF9C}, // Sun
    {"Dragonborn.esm", 0x01DF9F}, // Earth
    {"Dragonborn.esm", 0x01DFA4}, // Beast
    {"Dragonborn.esm", 0x01DFA5}, // Wind
    {"Dragonborn.esm", 0x01DFA7}, // Water
    {"Dragonborn.esm", 0x01DFAB}, // Tree
}};

constexpr bool SamePlugin(const std::string_view acA, const std::string_view acB) noexcept
{
    if (acA.size() != acB.size())
        return false;
    for (size_t i = 0; i < acA.size(); ++i)
    {
        const char cA = acA[i] >= 'A' && acA[i] <= 'Z' ? static_cast<char>(acA[i] - 'A' + 'a') : acA[i];
        const char cB = acB[i] >= 'A' && acB[i] <= 'Z' ? static_cast<char>(acB[i] - 'A' + 'a') : acB[i];
        if (cA != cB)
            return false;
    }
    return true;
}

constexpr bool IsStandingStoneSpell(const std::string_view acPlugin, const uint32_t aBaseId) noexcept
{
    for (const auto& cForm : kStandingStoneSpells)
        if (cForm.BaseId == aBaseId && SamePlugin(cForm.Plugin, acPlugin))
            return true;
    return false;
}
} // namespace SharedSelfEffects
