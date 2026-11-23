#pragma once

// Which quests the party shares, by editor id: plain values, so the lists are unit-tested. One list for every quest
// service, so a quest is never both kept to each player and shared with its targets.
//
// The caller decides which quest types a list applies to: the quest service keeps only None and Miscellaneous quests
// personal by these lists (two Main quests start with "WI"), while alias sync takes radiant quests of any type (the
// Companions and Thieves Guild jobs are typed).

#include <string_view>

namespace QuestFamilies
{
struct Prefix
{
    std::string_view Text;
    // The prefix only counts when a digit follows it: BQ01, not BQuestSomething.
    bool DigitNext;
};

// Each player's own life, and encounters each game's story manager starts by itself: never shared.
// DGIntimidate: a brawl, a fist fight whose opponent each game picks from its own dialogue.
inline constexpr Prefix kPersonal[]{
    {"Relationship", false}, {"BYOH", false}, {"HousePurchase", false}, {"DialogueFollower", false}, {"PerksQuest", false},   {"Tutorial", false}, {"JailQuest", false},
    {"WE", false},           {"WI", false},   {"DLC2WE", false},        {"weroad", false},           {"DGIntimidate", false},
};

// Radiant quests: each game picks their targets (a bandit chief, an item, a place) for itself. Shared only while the
// party shares those targets too (radiant quest alias sync); otherwise each player keeps their own.
inline constexpr Prefix kRadiant[]{
    {"BQ", true}, // bounties from the jarls' stewards and innkeepers
    {"CR", true}, // Companions jobs
    {"CompanionsRadiant", false},
    {"TGR", false}, // Thieves Guild jobs
    {"TGTQ", true}, // Thieves Guild city influence
    {"MGR", true},  // College of Winterhold (MGR01-30); not MGRitual, the ritual spells
    {"MGRApp", false},
    {"MGRShell", false},
    {"DBRecurring", false}, // Dark Brotherhood
    {"DLC1Radiant", false}, // Dawnguard
};

enum class Family
{
    Shared,
    Personal,
    Radiant
};

template <size_t N> constexpr bool Matches(std::string_view aEditorId, const Prefix (&acPrefixes)[N]) noexcept
{
    for (const Prefix& cPrefix : acPrefixes)
    {
        if (!aEditorId.starts_with(cPrefix.Text))
            continue;
        if (!cPrefix.DigitNext)
            return true;
        const size_t cNext = cPrefix.Text.size();
        if (aEditorId.size() > cNext && aEditorId[cNext] >= '0' && aEditorId[cNext] <= '9')
            return true;
    }
    return false;
}

constexpr Family Classify(std::string_view aEditorId) noexcept
{
    if (Matches(aEditorId, kPersonal))
        return Family::Personal;
    if (Matches(aEditorId, kRadiant))
        return Family::Radiant;
    return Family::Shared;
}

// Whether a quest stays each player's own. aRadiantTargetsShared: the server's Gameplay:bEnableQuestAliasSync.
constexpr bool StaysPersonal(std::string_view aEditorId, bool aRadiantTargetsShared) noexcept
{
    switch (Classify(aEditorId))
    {
    case Family::Personal: return true;
    case Family::Radiant: return !aRadiantTargetsShared;
    default: return false;
    }
}
} // namespace QuestFamilies
