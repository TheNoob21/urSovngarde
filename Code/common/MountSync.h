#pragma once

#include <cstddef>
#include <cstdint>

#include <CopyWork.h>

// Who rides what, as the games see it. A mount package is only intent: the rider's game reports what it reads back
// (Actor.IsOnMount), and the other games seat their copy of the rider and judge that by reading back too.
namespace MountSync
{
//! How often the rider's game looks at its riders.
inline constexpr double kWatchInterval = 0.25;
//! A rider given a mount package who is not on the horse within this long gave up (or was stopped): nothing is sent.
inline constexpr double kBoardingTimeout = 15.0;
//! While on the horse, the rider's game says so again this often, for games that load the pair later or join.
inline constexpr double kRefreshInterval = 5.0;
//! A copy whose rider was not confirmed for this long gets off: its owner left, or is out of range.
inline constexpr double kSeatStaleAfter = 15.0;
//! How many times, and how far apart, a copy is put on its horse before it is left on foot.
inline constexpr uint8_t kSeatTries = 3;
inline constexpr double kSeatRetryInterval = 1.0;
//! Looks in a row that must find the rider off before the others are told: one read during a cell change is not a dismount.
inline constexpr uint8_t kOffReadsToDismount = 3;
//! ...and that span at least this long. While a rider is boarding or reads off, it is read every frame, not every kWatchInterval.
inline constexpr double kOffTimeToDismount = 0.15;
//! A copy shown climbing on (its graph's "HorseEnter") that is not in the saddle by then is put there at once.
inline constexpr double kClimbTimeout = 2.5;
//! A copy shown climbing off (its graph's "HorseExit") is taken off its horse once it stands, or after this long.
inline constexpr double kExitTimeout = 2.0;
//! The sit state (Actor flags1 bits 14-17) of a rider in the saddle, and of one standing.
inline constexpr uint32_t kSitStateSeated = 3;
inline constexpr uint32_t kSitStateNone = 0;
//! A rider's game that just got on says so again this often for this long, each time as a rider getting on (not a refresh), so
//! that a copy whose first word came before its horse's new owner did still climbs on with its rider.
inline constexpr double kBoardingResendInterval = 0.5;
inline constexpr double kClimbWindow = 2.0;
//! A word for a rider or horse whose copy is not here yet is kept and tried again this long.
inline constexpr double kPendingWordFor = 2.0;
//! The game seats no rider farther than this from its horse (its quick mount refuses). A copy stands on its owner's saddle,
//! so a copy farther away has not been moved there yet: it waits rather than spend a try.
inline constexpr float kSeatRange = 3000.f;

struct Identity
{
    uint32_t Rider{};
    uint32_t RiderEpoch{};
    uint32_t Mount{};
    uint32_t MountEpoch{};
};

constexpr bool Valid(const Identity& acPair) noexcept
{
    // Server ids start at 0 (the first character on a fresh server), so only the epochs, which start at 1, mark "unknown".
    return acPair.Rider != acPair.Mount && acPair.RiderEpoch && acPair.MountEpoch;
}

constexpr bool Current(const Identity& acPair, uint32_t aRiderEpoch, uint32_t aMountEpoch) noexcept
{
    return Valid(acPair) && acPair.RiderEpoch == aRiderEpoch && acPair.MountEpoch == aMountEpoch;
}

struct RequestState
{
    bool RiderOwnedBySender{};
    bool MountOwnedBySender{};
    bool OwnersInSameParty{};
    bool RiderDead{};
    bool MountDead{};
    bool IsMount{};
    bool MountIsPlayer{};
    bool PairInRange{};
    bool SenderInRange{};
    //! The server shares party horses (Gameplay:bPartyHorses). Off, a horse another game runs is not taken this way either.
    bool SharingOn{true};
};

//! A rider's game asks to put its rider on a horse: its own, or, with party horses shared, one a party member's game runs, which then
//! goes to the rider's game.
constexpr bool MayRequest(const Identity& acPair, uint32_t aRiderEpoch, uint32_t aMountEpoch, const RequestState& acState) noexcept
{
    return Current(acPair, aRiderEpoch, aMountEpoch) && acState.RiderOwnedBySender && (acState.MountOwnedBySender || (acState.SharingOn && acState.OwnersInSameParty)) &&
           !acState.RiderDead && !acState.MountDead && acState.IsMount && !acState.MountIsPlayer && acState.PairInRange && acState.SenderInRange;
}

//! The rider's game, about one of its own riders.
struct Watch
{
    double StartedAt{};
    double SentAt{};
    //! Looks in a row that found the rider off its horse (the caller counts them, and resets them on any look that finds it on).
    uint8_t OffReads{};
    //! When the first of those looks was.
    double OffSince{};
    //! When the rider's getting on was first sent.
    double FirstSentAt{};
    bool Sent{};
};

//! Whether a word about this rider says it is getting on (its copies climb on), not that it rides on.
constexpr bool IsBoarding(const Watch& acWatch, bool aAlreadyOn, double aNow) noexcept
{
    return !aAlreadyOn && (!acWatch.Sent || aNow - acWatch.FirstSentAt < kClimbWindow);
}

//! Whether the rider's game looks at this rider every frame: it is getting on, or was just read off.
constexpr bool ReadEveryFrame(const Watch& acWatch) noexcept
{
    return !acWatch.Sent || acWatch.OffReads != 0;
}

enum class WatchStep
{
    Wait,
    SendMount,
    SendDismount,
    Drop,
};

//! What to do with a rider this game runs, from what the game reads back now.
//! aIdsKnown: the rider and the horse both have server ids. Before that the watch holds while the rider is really on the horse.
constexpr WatchStep NextWatchStep(const Watch& acWatch, bool aOnMount, bool aIdsKnown, double aNow) noexcept
{
    if (aOnMount)
    {
        if (aIdsKnown && (!acWatch.Sent || aNow - acWatch.SentAt >= kRefreshInterval))
            return WatchStep::SendMount;
        // Just got on: said again a few times, so that a copy that missed the first word still climbs on.
        if (aIdsKnown && aNow - acWatch.FirstSentAt < kClimbWindow && aNow - acWatch.SentAt >= kBoardingResendInterval)
            return WatchStep::SendMount;
        return WatchStep::Wait;
    }

    if (acWatch.Sent)
        return acWatch.OffReads >= kOffReadsToDismount && aNow - acWatch.OffSince >= kOffTimeToDismount ? WatchStep::SendDismount : WatchStep::Wait;

    return aNow - acWatch.StartedAt >= kBoardingTimeout ? WatchStep::Drop : WatchStep::Wait;
}

//! A rider this game runs that reads as on a horse with no watch (it connected, came back or loaded a save while riding,
//! so no mount package told of it): looked for this often, and given a watch on the horse it rides.
inline constexpr double kAlreadyOnScanInterval = 2.0;
//! How far from the rider that horse may stand (units): a seated rider stands where its horse does.
inline constexpr float kAlreadyOnUnits = 192.f;

//! A horse near such a rider.
struct NearHorse
{
    uint32_t FormId{};
    float Distance{};
    //! The game reads it as ridden (Actor.IsBeingRidden).
    bool Ridden{};
    //! Another rider of this game is watched on it already.
    bool Watched{};
};

//! The horse the rider is on: the nearest within kAlreadyOnUnits that is ridden and no other rider is watched on. 0 when none.
inline uint32_t HorseRiddenHere(const NearHorse* apHorses, const std::size_t aCount) noexcept
{
    uint32_t best = 0;
    float bestDistance = kAlreadyOnUnits;
    for (std::size_t i = 0; i < aCount; ++i)
    {
        const NearHorse& cHorse = apHorses[i];
        if (!cHorse.FormId || !cHorse.Ridden || cHorse.Watched || !(cHorse.Distance <= bestDistance))
            continue;
        best = cHorse.FormId;
        bestDistance = cHorse.Distance;
    }
    return best;
}

//! Another game's rider, on this game's copy.
struct Seat
{
    double ToldAt{};
    double TriedAt{};
    uint8_t Tries{};
    bool Seated{};
    bool GaveUp{};
    //! The rider was just getting on in its owner's game (not a refresh): the copy is shown climbing on.
    bool ShowClimb{};
    //! Shown climbing on since ClimbAt, not in the saddle yet.
    bool Climbing{};
    double ClimbAt{};
    //! Since when ShowClimb holds: when the word that its rider is getting on came.
    double BoardingAt{};
};

//! A refresh for a copy that gave up: one more try, as its horse may only now be loaded or close enough.
constexpr void RetryAfterRefresh(Seat& aSeat) noexcept
{
    if (!aSeat.GaveUp)
        return;
    aSeat.GaveUp = false;
    aSeat.Tries = aSeat.Tries >= kSeatTries ? static_cast<uint8_t>(kSeatTries - 1) : aSeat.Tries;
}

enum class ClimbStep
{
    Wait,
    Done,
    Instant,
};

//! A copy shown climbing on: in the saddle (done), still climbing, or put straight into the saddle as the climb did not take.
constexpr ClimbStep NextClimbStep(uint32_t aSitState, double aClimbingFor) noexcept
{
    if (aSitState == kSitStateSeated)
        return ClimbStep::Done;
    return aClimbingFor >= kClimbTimeout ? ClimbStep::Instant : ClimbStep::Wait;
}

//! A copy shown climbing off is taken off its horse once it stands, or after kExitTimeout.
constexpr bool ExitDone(uint32_t aSitState, double aExitingFor) noexcept
{
    return aSitState == kSitStateNone || aExitingFor >= kExitTimeout;
}

enum class SeatStep
{
    Wait,
    Try,
    Seated,
    Unseated,
    GiveUp,
    Release,
};

//! What to do with a copy told to ride. aPairReady: both copies are loaded, alive, in the same place and in seat range.
//! aOnMount: what the game reads back on the copy now.
constexpr SeatStep NextSeatStep(const Seat& acSeat, bool aPairReady, bool aOnMount, double aNow) noexcept
{
    if (aNow - acSeat.ToldAt >= kSeatStaleAfter)
        return SeatStep::Release;

    if (aOnMount)
        return acSeat.Seated ? SeatStep::Wait : SeatStep::Seated;

    // Seated, and now not: thrown, or the game took it off. It may be put back with the tries it has left.
    if (acSeat.Seated)
        return SeatStep::Unseated;

    if (acSeat.GaveUp || !aPairReady)
        return SeatStep::Wait;

    // Each try, the last one too, gets its full interval to take before it is judged.
    if (acSeat.Tries != 0 && aNow - acSeat.TriedAt < kSeatRetryInterval)
        return SeatStep::Wait;

    return acSeat.Tries >= kSeatTries ? SeatStep::GiveUp : SeatStep::Try;
}

//! What a copy's ride was told under: the rider's and the horse's epochs, and the two actors here by handle (0 until first seen).
struct Binding
{
    uint32_t RiderEpoch{};
    uint32_t MountEpoch{};
    uint32_t RiderHandle{};
    uint32_t MountHandle{};
};

enum class BindingStep
{
    Keep,
    //! The rider or the horse changed owner since: nothing is done to the copy until a word at the new epochs (Rebind), or until
    //! the ride goes stale and the copy gets off.
    Hold,
    //! The rider or the horse is another actor here now (unloaded and made again): the ride starts over on the actors there now.
    Restart,
};

constexpr BindingStep CheckBinding(const Binding& acHeld, uint32_t aRiderEpoch, uint32_t aMountEpoch, uint32_t aRiderHandle, uint32_t aMountHandle) noexcept
{
    if (acHeld.RiderEpoch != aRiderEpoch || acHeld.MountEpoch != aMountEpoch)
        return BindingStep::Hold;
    const bool cRiderChanged = acHeld.RiderHandle && aRiderHandle && acHeld.RiderHandle != aRiderHandle;
    const bool cMountChanged = acHeld.MountHandle && aMountHandle && acHeld.MountHandle != aMountHandle;
    return cRiderChanged || cMountChanged ? BindingStep::Restart : BindingStep::Keep;
}

//! A refresh of the same ride at the epochs the copies hold now: a new owner of the rider or the horse carries the ride on.
//! True when that changed the epochs, which withdraws any work decided under the old ones.
constexpr bool Rebind(Binding& aHeld, uint32_t aRiderEpoch, uint32_t aMountEpoch) noexcept
{
    const bool cChanged = aHeld.RiderEpoch != aRiderEpoch || aHeld.MountEpoch != aMountEpoch;
    aHeld.RiderEpoch = aRiderEpoch;
    aHeld.MountEpoch = aMountEpoch;
    return cChanged;
}

//! A ride that starts over: the copy's tries and read-back go, the climb is not shown again.
constexpr void Restart(Seat& aSeat) noexcept
{
    const double cToldAt = aSeat.ToldAt;
    aSeat = Seat{};
    aSeat.ToldAt = cToldAt;
}

//! Whether a copy is close enough to its horse for the game to seat it (aDistanceSquared in game units squared).
constexpr bool InSeatRange(float aDistanceSquared) noexcept
{
    return aDistanceSquared >= 0.f && aDistanceSquared <= kSeatRange * kSeatRange;
}

//! Whether a copy told to ride is shown climbing on: its rider is getting on (the word says so, or the copy just replayed its
//! rider's own climb), and it is not in the saddle or climbing already.
constexpr bool ShowsClimb(bool aWordBoarding, double aSinceOwnerClimbed, bool aSeatedOrTried) noexcept
{
    return !aSeatedOrTried && (aWordBoarding || (aSinceOwnerClimbed >= 0.0 && aSinceOwnerClimbed < kClimbTimeout));
}

//! Whether a try to seat a copy (acSeat.Tries counts it) shows it climbing on: the first one always when its rider is getting on, a
//! later one while that is still fresh. A first try the game refused (its horse not ready here yet) then still climbs on at the next,
//! instead of putting the copy straight into the saddle.
constexpr bool ClimbsOnTry(const Seat& acSeat, double aNow) noexcept
{
    return acSeat.ShowClimb && !acSeat.Seated && (acSeat.Tries <= 1 || (aNow >= acSeat.BoardingAt && aNow - acSeat.BoardingAt < kClimbWindow));
}

//! A horse handed over for this player's claim is got on once no other game's rider copy is left on it here (one still shown climbing
//! off, or seated by a word whose dismount never came). That copy is let off it first: the game seats nobody on a ridden horse.
constexpr bool RideReady(bool aRunHere, bool aCopyLeftOn) noexcept
{
    return aRunHere && !aCopyLeftOn;
}

//! What the server knows when a player asks for another game's horse to ride it.
struct ClaimToRide
{
    bool IsMount{};
    bool Dead{};
    bool IsPlayer{};
    bool HasOwner{};
    bool OwnedBySender{};
    //! The horse's game's player and the asking player are in the same party.
    bool OwnersInSameParty{};
    bool InRange{};
    bool EpochMatches{};
    //! Somebody else's rider sits on it now.
    bool RiddenByOther{};
    //! The server shares party horses (Gameplay:bPartyHorses).
    bool SharingOn{true};
};

//! Party members share horses: one may take a horse another party member's game runs, to ride it, unless someone sits on it.
//! Anybody else's horse stays theirs, as in the game. With sharing off on the server no claim is taken, whatever the games say.
constexpr bool MayClaimToRide(const ClaimToRide& acClaim) noexcept
{
    return acClaim.SharingOn && acClaim.IsMount && !acClaim.Dead && !acClaim.IsPlayer && acClaim.HasOwner && !acClaim.OwnedBySender && acClaim.OwnersInSameParty &&
           acClaim.InRange && acClaim.EpochMatches && !acClaim.RiddenByOther;
}

//! Whether this game treats a horse as its own player's too: party horse sharing is on, and a member of this player's party rode it
//! here. Anybody else's horse, and one only NPCs own, keeps the game's own ownership (taking it is stealing).
constexpr bool SharesHorse(bool aIsMount, bool aRiddenByPartyMember, bool aRiderStillInParty) noexcept
{
    return aIsMount && aRiddenByPartyMember && aRiderStillInParty;
}

//! Work on a horse done on the main thread for party horse sharing (MountService::RunQueuedWork).
enum class HorseJob : uint8_t
{
    //! Counted as this player's too.
    Share,
    //! Its owner here put back: undoes the share in place (Stamp.Work.Undoes).
    Unshare,
    //! This player gets on it, now that this game runs it.
    Ride,
};

//! What horse work was decided under. Work.Ticket is the claim or share it belongs to (0 for an Unshare), Work.Undoes the share an
//! Unshare takes back.
struct HorseStamp
{
    CopyWork::Stamp Work{};
    //! The connection it was decided in: a disconnect ends it.
    uint32_t Session{};
    //! The horse's handle when it was decided, so that a horse loaded again under the same form id is not taken for it.
    uint32_t Handle{};
};

//! What the main thread finds when horse work comes up.
struct HorseFound
{
    bool Exiting{};
    bool Loading{};
    uint32_t LoadingScreens{};
    uint32_t Session{};
    //! Its handle still names an actor with its form id, not deleted.
    bool SameHorse{};
    //! Alive, with its 3D.
    bool Living{};
    bool IsMount{};
    //! This game runs it (not another game's copy).
    bool RunHere{};
    //! The claim or share it belongs to is still live.
    bool Live{};
    //! The share in place on it now, 0 for none.
    uint32_t Shared{};
    uint64_t WaitedMs{};
    //! A Ride: the time its claim waited for the horse is up.
    bool Expired{};
};

//! Share and Ride are dropped by a disconnect, a loading screen, a withdrawn claim or share, or a horse that is not the one decided
//! for (or no horse any more); a Ride also needs it alive and run here, and its claim's time not up. An Unshare waits out a loading screen and puts the owner
//! back only on the same horse while the share it undoes is in place, even after a disconnect.
constexpr CopyWork::Verdict CheckHorse(HorseJob aJob, const HorseStamp& acStamp, const HorseFound& acFound) noexcept
{
    CopyWork::Found found{};
    found.Exiting = acFound.Exiting;
    found.Loading = acFound.Loading;
    found.LoadingScreens = acFound.LoadingScreens;
    found.Live = acFound.Live;
    found.InPlace = acFound.Shared;
    found.WaitedMs = acFound.WaitedMs;

    if (aJob == HorseJob::Unshare)
    {
        if (acStamp.Work.Ticket != 0)
            return CopyWork::Verdict::kDrop;
        found.Actor = acFound.SameHorse;
        return CopyWork::Check(acStamp.Work, found, true);
    }

    const bool cRide = aJob == HorseJob::Ride;
    if (acStamp.Work.Ticket == 0 || (!acFound.Exiting && acStamp.Session != acFound.Session) || (cRide && acFound.Expired))
        return CopyWork::Verdict::kDrop;
    found.Actor = acFound.SameHorse && acFound.IsMount && (!cRide || acFound.Living);
    found.Copy = !cRide || acFound.RunHere;
    return CopyWork::Check(acStamp.Work, found, false);
}

//! A shared horse's owner here before it was shared: put back when it no longer is, and for every save.
struct OwnerBefore
{
    uint32_t ActorOwner{};
    uint32_t FactionOwner{};
    //! The horse it was shared on, by handle, and the share (its ticket).
    uint32_t Handle{};
    uint32_t Share{};
    //! Its Unshare was dropped while a loading screen stayed up: the owner is still to be put back (HorseOwners::Settle).
    bool Owed{};
};

//! What an Unshare comes to for its horse's record.
enum class Undo : uint8_t
{
    //! No record of that share: a later share's stays as it is.
    kNothing,
    //! The owner here goes back on the horse, and the record goes.
    kPutBack,
    //! The horse can't be looked at (a loading screen outlasted the wait): the record stays, owed, and saves still lift the share.
    kOwe,
    //! The horse it was shared on is gone, or its form id names another one: nothing can be put back, and the record goes.
    kForget,
};

//! The owners shared horses had here, by horse form id. Changed on the main thread (MountService::RunQueuedWork) and read by the
//! character save; the caller locks.
class HorseOwners
{
public:
    [[nodiscard]] const OwnerBefore* Find(uint32_t aHorse) const noexcept
    {
        const auto it = m_records.find(aHorse);
        return it != m_records.end() ? &it->second : nullptr;
    }

