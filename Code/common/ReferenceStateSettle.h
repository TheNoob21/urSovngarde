#pragma once

// How a game brings its references to the states other games sent (ReferenceStates.h), and which changes of its own it
// sends: plain values, so the rules are unit-tested.
//
// The game's own way is used where it has no effect beyond the reference: a bear trap is used until it is as the other game
// has it, and a destructible is damaged until it reaches the other game's stage. A picked plant has the flag the game keeps
// for it set; a nirnroot and a rune gone off are switched off, as a nirnroot's own script would also count the pick and give
// the item.

#include <ActivationCatchUp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ReferenceStates
{
using Clock = std::chrono::steady_clock;

//! A state for a reference that is not loaded here waits this long for it, as a use does (ActivationCatchUp::kWait).
inline constexpr auto kWait = ActivationCatchUp::kWait;

// The bear traps here, as their script's animations show them: Trigger01 springs one (stepped on, or used while armed),
// Reset01 arms one (used while sprung), StartOpen arms one as it loads. A trap with no animation since it loaded is in the
// script's first state, sprung.
class BearTraps
{
public:
    //! The script takes no use while an animation plays (its Busy state); one plays about this long.
    static constexpr auto kBusy = std::chrono::milliseconds(2500);
    //! An animation this soon after a use made here for another game's word is that use's, and is not sent back.
    static constexpr auto kOwnWindow = std::chrono::seconds(3);
    //! Uses at most to bring a trap to another game's state: one should do.
    static constexpr uint32_t kMaxUses = 2;
    //! Traps remembered; the ones seen longest ago go past this, and a reloaded one plays StartOpen again.
    static constexpr size_t kMaxTraps = 64;

    // Its script played an animation; aArmed whether it arms the trap, aLoad whether its load did. True when it is this
    // game's own change, to send: not a load, nor a use made here for another game.
    bool Played(uint32_t aFormId, bool aArmed, bool aLoad, Clock::time_point aNow)
    {
        Seen& seen = Remember(aFormId, aNow);
        const bool cForOther = seen.UsedForOther != Clock::time_point{} && aNow - seen.UsedForOther < kOwnWindow;
        seen.Armed = aArmed;
        seen.At = aNow;
        if (aLoad || cForOther)
            return false;
        // This game's own change wins over an older word from another game.
        std::erase_if(m_wanted, [aFormId](const Wanted& acWanted) { return acWanted.FormId == aFormId; });
        return true;
    }

    // Another game's trap is aArmed now; a newer word replaces an older one.
    void Want(uint32_t aFormId, bool aArmed, Clock::time_point aNow)
    {
        std::erase_if(m_wanted, [aFormId](const Wanted& acWanted) { return acWanted.FormId == aFormId; });
        m_wanted.push_back({aFormId, aArmed, aNow, 0});
    }

    // The trap to use now, if any (0 for none): one that differs from another game's word and is not busy.
    // aLoaded(formId): whether it is loaded here; one that is not waits kWait.
    template <class TLoaded> [[nodiscard]] uint32_t Next(Clock::time_point aNow, TLoaded&& aLoaded)
    {
        for (size_t i = 0; i < m_wanted.size();)
        {
            Wanted& wanted = m_wanted[i];
            if (!aLoaded(wanted.FormId))
            {
                if (aNow - wanted.Since >= kWait)
                {
                    m_wanted.erase(m_wanted.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
                continue;
            }

            // Busy while an animation plays, or one asked for here has not begun.
            const auto cIt = m_seen.find(wanted.FormId);
            const bool cArmed = cIt != m_seen.end() && cIt->second.Armed;
            if (cIt != m_seen.end() && aNow - std::max(cIt->second.At, cIt->second.UsedForOther) < kBusy)
            {
                ++i;
                continue;
            }
            if (cArmed == wanted.Armed || wanted.Uses >= kMaxUses)
            {
                m_wanted.erase(m_wanted.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            ++wanted.Uses;
            Remember(wanted.FormId, aNow).UsedForOther = aNow;
            return wanted.FormId;
        }
        return 0;
    }

    [[nodiscard]] std::optional<bool> IsArmed(uint32_t aFormId) const noexcept
    {
        const auto cIt = m_seen.find(aFormId);
        if (cIt == m_seen.end())
            return std::nullopt;
        return cIt->second.Armed;
    }

    //! The other games' words go with the connection; what this game's traps show stays.
    void ClearWanted() noexcept { m_wanted.clear(); }
    [[nodiscard]] size_t WantedCount() const noexcept { return m_wanted.size(); }
    [[nodiscard]] size_t SeenCount() const noexcept { return m_seen.size(); }

private:
    struct Seen
    {
        bool Armed{};
        Clock::time_point At{};
        Clock::time_point UsedForOther{};
    };

    struct Wanted
    {
        uint32_t FormId;
        bool Armed;
        Clock::time_point Since;
        uint32_t Uses;
    };

    Seen& Remember(uint32_t aFormId, Clock::time_point aNow)
    {
        if (!m_seen.contains(aFormId) && m_seen.size() >= kMaxTraps)
        {
            const auto cOldest = std::min_element(
                m_seen.begin(), m_seen.end(), [](const auto& acLeft, const auto& acRight)
                { return std::max(acLeft.second.At, acLeft.second.UsedForOther) < std::max(acRight.second.At, acRight.second.UsedForOther); });
            m_seen.erase(cOldest);
        }
        Seen& seen = m_seen[aFormId];
        if (seen.At == Clock::time_point{})
            seen.At = aNow - kBusy;
        return seen;
    }

    std::unordered_map<uint32_t, Seen> m_seen;
    std::vector<Wanted> m_wanted;
};

// Another game's destruction stage, reached here by damaging the reference a little at a time (DamageObject): a stage
// begins at a share of a health this game does not show. The damage doubles while the stage holds, and starts small again
// at each new stage, so one step rarely passes two stages. An ore vein cannot pass its one stage (its stage caps the
// damage); a destructible with stages close together can, and is then left one stage further on, as damage is not undone.
class StageSettle
{
public:
    //! A damage shows in the stage within a frame or two.
    static constexpr auto kStepGap = std::chrono::milliseconds(150);
    static constexpr float kFirstStep = 1.f;
    static constexpr float kMaxStep = 256.f;
    //! Steps at most for one reference; then it is left as it is.
    static constexpr uint32_t kMaxSteps = 48;

    // A higher stage replaces a lower one waiting for the same reference.
    void Add(uint32_t aFormId, uint8_t aStage, Clock::time_point aNow)
    {
        for (Entry& entry : m_entries)
        {
            if (entry.FormId == aFormId)
            {
                entry.Target = std::max(entry.Target, aStage);
                return;
            }
        }
        m_entries.push_back({aFormId, aStage, aNow});
    }

    // aStageOf(formId) -> std::optional<int32_t>, its stage here, empty when it is not loaded, negative when it is not one to
    // set here (it is dropped). aDamage(formId, damage) damages it. aReached(formId) once it is at or past the stage, ore
    // veins being then set destroyed as their script does.
    template <class TStageOf, class TDamage, class TReached> void Update(Clock::time_point aNow, TStageOf&& aStageOf, TDamage&& aDamage, TReached&& aReached)
    {
        for (size_t i = 0; i < m_entries.size();)
        {
            Entry& entry = m_entries[i];
            if (aNow < entry.NextAt)
            {
                ++i;
                continue;
            }

            const std::optional<int32_t> cStage = aStageOf(entry.FormId);
            if (!cStage)
            {
                if (aNow - entry.Since >= kWait || entry.Steps > 0)
                {
                    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
                continue;
            }
            if (*cStage < 0)
            {
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (*cStage >= static_cast<int32_t>(entry.Target))
            {
                const uint32_t cFormId = entry.FormId;
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                aReached(cFormId);
                continue;
            }
            if (entry.Steps >= kMaxSteps)
            {
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            if (entry.Steps == 0 || *cStage != entry.LastStage)
                entry.Step = kFirstStep;
            else
                entry.Step = std::min(entry.Step * 2.f, kMaxStep);
            entry.LastStage = *cStage;
            ++entry.Steps;
            entry.NextAt = aNow + kStepGap;
            aDamage(entry.FormId, entry.Step);
            ++i;
        }
    }

    [[nodiscard]] bool IsSettling(uint32_t aFormId) const noexcept
    {
        return std::any_of(m_entries.begin(), m_entries.end(), [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; });
    }

    void Cancel(uint32_t aFormId) noexcept
    {
        std::erase_if(m_entries, [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; });
    }

    void Clear() noexcept { m_entries.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t FormId;
        uint8_t Target;
        Clock::time_point Since;
        Clock::time_point NextAt{};
        int32_t LastStage{};
        float Step{};
        uint32_t Steps{};
    };

    std::vector<Entry> m_entries;
};

// Other games' "used up" words waiting for their reference: each is applied once it has been loaded here kSettle, so its
// own scripts ran first (a nirnroot places its glow as its cell attaches) and another game's use of it, replayed as an
// activation, has landed. A reference not loaded within kWait is passed over.
class Pending
{
public:
    static constexpr auto kSettle = std::chrono::seconds(1);
    //! Applied at most this many at once: setting a plant reloads its model.
    static constexpr size_t kPerUpdate = 8;

    void Add(uint32_t aFormId, uint8_t aValue, Clock::time_point aNow)
    {
        std::erase_if(m_entries, [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; });
        m_entries.push_back({aFormId, aValue, aNow});
    }

    // aLoaded(formId): whether it is loaded here. The ones to apply now, as (formId, value).
    template <class TLoaded> [[nodiscard]] std::vector<std::pair<uint32_t, uint8_t>> Ready(Clock::time_point aNow, TLoaded&& aLoaded)
    {
        std::vector<std::pair<uint32_t, uint8_t>> ready;
        for (size_t i = 0; i < m_entries.size() && ready.size() < kPerUpdate;)
        {
            Entry& entry = m_entries[i];
            if (!aLoaded(entry.FormId))
            {
                entry.LoadedSince = {};
                if (aNow - entry.Since >= kWait)
                {
                    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
                continue;
            }
            if (entry.LoadedSince == Clock::time_point{})
                entry.LoadedSince = aNow;
            if (aNow - entry.LoadedSince < kSettle)
            {
                ++i;
                continue;
            }
            ready.emplace_back(entry.FormId, entry.Value);
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return ready;
    }

    void Cancel(uint32_t aFormId) noexcept
    {
        std::erase_if(m_entries, [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; });
    }

    void Clear() noexcept { m_entries.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t FormId;
        uint8_t Value;
        Clock::time_point Since;
        Clock::time_point LoadedSince{};
    };

    std::vector<Entry> m_entries;
};

// This game's reports, sent a few a second at most so the server, which takes eight a second from each player, never
// drops one: a burst (a garden picked, a row of runes) waits its turn. Past kMaxWaiting the oldest is dropped.
template <class TReport> class SendPacer
{
public:
    static constexpr uint32_t kPerSecond = 6;
    static constexpr size_t kMaxWaiting = 256;

    // False when a report waiting longest had to go to make room.
    bool Push(TReport aReport)
    {
        const bool cRoom = m_waiting.size() < kMaxWaiting;
        if (!cRoom)
            m_waiting.erase(m_waiting.begin());
        m_waiting.push_back(std::move(aReport));
        return cRoom;
    }

    // aSend(report) for each that may go now, oldest first.
    template <class TSend> void Drain(Clock::time_point aNow, TSend&& aSend)
    {
        if (m_since == Clock::time_point{} || aNow - m_since >= std::chrono::seconds(1))
        {
            m_since = aNow;
            m_sent = 0;
        }
        size_t i = 0;
        for (; i < m_waiting.size() && m_sent < kPerSecond; ++i, ++m_sent)
            aSend(m_waiting[i]);
        m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void Clear() noexcept { m_waiting.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_waiting.size(); }

private:
    std::vector<TReport> m_waiting;
    Clock::time_point m_since{};
    uint32_t m_sent{};
};

// The references this game's own cell reset brought back as new just now: another game's state for one of them is not
// forced over that reset, the kept state the server sent as this game loaded the cell included. A newer state, after the
// window, is applied as usual.
class Resets
{
public:
    static constexpr auto kWindow = std::chrono::seconds(30);

    void Note(uint32_t aFormId, Clock::time_point aNow) { m_at[aFormId] = aNow; }

    [[nodiscard]] bool IsRecent(uint32_t aFormId, Clock::time_point aNow) const noexcept
    {
        const auto cIt = m_at.find(aFormId);
        return cIt != m_at.end() && aNow - cIt->second < kWindow;
    }

    void Forget(Clock::time_point aNow)
    {
        std::erase_if(m_at, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kWindow; });
    }

    void Clear() noexcept { m_at.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_at.size(); }

private:
    std::unordered_map<uint32_t, Clock::time_point> m_at;
};

//! What a watched reference is now, for UseWatch.
enum class Look : uint8_t
{
    //! Its cell is not loaded here: nothing is known.
    Unloaded,
    //! Loaded, and as new.
    New,
    //! Loaded, and used up: a plant picked, a rune gone off.
    Used
};

// The references used up here, watched so that each change is sent once:
// - one this player used (picked a plant), sent as used once the game shows it so, or forgotten after kCheck;
// - a rune trap loaded as new, sent as used once it has gone off;
// - one used up here, sent as new again once it is loaded as new: its cell reset it.
class UseWatch
{
public:
    //! A use shows within this long (a nirnroot's script disables it a moment later).
    static constexpr auto kCheck = std::chrono::seconds(5);
    static constexpr size_t kMaxWatched = 512;

    // This player used aFormId just now.
    void Used(uint32_t aFormId, Clock::time_point aNow)
    {
        if (m_watched.contains(aFormId))
            return;
        if (m_checks.size() < kMaxWatched)
            m_checks[aFormId] = aNow;
    }

    // A rune trap loaded here, to send when it goes off.
    void Armed(uint32_t aFormId)
    {
        if (!m_watched.contains(aFormId) && m_armed.size() < kMaxWatched)
            m_armed.insert(aFormId);
    }

    // aFormId was used up here for another game's word just now: not sent, but once the game shows it used, watched for its
    // cell's reset. One that never shows it used is forgotten, so a use that did not take is not sent as a reset.
    void Applied(uint32_t aFormId, Clock::time_point aNow)
    {
        m_checks.erase(aFormId);
        m_armed.erase(aFormId);
        m_watched.erase(aFormId);
        if (m_applied.size() < kMaxWatched)
            m_applied[aFormId] = aNow;
    }

    // aLook(formId) -> Look. aSend(formId, used) for each change to send.
    template <class TLook, class TSend> void Update(Clock::time_point aNow, TLook&& aLook, TSend&& aSend)
    {
        for (auto it = m_applied.begin(); it != m_applied.end();)
        {
            const uint32_t cFormId = it->first;
            if (aLook(cFormId) == Look::Used)
            {
                it = m_applied.erase(it);
                if (m_watched.size() < kMaxWatched)
                    m_watched.insert(cFormId);
                continue;
            }
            it = aNow - it->second >= kCheck ? m_applied.erase(it) : std::next(it);
        }

        for (auto it = m_checks.begin(); it != m_checks.end();)
        {
            const uint32_t cFormId = it->first;
            if (aLook(cFormId) == Look::Used)
            {
                it = m_checks.erase(it);
                m_armed.erase(cFormId);
                if (m_watched.size() < kMaxWatched)
                    m_watched.insert(cFormId);
                aSend(cFormId, true);
                continue;
            }
            it = aNow - it->second >= kCheck ? m_checks.erase(it) : std::next(it);
        }

        for (auto it = m_armed.begin(); it != m_armed.end();)
        {
            const uint32_t cFormId = *it;
            const Look cLook = aLook(cFormId);
            if (cLook == Look::New)
            {
                ++it;
                continue;
            }
            it = m_armed.erase(it);
            if (cLook == Look::Used)
            {
                if (m_watched.size() < kMaxWatched)
                    m_watched.insert(cFormId);
                aSend(cFormId, true);
            }
        }

        for (auto it = m_watched.begin(); it != m_watched.end();)
        {
            const uint32_t cFormId = *it;
            if (aLook(cFormId) == Look::New)
            {
                it = m_watched.erase(it);
                aSend(cFormId, false);
                continue;
            }
            ++it;
        }
    }

    void Clear() noexcept
    {
        m_checks.clear();
        m_applied.clear();
        m_armed.clear();
        m_watched.clear();
    }

    [[nodiscard]] bool IsWatched(uint32_t aFormId) const noexcept
    {
        return m_watched.contains(aFormId) || m_armed.contains(aFormId) || m_checks.contains(aFormId) || m_applied.contains(aFormId);
    }

private:
    std::unordered_map<uint32_t, Clock::time_point> m_checks;
    std::unordered_map<uint32_t, Clock::time_point> m_applied;
    std::unordered_set<uint32_t> m_armed;
    std::unordered_set<uint32_t> m_watched;
};
} // namespace ReferenceStates
