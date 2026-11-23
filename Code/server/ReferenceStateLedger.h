#pragma once

// The reference states the server passed on this session, for a player whose game was not near them then: plain values, so
// the rules are unit-tested.
//
// A destructible's stage or a picked plant goes live to the games that have it loaded (ReferenceStateService). A game that
// comes later still has it as its own save left it; it gets the states it missed as it comes near, each with the game hours
// since no player had its place loaded, and applies only those its own game would not have reset by now
// (ReferenceStates::IsFresh). A game that reports one back as new (its cell reset it) ends that state here. A player is known
// by connection and forgotten on leaving, so one who comes back, maybe from an older save, gets the kept states again, and two
// players of one name are two games. The session ends when the last player leaves, as the ActivationLedger's does.

#include <ReferenceStates.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

//! How often a player's game is sent the kept states again after it loads a save: one fresh catch-up per kEvery, the asks in
//! between are dropped (one log line each time), so that no game can make the server send catch-ups without end.
class LoadCatchUpLimit
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kEvery = std::chrono::seconds(5);

    enum class Answer
    {
        kTaken,
        kDropped,       // the first drop since the last catch-up: logged
        kDroppedQuietly // dropped again
    };

    Answer Ask(uint32_t aPlayerId, Clock::time_point aNow)
    {
        auto [it, cNew] = m_players.try_emplace(aPlayerId, Last{aNow, false});
        if (cNew)
            return Answer::kTaken;
        if (aNow - it->second.At >= kEvery)
        {
            it->second = Last{aNow, false};
            return Answer::kTaken;
        }
        if (it->second.DropLogged)
            return Answer::kDroppedQuietly;
        it->second.DropLogged = true;
        return Answer::kDropped;
    }

    void Forget(uint32_t aPlayerId) { m_players.erase(aPlayerId); }

private:
    struct Last
    {
        Clock::time_point At;
        bool DropLogged;
    };
    std::unordered_map<uint32_t, Last> m_players;
};