    //! The share in place on a horse, 0 for none.
    [[nodiscard]] uint32_t Shared(uint32_t aHorse) const noexcept
    {
        const OwnerBefore* pRecord = Find(aHorse);
        return pRecord ? pRecord->Share : 0;
    }

    //! A share made on aHandle carries on the record kept for that horse, and pays off an owed one. False when there is none for it
    //! and the owner here is to be read and recorded: a record left by another horse with that form id goes.
    bool CarryOn(uint32_t aHorse, uint32_t aHandle, uint32_t aShare) noexcept
    {
        const auto it = m_records.find(aHorse);
        if (it == m_records.end())
            return false;
        if (it->second.Handle != aHandle)
        {
            m_records.erase(it);
            return false;
        }
        it.value().Share = aShare;
        it.value().Owed = false;
        return true;
    }

    void Record(uint32_t aHorse, const OwnerBefore& acRecord) noexcept { m_records[aHorse] = acRecord; }

    //! What the Unshare of aUndoes comes to by its verdict. On kPutBack and kForget the record is copied to aRecord and goes.
    Undo Unshare(uint32_t aHorse, uint32_t aUndoes, CopyWork::Verdict aVerdict, bool aLoading, OwnerBefore& aRecord) noexcept
    {
        const auto it = m_records.find(aHorse);
        if (it == m_records.end() || !aUndoes || it->second.Share != aUndoes || aVerdict == CopyWork::Verdict::kHold)
            return Undo::kNothing;
        if (aVerdict == CopyWork::Verdict::kDrop && aLoading)
        {
            it.value().Owed = true;
            return Undo::kOwe;
        }
        aRecord = it->second;
        m_records.erase(it);
        return aVerdict == CopyWork::Verdict::kDo ? Undo::kPutBack : Undo::kForget;
    }

