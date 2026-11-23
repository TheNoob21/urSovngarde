#pragma once

#include <cstdint>

#include <TiltedCore/Stl.hpp>

// Work on this game's copy of another game's actor: seating it on its horse or in furniture, and taking it off again.
// It is decided in the client's update, which runs on one of the game's job threads (and beside the main thread during a load),
// and done at the start of the next frame on the main thread, the way removed copies are switched off. There it is done only if
// what it was decided under still holds: the same actor (by handle, as the game reuses a freed form id) with its 3D, still a copy,
// no loading screen since, the game not exiting, and the decision not withdrawn (its ticket still live). A decision is withdrawn
// the moment the copy (or, for a ride, its horse) changes owner or is told something else, so work queued before that is dropped.
namespace CopyWork
{
//! How long an undo waits for a loading screen to go before it is dropped.
inline constexpr uint64_t kHoldUndoMs = 60000;

//! What a piece of work was decided under.
struct Stamp
{
    //! The decision it belongs to (Tickets::Issue), or 0 for an undo.
    uint32_t Ticket{};
    //! The loading screens queued by then (UI::LoadingScreensQueued).
    uint32_t LoadingScreens{};
    //! An undo: the decision whose seat it takes away again. It is done only while that seat is the one in place, so it never
    //! takes away a later one.
    uint32_t Undoes{};
};

//! What the main thread finds when the work comes up.
struct Found
{
    bool Exiting{};
    //! A loading screen is up now.
    bool Loading{};
    uint32_t LoadingScreens{};
    //! The actor its handle names now is the one the work was for, not deleted, and has its 3D.
    bool Actor{};
    //! It is still another game's copy here, not taken over by this game.
    bool Copy{};
    //! The decision's ticket is still live.
    bool Live{};
    //! The decision whose seat is in place on the copy now (Tickets::InPlace), 0 for none.
    uint32_t InPlace{};
    //! How long the work has waited so far.
    uint64_t WaitedMs{};
};

enum class Verdict : uint8_t
{
    kDo,
    //! Not now: looked at again next frame.
    kHold,
    kDrop,
};

//! A decision is dropped by anything that changed since it was made. An undo waits out a loading screen, and is done on the same
//! actor while the seat it undoes is still in place and the actor is still a copy; at the end of a session (aSessionOver) every
//! actor is this game's again, and the undo still runs.
constexpr Verdict Check(const Stamp& acStamp, const Found& acFound, const bool aSessionOver) noexcept
{
    if (acFound.Exiting)
        return Verdict::kDrop;

    if (acStamp.Ticket == 0)
    {
        if (acFound.Loading)
            return acFound.WaitedMs < kHoldUndoMs ? Verdict::kHold : Verdict::kDrop;
        if (acStamp.Undoes == 0 || acFound.InPlace != acStamp.Undoes)
            return Verdict::kDrop;
        return acFound.Actor && (acFound.Copy || aSessionOver) ? Verdict::kDo : Verdict::kDrop;
    }

    if (acFound.Loading || acFound.LoadingScreens != acStamp.LoadingScreens || !acFound.Live)
        return Verdict::kDrop;
    return acFound.Actor && acFound.Copy ? Verdict::kDo : Verdict::kDrop;
}

//! The game's load state as the main thread reads it at one moment.
struct Moment
{
    bool Exiting{};
    bool Loading{};
    uint32_t LoadingScreens{};
};

//! Right before an engine call, once the verdict was kDo: whether it still goes ahead. aAtVerdict was read before the actor was looked
//! up, aNow just now in the same main-thread step. A load seen in between drops a decision and holds an undo for the next frame.
constexpr Verdict Recheck(const Stamp& acStamp, const Moment& acAtVerdict, const Moment& acNow) noexcept
{
    if (acNow.Exiting)
        return Verdict::kDrop;
    if (!acAtVerdict.Loading && !acNow.Loading && acNow.LoadingScreens == acAtVerdict.LoadingScreens)
        return Verdict::kDo;
    return acStamp.Ticket == 0 ? Verdict::kHold : Verdict::kDrop;
}

//! What becomes of a copy handed over to be taken off its horse and then deleted.
enum class DeleteStep : uint8_t
{
    kReleaseThenDelete,
    //! Not on a horse: deleted as it is.
    kDelete,
    //! On a horse it cannot be taken off yet (a load, or no 3D): looked at again next frame, however long that takes.
    kWait,
    //! Gone already, or the game is exiting.
    kNothing,
};

//! aFirst is the undo's verdict (Check), aVerdict what is left of it after Recheck. Only kDo takes the copy off; otherwise a ridden copy
//! is never deleted, it waits until it can be taken off or the game frees it. aRidden: the copy is on a horse, or the seat this game
//! put in place is still the one in place.
constexpr DeleteStep StepForDelete(const Verdict aFirst, const Verdict aVerdict, const Found& acFound, const bool aThere, const bool aRidden) noexcept
{
    // Recheck drops an undo only for the exit.
    if (acFound.Exiting || (aFirst == Verdict::kDo && aVerdict == Verdict::kDrop))
        return DeleteStep::kNothing;
    if (aVerdict == Verdict::kDo)
        return DeleteStep::kReleaseThenDelete;
    if (aVerdict == Verdict::kHold || acFound.Loading)
        return DeleteStep::kWait;
    if (!aThere)
        return DeleteStep::kNothing;
    return aRidden ? DeleteStep::kWait : DeleteStep::kDelete;
}

//! Native work the client's update does on copies itself (graph reads and writes, a copy's recovery, bone reads): it waits while the
//! game exits or a loading screen is up, and for aSettleMs after a load, so that nothing kept from before the load is used on what
//! the load made again. One gate per caller, kept between its passes.
struct LoadGate
{
    uint32_t Seen{};
    uint64_t ChangedAtMs{};
    bool Known{};
};

inline constexpr uint64_t kSettleAfterLoadMs = 500;

constexpr bool Settled(LoadGate& aGate, const bool aExiting, const bool aLoading, const uint32_t aLoadingScreens, const uint64_t aNowMs) noexcept
{
    if (!aGate.Known || aLoadingScreens != aGate.Seen || aLoading)
    {
        aGate.Seen = aLoadingScreens;
        aGate.ChangedAtMs = aNowMs;
        aGate.Known = true;
    }
    return !aExiting && !aLoading && aNowMs - aGate.ChangedAtMs >= kSettleAfterLoadMs;
}

//! The decision each copy's work belongs to, by a key of the service's (a server id), and the decision whose seat is in place on
//! it. A new decision withdraws the last one's work.
class Tickets
{
public:
    uint32_t Issue(const uint32_t aKey) noexcept
    {
        if (++m_last == 0)
            ++m_last;
        m_live[aKey] = m_last;
        return m_last;
    }

    void Retire(const uint32_t aKey) noexcept { m_live.erase(aKey); }

    [[nodiscard]] bool Live(const uint32_t aKey, const uint32_t aTicket) const noexcept
    {
        const auto it = m_live.find(aKey);
        return aTicket != 0 && it != m_live.end() && it->second == aTicket;
    }

    //! The main thread put a seat in place under aTicket (0: it took it away).
    void Place(const uint32_t aKey, const uint32_t aTicket) noexcept
    {
        if (aTicket)
            m_inPlace[aKey] = aTicket;
        else
            m_inPlace.erase(aKey);
    }

    [[nodiscard]] uint32_t InPlace(const uint32_t aKey) const noexcept
    {
        const auto it = m_inPlace.find(aKey);
        return it != m_inPlace.end() ? it->second : 0;
    }

    void Clear() noexcept
    {
        m_live.clear();
        m_inPlace.clear();
    }

private:
    TiltedPhoques::Map<uint32_t, uint32_t> m_live;
    TiltedPhoques::Map<uint32_t, uint32_t> m_inPlace;
    uint32_t m_last{};
};
} // namespace CopyWork
