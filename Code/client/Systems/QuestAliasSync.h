#pragma once

// Radiant quest alias sync: the rules, in plain values (no game types), so they are unit-tested.
//
// The game that picked a radiant quest's targets sends its alias fills when they are new or changed. The server relays
// them from one game per quest run (QuestAliasDeciders), and the others fill the same aliases.
// Off unless the server sets Gameplay:bEnableQuestAliasSync.

#include <Structs/QuestAliases.h>
#include <Systems/QuestFamilies.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace QuestAliasSync
{
// Quests whose targets each game picks for itself (QuestFamilies::kRadiant). Other quests' aliases are fixed by their
// plugin, or are the player's own (followers, spouse, steward), and are left alone.
inline bool IsRadiantQuest(std::string_view aEditorId) noexcept
{
    return QuestFamilies::Classify(aEditorId) == QuestFamilies::Family::Radiant;
}

using Fill = QuestAliases::Fill;

// The wanted fills that the local ones do not match yet, locations first: a reference alias's own conditions and
// scripts may read the location alias. A wanted alias missing from acLocal counts as different.
inline std::vector<Fill> Differences(const std::vector<Fill>& acWanted, const std::vector<Fill>& acLocal)
{
    std::vector<Fill> result;
    for (const uint8_t cKind : {static_cast<uint8_t>(QuestAliases::kLocation), static_cast<uint8_t>(QuestAliases::kRef)})
    {
        for (const Fill& cWanted : acWanted)
        {
            if (cWanted.AliasKind != cKind)
                continue;
            bool same = false;
            for (const Fill& cLocal : acLocal)
            {
                if (cLocal.AliasId == cWanted.AliasId)
                {
                    same = cLocal == cWanted;
                    break;
                }
            }
            if (!same)
                result.push_back(cWanted);
        }
    }
    return result;
}

// A fill a receiver can use: a target, and a kind it knows.
inline bool IsUsable(const Fill& acFill) noexcept
{
    return acFill.Target && (acFill.AliasKind == QuestAliases::kRef || acFill.AliasKind == QuestAliases::kLocation);
}

inline uint32_t Hash(const std::vector<Fill>& acFills) noexcept
{
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](uint32_t aValue)
    {
        for (int i = 0; i < 4; ++i)
        {
            hash ^= (aValue >> (i * 8)) & 0xFF;
            hash *= 16777619u;
        }
    };
    for (const Fill& cFill : acFills)
    {
        mix(cFill.AliasId);
        mix(cFill.AliasKind);
        mix(cFill.Target.ModId);
        mix(cFill.Target.BaseId);
    }
    return hash;
}

// The quests this game offered its own fills for, and what it last sent of each. Changed fills go out at once, unchanged
// ones every kResendEvery, as the war does: for a send the server dropped, a new leader, or a member that drifted.
class SendTracker
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kResendEvery = std::chrono::seconds(30);

    // Checked from now on, even while nothing is filled yet.
    void Offer(uint32_t aQuestFormId) { m_sent.try_emplace(aQuestFormId); }

    bool ShouldSend(uint32_t aQuestFormId, uint32_t aHash, Clock::time_point aNow)
    {
        Sent& sent = m_sent[aQuestFormId];
        if (sent.Hash == aHash && aNow - sent.At < kResendEvery)
            return false;
        sent.Hash = aHash;
        sent.At = aNow;
        return true;
    }

    // Someone joined the party, or a save was loaded: every quest's fills go out again at its next check.
    void Reoffer()
    {
        for (auto& [_, sent] : m_sent)
            sent.Hash.reset();
    }

    std::vector<uint32_t> Quests() const
    {
        std::vector<uint32_t> quests;
        quests.reserve(m_sent.size());
        for (const auto& [cQuest, _] : m_sent)
            quests.push_back(cQuest);
        return quests;
    }

    // Whether this game offers its own fills for the quest: it picked that run's targets.
    bool Offered(uint32_t aQuestFormId) const { return m_sent.find(aQuestFormId) != m_sent.end(); }

    void Forget(uint32_t aQuestFormId) { m_sent.erase(aQuestFormId); }
    void Clear() { m_sent.clear(); }
    bool Empty() const noexcept { return m_sent.empty(); }