    //! Called with no loading screen up: each owed record is handed to acPutBack(horse, record), which puts the owner back if its
    //! handle still names that horse, and then goes (put back, or its horse gone).
    template <class TPutBack> void Settle(const TPutBack& acPutBack) noexcept
    {
        for (auto it = m_records.begin(); it != m_records.end();)
        {
            if (!it->second.Owed)
            {
                ++it;
                continue;
            }
            acPutBack(it->first, it->second);
            it = m_records.erase(it);
        }
    }

private:
    TiltedPhoques::Map<uint32_t, OwnerBefore> m_records;
};

//! A party member's horse this player is getting on, from the ask until the main thread has got on it or dropped the ride.
struct RideClaim
{
    //! Until when the horse is waited for; it goes with the Ride.
    double Until{};
    //! The claim's ticket, and the loading screens, connection and horse handle when it was asked for.
    HorseStamp Stamp{};
    //! Its Ride is queued: the claim stays until the main thread has done or dropped it (RideClaims::Spent).
    bool Queued{};
};

//! The ride claims, by horse form id, kept by the client's update. A claim goes, and its ticket is withdrawn, when the party or the
//! switch ends it or its time is up, queued or not, and when the main thread has done or dropped its Ride.
class RideClaims
{
public:
    void Ask(uint32_t aHorse, const RideClaim& acClaim) noexcept { m_claims[aHorse] = acClaim; }

