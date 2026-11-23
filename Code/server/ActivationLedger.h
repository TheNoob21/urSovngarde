#pragma once

// The activations the server passed on this session, for a player whose game was not near them then: plain values, so the
// rules are unit-tested.
//
// A lever, a chain, a puzzle pillar or a claw ring is used live in the games that have it loaded (ObjectService::OnActivate).
// A game that comes later, or was elsewhere, still had it as before; it gets the uses it missed, oldest first, as it comes
// near, and replays the mechanisms among them (ActivationCatchUp.h). A player is known by name, so a reconnect does not
// replay what that player's game already did. The session ends when the last player leaves, as the ContainerLedger's does.
// A lever or a pillar whose user reported the end state it came to rest in (RequestActivationState) goes as that state, not
// as its uses: a late player's own save may hold some of them already.

#include <ActivationCatchUp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

template <class TPlace> class ActivationLedger
{
public:
    //! The oldest uses go first past this; a session's dungeons hold far fewer mechanisms.
    static constexpr size_t kMaxEntries = 4096;
    //! At most this many go to a player at once (NotifyActivationCatchUp::kMax); the rest go at the next cell change.
    static constexpr size_t kMaxCatchUp = 256;
    //! End states kept; the least recently reported go first past this.
    static constexpr size_t kMaxStates = 1024;
    //! At most this many end states go to a player at once (NotifyActivationCatchUp::kMaxStates).
    static constexpr size_t kMaxStateCatchUp = 64;
    //! A player's end state is taken only for a reference that player used this recently.
    static constexpr std::chrono::seconds kStateWindow{10};
    //! The recent uses kept; the oldest go first past this.
    static constexpr size_t kMaxRecentUses = 256;

    using Clock = std::chrono::steady_clock;

    struct State
    {
        uint64_t Ref;
        ActivationCatchUp::Pose Pose;
    };

    // aRef: the reference's server id, its mod id in the upper half.
    // acDoneBy: the players whose games did it already, the sender and those it was passed on to.
    void Record(uint64_t aRef, const TPlace& acWhere, std::vector<std::string> aDoneBy)
    {
        if (m_entries.size() == kMaxEntries)
            m_entries.pop_front();
        m_entries.push_back({aRef, acWhere, std::move(aDoneBy)});
    }

    // acPlayer used aRef just now.
    void NoteUse(const std::string& acPlayer, uint64_t aRef, Clock::time_point aNow)
    {
        const auto cExisting = std::find_if(m_recent.begin(), m_recent.end(), [&](const Recent& acUse) { return acUse.Ref == aRef && acUse.Player == acPlayer; });
        if (cExisting != m_recent.end())
            m_recent.erase(cExisting);
        else if (m_recent.size() == kMaxRecentUses)
            m_recent.pop_front();
        m_recent.push_back({aRef, acPlayer, aNow});
    }

    // Whether acPlayer used aRef within kStateWindow of aNow.
    [[nodiscard]] bool UsedRecently(const std::string& acPlayer, uint64_t aRef, Clock::time_point aNow) const
    {
        return std::any_of(m_recent.begin(), m_recent.end(), [&](const Recent& acUse) { return acUse.Ref == aRef && acUse.Player == acPlayer && aNow - acUse.At <= kStateWindow; });
    }

    // Whether acPlayer used any reference within kStateWindow of aNow.
    [[nodiscard]] bool UsedAnyRecently(const std::string& acPlayer, Clock::time_point aNow) const
    {
        return std::any_of(m_recent.begin(), m_recent.end(), [&](const Recent& acUse) { return acUse.Player == acPlayer && aNow - acUse.At <= kStateWindow; });
    }

    // The end state aRef came to rest in, in the game of acDoneBy's first player; acDoneBy: the players whose games have it
    // so. It replaces an older one. False, and nothing kept, for a pose no mechanism has.
    bool RecordState(uint64_t aRef, const TPlace& acWhere, ActivationCatchUp::Pose aPose, std::vector<std::string> aDoneBy)
    {
        if (!ActivationCatchUp::IsValidPoseSize(aPose.size()))
            return false;
        const auto cExisting = std::find_if(m_states.begin(), m_states.end(), [aRef](const StateEntry& acEntry) { return acEntry.Value.Ref == aRef; });
        if (cExisting != m_states.end())
            m_states.erase(cExisting);
        else if (m_states.size() == kMaxStates)
            m_states.pop_front();
        m_states.push_back({{aRef, std::move(aPose)}, acWhere, std::move(aDoneBy)});
        return true;
    }

    // The end states acPlayer's game does not have among those whose place aInRange accepts; had from now on.
    template <class TInRange> [[nodiscard]] std::vector<State> CatchUpStates(const std::string& acPlayer, TInRange&& aInRange)
    {
        std::vector<State> states;
        for (StateEntry& entry : m_states)
        {
            if (states.size() == kMaxStateCatchUp)
                break;
            if (std::find(entry.DoneBy.begin(), entry.DoneBy.end(), acPlayer) != entry.DoneBy.end() || !aInRange(entry.Where))
                continue;
            entry.DoneBy.push_back(acPlayer);
            states.push_back(entry.Value);
        }
        return states;
    }

    // The uses acPlayer's game has not done among those whose place aInRange accepts, oldest first; done from now on.
    // A use of a reference with an end state is passed over: the state is sent instead (CatchUpStates).
    template <class TInRange> [[nodiscard]] std::vector<uint64_t> CatchUp(const std::string& acPlayer, TInRange&& aInRange)
    {
        std::vector<uint64_t> refs;
        for (Entry& entry : m_entries)
        {
            if (refs.size() == kMaxCatchUp)
                break;
            if (std::find(entry.DoneBy.begin(), entry.DoneBy.end(), acPlayer) != entry.DoneBy.end() || !aInRange(entry.Where))
                continue;
            entry.DoneBy.push_back(acPlayer);
            if (!HasState(entry.Ref))
                refs.push_back(entry.Ref);
        }
        return refs;
    }

    [[nodiscard]] bool HasState(uint64_t aRef) const noexcept
    {
        return std::any_of(m_states.begin(), m_states.end(), [aRef](const StateEntry& acEntry) { return acEntry.Value.Ref == aRef; });
    }

    void Clear() noexcept
    {
        m_entries.clear();
        m_states.clear();
        m_recent.clear();
    }
    [[nodiscard]] size_t Size() const noexcept { return m_entries.size() + m_states.size(); }
    [[nodiscard]] size_t StateCount() const noexcept { return m_states.size(); }

private:
    struct Entry
    {
        uint64_t Ref;
        TPlace Where;
        std::vector<std::string> DoneBy;
    };

    struct StateEntry
    {
        State Value;
        TPlace Where;
        std::vector<std::string> DoneBy;
    };

    struct Recent
    {
        uint64_t Ref;
        std::string Player;
        Clock::time_point At;
    };

    std::deque<Entry> m_entries;
    std::deque<StateEntry> m_states;
    std::deque<Recent> m_recent;
};