template <class TPlace> class ReferenceStateLedger
{
public:
    //! The oldest recorded states go first past this.
    static constexpr size_t kMaxEntries = 2048;
    //! At most this many go to a player at once (NotifyReferenceStates::kMax); the rest go at the next cell change.
    static constexpr size_t kMaxCatchUp = 256;
    //! The recent uses kept; the oldest go first past this.
    static constexpr size_t kMaxRecentUses = 512;

    using Clock = std::chrono::steady_clock;
    using Kind = ReferenceStates::Kind;

    struct State
    {
        uint64_t Ref;
        Kind What;
        uint8_t Value;
        uint32_t IdleHours;
    };

    enum class Result : uint8_t
    {
        //! A new or changed state, to pass on.
        Changed,
        //! What was kept already; the sender's game has it so.
        Same,
        //! Behind what is kept (a lower stage): the sender's game is sent the kept state.
        Behind,
        //! Reported back as new: the kept state is gone, and nothing is passed on.
        Ended
    };

    // aRef: the reference's server id, its mod id in the upper half. acDoneBy: the players whose games have it so, the sender
    // and those it is passed on to. aNow: game hours on the server's calendar. acAt: where the reference is, as its first
    // reporter's game had it; a later report keeps the first. aLoadedElsewhere: another player has its place loaded now, so
    // the shared world has not left it long enough to reset, whatever the reporting game's own cell did: a report back as new
    // is not taken then.
    Result Record(
        uint64_t aRef, Kind aKind, uint8_t aValue, const TPlace& acWhere, const ReferenceStates::Point& acAt, std::vector<std::string> aDoneBy, double aNow,
        bool aLoadedElsewhere = false)
    {
        if (!ReferenceStates::IsKept(aKind))
            return Result::Changed;

        const auto cExisting = Find(aRef, aKind);
        if (ReferenceStates::IsReset(aKind, aValue))
        {
            if (cExisting == m_entries.end() || aLoadedElsewhere)
                return Result::Same;
            m_entries.erase(cExisting);
            return Result::Ended;
        }

        if (cExisting != m_entries.end())
        {
            cExisting->LastSeen = aNow;
            // A destructible only breaks further until its cell resets.
            if (aValue <= cExisting->Value)
            {
                if (!aDoneBy.empty() && std::find(cExisting->DoneBy.begin(), cExisting->DoneBy.end(), aDoneBy.front()) == cExisting->DoneBy.end())
                    cExisting->DoneBy.push_back(aDoneBy.front());
                return aValue < cExisting->Value ? Result::Behind : Result::Same;
            }
            Entry entry = std::move(*cExisting);
            m_entries.erase(cExisting);
            entry.Value = aValue;
            entry.DoneBy = std::move(aDoneBy);
            m_entries.push_back(std::move(entry));
            return Result::Changed;
        }

        if (m_entries.size() == kMaxEntries)
            m_entries.pop_front();
        m_entries.push_back({aRef, aKind, aValue, acWhere, acAt, std::move(aDoneBy), aNow});
        return Result::Changed;
    }

    //! Where the first report put the reference, if a state of it is kept.
    [[nodiscard]] std::optional<ReferenceStates::Point> PlaceOf(uint64_t aRef) const noexcept
    {
        for (const Entry& entry : m_entries)
        {
            if (entry.Ref == aRef)
                return entry.At;
        }
        return std::nullopt;
    }

    // The players whose places aInRange accepts have the kept states there loaded at aNow.
    template <class TInRange> void Seen(TInRange&& aInRange, double aNow)
    {
        for (Entry& entry : m_entries)
        {
            if (aInRange(entry.Where))
                entry.LastSeen = aNow;
        }
    }

    // The states acPlayer's game does not have among those whose place aInRange accepts, oldest first, each with its idle
    // hours at aNow; had from now on. Call Seen for the other players first, so a place one of them has loaded counts as seen.
    template <class TInRange> [[nodiscard]] std::vector<State> CatchUp(const std::string& acPlayer, TInRange&& aInRange, double aNow)
    {
        std::vector<State> states;
        for (Entry& entry : m_entries)
        {
            if (states.size() == kMaxCatchUp)
                break;
            if (std::find(entry.DoneBy.begin(), entry.DoneBy.end(), acPlayer) != entry.DoneBy.end() || !aInRange(entry.Where))
                continue;
            entry.DoneBy.push_back(acPlayer);
            states.push_back({entry.Ref, entry.What, entry.Value, ReferenceStates::IdleHours(aNow, entry.LastSeen)});
        }
        return states;
    }

    // Whether a report from acPlayer is taken (ReferenceStates::Check). aInCell: the sender stands in the reference's cell, or
    // outdoors has its grid loaded. acSender: where the server has the sender's own character. The reference is where the
    // first kept report put it, else where acClaimed puts it: either way a reporting game's word, bounded by the cell check.
    [[nodiscard]] ReferenceStates::Refusal Judge(
        const std::string& acPlayer, uint64_t aRef, uint8_t aKind, uint8_t aValue, bool aInCell, const std::optional<ReferenceStates::Point>& acSender,
        const ReferenceStates::Point& acClaimed, Clock::time_point aNow) const
    {
        bool withinReach = false;
        if (acSender && ReferenceStates::IsKnown(aKind))
            withinReach = ReferenceStates::IsWithin(*acSender, PlaceOf(aRef).value_or(acClaimed), ReferenceStates::ReachOf(static_cast<Kind>(aKind)));
        return ReferenceStates::Check(aKind, aValue, aInCell, UsedRecently(acPlayer, aRef, aNow), withinReach);
    }

    // acPlayer's own character used aRef just now.
    void NoteUse(const std::string& acPlayer, uint64_t aRef, Clock::time_point aNow)
    {
        const auto cExisting = std::find_if(m_recent.begin(), m_recent.end(), [&](const Recent& acUse) { return acUse.Ref == aRef && acUse.Player == acPlayer; });
        if (cExisting != m_recent.end())
            m_recent.erase(cExisting);
        else if (m_recent.size() == kMaxRecentUses)
            m_recent.pop_front();
        m_recent.push_back({aRef, acPlayer, aNow});
    }

    // Whether acPlayer used aRef within ReferenceStates::kUseWindow of aNow.
    [[nodiscard]] bool UsedRecently(const std::string& acPlayer, uint64_t aRef, Clock::time_point aNow) const
    {
        return std::any_of(
            m_recent.begin(), m_recent.end(), [&](const Recent& acUse) { return acUse.Ref == aRef && acUse.Player == acPlayer && aNow - acUse.At <= ReferenceStates::kUseWindow; });
    }

    // acPlayer's game loaded a save: it has that save's states again, so every kept state may go to it once more. Only its own
    // marks go; its recent uses, which judge its reports, stay.
    void ForgetToldTo(const std::string& acPlayer)
    {
        for (Entry& entry : m_entries)
            std::erase(entry.DoneBy, acPlayer);
    }

    // acPlayer left: whatever its game had is no longer known, and its uses are gone with it.
    void Forget(const std::string& acPlayer)
    {
        for (Entry& entry : m_entries)
            std::erase(entry.DoneBy, acPlayer);
        std::erase_if(m_recent, [&](const Recent& acUse) { return acUse.Player == acPlayer; });
    }

    void Clear() noexcept
    {
        m_entries.clear();
        m_recent.clear();
    }

    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

    [[nodiscard]] std::optional<uint8_t> ValueOf(uint64_t aRef, Kind aKind) const noexcept
    {
        const auto cIt = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& acEntry) { return acEntry.Ref == aRef && acEntry.What == aKind; });
        if (cIt == m_entries.end())
            return std::nullopt;
        return cIt->Value;
    }

private:
    struct Entry
    {
        uint64_t Ref;
        Kind What;
        uint8_t Value;
        TPlace Where;
        ReferenceStates::Point At;
        std::vector<std::string> DoneBy;
        //! Game hours when a player last had its place loaded.
        double LastSeen;
    };

    struct Recent
    {
        uint64_t Ref;
        std::string Player;
        Clock::time_point At;
    };

    typename std::deque<Entry>::iterator Find(uint64_t aRef, Kind aKind)
    {
        return std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& acEntry) { return acEntry.Ref == aRef && acEntry.What == aKind; });
    }

    std::deque<Entry> m_entries;
    std::deque<Recent> m_recent;
};