    //! The main thread did or dropped the Ride of aTicket. True when that is the claim kept, which goes (its ticket is the caller's to
    //! withdraw); the word of an older claim changes nothing.
    bool Spent(uint32_t aHorse, uint32_t aTicket) noexcept
    {
        const auto it = m_claims.find(aHorse);
        if (it == m_claims.end() || !aTicket || it->second.Stamp.Work.Ticket != aTicket)
            return false;
        m_claims.erase(it);
        return true;
    }

    //! acHandedOver(horse): this game runs the horse now. acQueue(horse, claim) queues its Ride; acRetire(horse) withdraws its ticket.
    template <class THandedOver, class TQueue, class TRetire>
    void Update(double aNow, bool aStillShared, const THandedOver& acHandedOver, const TQueue& acQueue, const TRetire& acRetire) noexcept
    {
        for (auto it = m_claims.begin(); it != m_claims.end();)
        {
            if (!aStillShared || aNow >= it->second.Until)
            {
                acRetire(it->first);
                it = m_claims.erase(it);
                continue;
            }
            if (!it->second.Queued && acHandedOver(it->first))
            {
                it.value().Queued = true;
                acQueue(it->first, it->second);
            }
            ++it;
        }
    }

    //! Every claim goes (a disconnect).
    template <class TRetire> void Clear(const TRetire& acRetire) noexcept
    {
        for (const auto& [cHorse, cClaim] : m_claims)
            acRetire(cHorse);
        m_claims.clear();
    }