private:
    struct Sent
    {
        std::optional<uint32_t> Hash;
        Clock::time_point At;
    };
    std::unordered_map<uint32_t, Sent> m_sent;
};

// The fills another game picked, per quest run. They are applied all together, when every target is found here and no
// scene of the quest plays, and again each time the picking game sends them (a re-pick, or its resend every 30 s). This
// game follows meanwhile: it sends none of its own. The entry ends with the run, or with a start of this game's own.
class Wanted
{
public:
    using Clock = std::chrono::steady_clock;
    // A quest that never runs here, or a target never found here: the fills are dropped after this long unapplied.
    static constexpr auto kGiveUpAfter = std::chrono::minutes(10);

    struct Entry
    {
        std::vector<Fill> Fills;
        // Not applied yet: the fills to compare with this game's when the quest runs here.
        bool Pending = true;
        // The quest ran here since the fills came: a later pass that finds it stopped ends the run.
        bool SeenRunning = false;
        Clock::time_point Since;
        // Logged once per wait: a wait that repeats every pass is noise.
        bool WaitLogged = false;
        // Unapplied for kGiveUpAfter: kept so the same fills, sent again, are not tried again this run.
        bool GaveUp = false;
    };

    // Fills for the quest, applied (again) at the next chance. The same fills sent again keep a wait's start, so a target
    // never found here is still given up on; new fills (a re-pick or a new run) wait afresh.
    void Set(uint32_t aQuestFormId, std::vector<Fill> aFills, Clock::time_point aNow)
    {
        auto [it, cInserted] = m_wanted.try_emplace(aQuestFormId);
        Entry& entry = it->second;
        if (!cInserted && entry.Fills == aFills)
        {
            if (entry.GaveUp)
                return;
            if (!entry.Pending)
            {
                entry.Since = aNow;
                entry.WaitLogged = false;
            }
            entry.Pending = true;
            return;
        }
        const bool cSeenRunning = entry.SeenRunning;
        entry = Entry{std::move(aFills), true, cSeenRunning, aNow, false, false};
    }

    Entry* Find(uint32_t aQuestFormId)
    {
        auto it = m_wanted.find(aQuestFormId);
        return it == m_wanted.end() ? nullptr : &it->second;
    }

    // A save was loaded: its own picks came back, so every quest's fills are applied again.
    void Rearm(Clock::time_point aNow)
    {
        for (auto& [_, entry] : m_wanted)
        {
            if (entry.GaveUp)
                continue;
            entry.Pending = true;
            entry.Since = aNow;
            entry.WaitLogged = false;
        }
    }

    void Forget(uint32_t aQuestFormId) { m_wanted.erase(aQuestFormId); }
    void Clear() { m_wanted.clear(); }
    bool Empty() const noexcept { return m_wanted.empty(); }

    std::vector<uint32_t> Quests() const
    {
        std::vector<uint32_t> quests;
        quests.reserve(m_wanted.size());
        for (const auto& [cQuest, _] : m_wanted)
            quests.push_back(cQuest);
        return quests;
    }

    // Gives up on, and returns, the quests whose fills are still unapplied after kGiveUpAfter. A given-up entry stays
    // for the run, so this game keeps following (it sends none of its own) and ignores the same fills sent again; new
    // fills (a re-pick) are tried afresh.
    std::vector<uint32_t> TakeExpired(Clock::time_point aNow)
    {
        std::vector<uint32_t> expired;
        for (auto& [cQuest, entry] : m_wanted)
        {
            if (entry.Pending && aNow - entry.Since >= kGiveUpAfter)
            {
                entry.Pending = false;
                entry.GaveUp = true;
                expired.push_back(cQuest);
            }
        }
        return expired;
    }

private:
    std::unordered_map<uint32_t, Entry> m_wanted;
};
} // namespace QuestAliasSync