    [[nodiscard]] bool Empty() const noexcept { return m_claims.empty(); }

    [[nodiscard]] const RideClaim* Find(uint32_t aHorse) const noexcept
    {
        const auto it = m_claims.find(aHorse);
        return it != m_claims.end() ? &it->second : nullptr;
    }

private:
    TiltedPhoques::Map<uint32_t, RideClaim> m_claims;
};

//! A shared horse's record, as a save sees it: the horse it was made for, by handle, and the share it belongs to.
struct SharedOwner
{
    uint32_t Handle{};
    uint32_t Share{};
};

//! A save lifts the share off a horse only if the record is for this very horse.
constexpr bool GuardLifts(bool aRecorded, const SharedOwner& acRecord, uint32_t aHandle) noexcept
{
    return aRecorded && acRecord.Share && aHandle && acRecord.Handle == aHandle;
}

//! ...and gives it back after the save only if that same share is still in place on it: one taken back meanwhile stays taken back,
//! and one made again meanwhile already set its own owner.
constexpr bool GuardRestores(bool aStillRecorded, const SharedOwner& acNow, const SharedOwner& acLifted, uint32_t aHandle) noexcept
{
    return aStillRecorded && acNow.Share == acLifted.Share && acNow.Handle == acLifted.Handle && aHandle == acLifted.Handle;
}

//! A dismount cancels a word to get on that still waits for its copies when both name the same rider and horse and the word is no
//! newer than the dismount (the rider's epoch it was said under is not later). A word for another horse, or from a later owner of
//! the rider, is a newer ride and stays.
constexpr bool DismountCancels(const Identity& acWaiting, uint32_t aRider, uint32_t aRiderEpoch, uint32_t aMount) noexcept
{
    return aRiderEpoch && acWaiting.Rider == aRider && acWaiting.Mount == aMount && acWaiting.RiderEpoch <= aRiderEpoch;
}

//! What the server keeps of a ride the rider's game said, for players sent the pair later.
struct Ride
{
    uint32_t Mount{};
    uint32_t RiderEpoch{};
    uint32_t MountEpoch{};
    double ToldAt{};
};

//! Whether a player sent the rider or its horse now is told of the ride: both still under the epochs it was said for, and
//! said again lately (a rider whose owner went quiet gets off the copies anyway, see kSeatStaleAfter).
constexpr bool TellLateLoader(const Ride& acRide, uint32_t aRiderEpoch, uint32_t aMountEpoch, double aNow) noexcept
{
    return acRide.RiderEpoch && acRide.RiderEpoch == aRiderEpoch && acRide.MountEpoch == aMountEpoch && aNow >= acRide.ToldAt && aNow - acRide.ToldAt < kSeatStaleAfter;
}

//! A dismount is taken only from the rider's current owner, for its current epoch.
constexpr bool MayDismount(uint32_t aSentEpoch, uint32_t aCurrentEpoch, bool aOwnedBySender) noexcept
{
    return aSentEpoch && aSentEpoch == aCurrentEpoch && aOwnedBySender;
}
} // namespace MountSync
