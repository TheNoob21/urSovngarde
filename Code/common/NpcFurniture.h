#pragma once

#include <cmath>
#include <cstdint>

#include <TiltedCore/Stl.hpp>
#include <Structs/RuntimeSeat.h>

// The rules for NPCs at work spots across games (FurnitureService), with no game in them.
// The game that runs an NPC (its owner) runs the package that walks it to a chair, a bed, a forge or a counter and sits it
// down. Copies run no AI, so they never sit: the owner reads where each of its NPCs sits and tells the other games, whose
// copies are then put in that furniture by the game's own sit/sleep state change, and taken out of it when the owner's is.
namespace NpcFurniture
{
// How often the owner reads its NPCs, how often it says every seated NPC again (for a game that loads the copy later),
// how often the server passes on a word that did not change, and how long a copy keeps a word.
constexpr uint64_t kReadEveryMs = 250;
constexpr uint64_t kHeartbeatMs = 2000;
constexpr uint64_t kRelayEveryMs = 1500;
constexpr uint64_t kExpireMs = 5000;
// How long a copy whose owner's NPC got up waits for its own replayed get-up before this game stands it.
constexpr uint64_t kStandGraceMs = 2000;

// The game's sit/sleep states (SIT_SLEEP_STATE, bits 14-17 of the actor state's first flags): only the two settled ones are sent.
// Work furniture (a forge, a chopping block, a wall to lean on) has no state of its own: the game's state change settles an actor
// only into these two, and the others (1, 2, 4, 5, 6, 8) are the ways in and out.
constexpr uint8_t kStateNone = 0;
constexpr uint8_t kStateSitting = 3;
constexpr uint8_t kStateSleeping = 7;

//! A state another game is told: sitting or sleeping in furniture, or nothing (the ways in and out are the owner's own).
inline bool IsSettled(const uint8_t aState) noexcept
{
    return aState == kStateSitting || aState == kStateSleeping;
}

//! The furniture, by the plugin that placed it, and the marker in it. Furniture placed at run time is named by its base furniture
//! form and where it stands (Placement).
struct Seat
{
    uint32_t FurnitureModId{};
    uint32_t FurnitureBaseId{};
    uint32_t Marker{};
    uint8_t State{kStateNone};
    RuntimeSeat Placement{};

    bool operator==(const Seat& acRhs) const noexcept
    {
        return FurnitureModId == acRhs.FurnitureModId && FurnitureBaseId == acRhs.FurnitureBaseId && Marker == acRhs.Marker && State == acRhs.State && Placement == acRhs.Placement;
    }
};

struct Word
{
    uint32_t NpcId{};
    Seat Where{};
    uint32_t OwnershipEpoch{};
};

// What the owner reads from one of its NPCs: settled in furniture this game can name to the others, or not seated.
// Furniture placed at run time (a 0xFF form) is named by its base form, with where it stands (WithPlacement).
// A rider can read as seated too: only a reference whose base is furniture is a seat, and a mounted NPC sits in none.
inline Seat ReadSeat(
    const uint8_t aState, const bool aHasFurniture, const bool aIsFurniture, const bool aMounted, const bool aFurnitureNamed, const uint32_t aModId, const uint32_t aBaseId,
    const uint32_t aMarker) noexcept
{
    if (!IsSettled(aState) || !aHasFurniture || !aIsFurniture || aMounted || !aFurnitureNamed)
        return {};
    return {aModId, aBaseId, aMarker, aState};
}

// A seat read from furniture placed at run time: aModId and aBaseId named its base form, and acPlacement says where it stands.
// A seat that is none stays none.
inline Seat WithPlacement(Seat aSeat, const RuntimeSeat& acPlacement) noexcept
{
    if (IsSettled(aSeat.State))
        aSeat.Placement = acPlacement;
    return aSeat;
}

// A furniture placed at run time here is the one another game names (its base form and where it stands) only this close to that
// place: the same script or mod placed it at the same spot in both games.
constexpr float kRuntimeSeatReach = 50.f;

// A furniture reference placed at run time here, of the base form a word names.
struct RuntimeCandidate
{
    uint32_t FormId{};
    float X{};
    float Y{};
    float Z{};
};

// Two candidates whose distances to the named place differ by less than this cannot be told apart: neither is taken.
constexpr float kRuntimeSeatTie = 2.f;

// Whether a candidate stands within kRuntimeSeatReach of the place a word names, a place within the world's range.
inline bool RuntimeSeatFits(const RuntimeCandidate& acCandidate, const RuntimeSeat& acPlacement) noexcept
{
    const float cX = acCandidate.X - static_cast<float>(acPlacement.X);
    const float cY = acCandidate.Y - static_cast<float>(acPlacement.Y);
    const float cZ = acCandidate.Z - static_cast<float>(acPlacement.Z);
    const float cSquared = cX * cX + cY * cY + cZ * cZ;
    return RuntimeSeats::InWorld(acPlacement) && acCandidate.FormId != 0 && cSquared == cSquared && cSquared <= kRuntimeSeatReach * kRuntimeSeatReach;
}

// The candidate nearest the place a word names, within kRuntimeSeatReach; 0 when none is (nothing to sit in here), or when the
// nearest two are within kRuntimeSeatTie of the same distance (two there that cannot be told apart).
inline uint32_t NearestRuntimeSeat(const TiltedPhoques::Vector<RuntimeCandidate>& acCandidates, const RuntimeSeat& acPlacement) noexcept
{
    uint32_t nearest = 0;
    float best = -1.f;
    float second = -1.f;
    for (const RuntimeCandidate& cCandidate : acCandidates)
    {
        if (!RuntimeSeatFits(cCandidate, acPlacement))
            continue;
        const float cX = cCandidate.X - static_cast<float>(acPlacement.X);
        const float cY = cCandidate.Y - static_cast<float>(acPlacement.Y);
        const float cZ = cCandidate.Z - static_cast<float>(acPlacement.Z);
        const float cDistance = std::sqrt(cX * cX + cY * cY + cZ * cZ);
        if (best < 0.f || cDistance < best)
        {
            second = best;
            best = cDistance;
            nearest = cCandidate.FormId;
        }
        else if (second < 0.f || cDistance < second)
            second = cDistance;
    }
    if (second >= 0.f && second - best < kRuntimeSeatTie)
        return 0;
    return nearest;
}

// The owner's words: those that changed since the last round, and every seated NPC at a heartbeat.
// An NPC that gets up sends one "not seated" and is then dropped. An NPC not put in a round (gone, no longer owned) is
// forgotten without a word: its new owner, or the copies' expiry, ends the seat.
class Outbox
{
public:
    //! An NPC this game runs, and where it sits now.
    void Put(const uint32_t aNpcId, const Seat& acSeat, const uint32_t aOwnershipEpoch) noexcept { m_round[aNpcId] = {acSeat, aOwnershipEpoch}; }

    TiltedPhoques::Vector<Word> Take(const uint64_t aNowMs) noexcept
    {
        const bool cHeartbeat = aNowMs >= m_nextHeartbeat;
        if (cHeartbeat)
            m_nextHeartbeat = aNowMs + kHeartbeatMs;

        TiltedPhoques::Vector<Word> words;
        TiltedPhoques::Map<uint32_t, Held> seated;
        for (const auto& [cNpcId, cHeld] : m_round)
        {
            const auto cLast = m_sent.find(cNpcId);
            const bool cWasSeated = cLast != m_sent.end();
            if (!IsSettled(cHeld.Where.State))
            {
                if (cWasSeated)
                    words.push_back({cNpcId, {}, cHeld.OwnershipEpoch});
                continue;
            }

            // A new epoch is a change too: the server and the copies hold only the new one.
            if (cHeartbeat || !cWasSeated || !(cLast->second.Where == cHeld.Where) || cLast->second.OwnershipEpoch != cHeld.OwnershipEpoch)
                words.push_back({cNpcId, cHeld.Where, cHeld.OwnershipEpoch});
            seated.emplace(cNpcId, cHeld);
        }
        m_sent = std::move(seated);
        m_round = {};
        return words;
    }

private:
    struct Held
    {
        Seat Where{};
        uint32_t OwnershipEpoch{};
    };

    TiltedPhoques::Map<uint32_t, Held> m_round;
    //! Only the NPCs that were seated when last sent.
    TiltedPhoques::Map<uint32_t, Held> m_sent;
    uint64_t m_nextHeartbeat{};
};

enum class ServerVerdict : uint8_t
{
    //! Not about an NPC (a player's character, or nothing), or a seat with no furniture: dropped.
    kSkip,
    //! Not from the NPC's owner at its current epoch: dropped, and logged as an epoch drop.
    kEpochDrop,
    //! Kept, but not passed on now: the same seat went less than kRelayEveryMs ago.
    kStore,
    //! Kept and passed on to the other games near the NPC.
    kRelay,
};

// The server takes a word only for an NPC from its owner at the epoch the server holds (0 is never valid), naming furniture
// when it says seated. A changed seat goes on at once; an unchanged one at most every kRelayEveryMs.
inline ServerVerdict ServerTakes(
    const bool aIsCharacter, const bool aIsPlayer, const bool aSenderOwns, const uint32_t aHeldEpoch, const Word& acWord, const bool aChanged,
    const uint64_t aSinceRelayedMs) noexcept
{
    if (!aIsCharacter || aIsPlayer)
        return ServerVerdict::kSkip;
    if (!aSenderOwns || acWord.OwnershipEpoch == 0 || acWord.OwnershipEpoch != aHeldEpoch)
        return ServerVerdict::kEpochDrop;
    if (acWord.Where.State != kStateNone && (!IsSettled(acWord.Where.State) || !acWord.Where.FurnitureBaseId))
        return ServerVerdict::kSkip;
    return aChanged || aSinceRelayedMs >= kRelayEveryMs ? ServerVerdict::kRelay : ServerVerdict::kStore;
}

enum class CopyVerdict : uint8_t
{
    //! A copy of an NPC another game runs, at the epoch the word names.
    kTake,
    //! No character of that id here.
    kUnknown,
    //! This game runs it: its own AI decides.
    kOwn,
    //! The word's epoch is not the copy's.
    kEpochMiss,
};

inline CopyVerdict CopyTakes(const bool aKnown, const bool aIsCopy, const uint32_t aCopyEpoch, const uint32_t aWordEpoch) noexcept
{
    if (!aKnown)
        return CopyVerdict::kUnknown;
    if (!aIsCopy)
        return CopyVerdict::kOwn;
    if (aWordEpoch == 0 || aWordEpoch != aCopyEpoch)
        return CopyVerdict::kEpochMiss;
    return CopyVerdict::kTake;
}

// A copy's word goes when no new one came for kExpireMs: its owner stopped sending, or it is gone.
inline bool CopyExpired(const uint64_t aHeardAtMs, const uint64_t aNowMs) noexcept
{
    return aNowMs - aHeardAtMs > kExpireMs;
}

// The owner's NPC got up at aAskedAtMs (0: it has not); its get-up plays on the copy through the action replay, a little later.
// Only once kStandGraceMs passed with the copy still seated does this game stand it, so a copy does not pop up before its animation.
inline bool StandGraceOver(const uint64_t aAskedAtMs, const uint64_t aNowMs) noexcept
{
    return aAskedAtMs != 0 && aNowMs - aAskedAtMs >= kStandGraceMs;
}

// A seat the game refused (the marker taken here, the copy too far from it) is tried again only every kHeartbeatMs, not every round.
inline bool MaySeatAgain(const uint64_t aTriedAtMs, const uint64_t aNowMs) noexcept
{
    return aTriedAtMs == 0 || aNowMs - aTriedAtMs >= kHeartbeatMs;
}

enum class CopyStep : uint8_t
{
    //! Nothing to do now: already as the owner says, or nothing of ours to undo.
    kKeep,
    //! Seated as the owner says, but the copy or the furniture is not ready here (not loaded), or the owner's NPC got up and the
    //! copy's own get-up has not played yet: tried again next round.
    kWait,
    //! Put the copy in the furniture, settled at once.
    kSeat,
    //! Take the copy out of the furniture this game put it in.
    kStand,
    //! Nothing more to do for this copy: the word is dropped.
    kForget,
};

// What a copy does with its owner's word. aCopyState is the copy's own sit/sleep state here; aSameFurniture says whether
// the furniture it is in is the one the word names; aSeatedByUs whether this game put it there. A copy in some other state
// that this game did not set (a horse, a cart, a scene) is left to whoever set it. aStandGraceOver says whether the copy has had
// kStandGraceMs to get up by its owner's replayed get-up; a word that ran out stands it at once, as no get-up is coming.
inline CopyStep CopyAction(
    const uint8_t aWordState, const bool aExpired, const uint8_t aCopyState, const bool aSameFurniture, const bool aSeatedByUs, const bool aReady,
    const bool aStandGraceOver) noexcept
{
    const bool cWantSeated = !aExpired && IsSettled(aWordState);
    if (!cWantSeated)
    {
        if (!aSeatedByUs || aCopyState == kStateNone)
            return CopyStep::kForget;
        return aExpired || aStandGraceOver ? CopyStep::kStand : CopyStep::kWait;
    }

    if (aCopyState == aWordState && aSameFurniture)
        return CopyStep::kKeep;
    if (aCopyState != kStateNone)
        return aSeatedByUs ? CopyStep::kStand : CopyStep::kWait;
    return aReady ? CopyStep::kSeat : CopyStep::kWait;
}

// Whether the copy is in the seat the word names, for CopyAction's aSameFurniture: the same furniture, at the same marker. A marker the
// game gave in place of the one this game asked for (aAskedForWordMarker: it asked for the word's marker, in the furniture the copy is
// in) counts as the word's, so a copy is not stood and seated again every round while the asked one is taken here.
inline bool SameSeat(const bool aSameFurniture, const uint32_t aHeldMarker, const uint32_t aWordMarker, const bool aAskedForWordMarker, const uint32_t aGivenMarker) noexcept
{
    return aSameFurniture && (aHeldMarker == aWordMarker || (aAskedForWordMarker && aHeldMarker == aGivenMarker));
}

// The sit/sleep state a copy is judged to be in. The action replay writes the owner's first flags over an NPC copy's, so the state
// bits of a copy are not always the seat this game made: they can read seated with no furniture held (nothing here seated it), or read
// standing while the copy still holds the seat this game put it in (aHoldsSeatOfOurs, in the state aSeatedState). The furniture the
// copy holds is what tells them apart.
inline uint8_t CopySeatState(const uint8_t aBitsState, const bool aHoldsFurniture, const bool aHoldsSeatOfOurs, const uint8_t aSeatedState) noexcept
{
    if (!aHoldsFurniture)
        return kStateNone;
    if (aBitsState == kStateNone && aHoldsSeatOfOurs && IsSettled(aSeatedState))
        return aSeatedState;
    return aBitsState;
}

// A word kept from an earlier owner of the NPC (its epoch is not the copy's any more) seats no copy: the new owner's word will. Standing
// a copy this game seated, once the word runs out or says it got up, is still done.
inline CopyStep CopyActionUnder(const bool aCurrentOwner, const CopyStep aStep) noexcept
{
    return !aCurrentOwner && aStep == CopyStep::kSeat ? CopyStep::kWait : aStep;
}

enum class SeatReadBack : uint8_t
{
    //! Settled as asked: that furniture, that marker.
    kAsAsked,
    //! Settled, but not where asked (another marker, or other furniture): this game's to stand again, and not tried again at once.
    kElsewhere,
    //! Not settled as asked.
    kRefused,
};

// What the game reads back from a copy this game just seated.
inline SeatReadBack ReadBack(const uint8_t aAskedState, const uint8_t aReadState, const bool aSameFurniture, const uint32_t aAskedMarker, const uint32_t aReadMarker) noexcept
{
    if (aReadState != aAskedState)
        return SeatReadBack::kRefused;
    return aSameFurniture && aReadMarker == aAskedMarker ? SeatReadBack::kAsAsked : SeatReadBack::kElsewhere;
}

// A seat or stand handed to the main thread that has not come back within this long is given up on, so the copy is looked at again.
constexpr uint64_t kWorkLostMs = 5000;

// Which seat or stand handed to the main thread a copy waits for, and which one a receipt is for.
struct WorkId
{
    //! Handed out by the service (WorkSerials), not counted per copy: a copy's seat state made again starts no count of its own.
    uint32_t Serial{};
    //! The decision a seat was handed out under (CopyWork::Tickets); 0 for a stand.
    uint32_t Ticket{};
    //! The copy it was for, by handle (the game reuses a freed form id).
    uint32_t CopyHandle{};

    bool operator==(const WorkId& acRhs) const noexcept { return Serial == acRhs.Serial && Ticket == acRhs.Ticket && CopyHandle == acRhs.CopyHandle; }
};

// The serials of all the seats and stands one service hands out, never 0.
class WorkSerials
{
public:
    uint32_t Next() noexcept
    {
        if (++m_last == 0)
            ++m_last;
        return m_last;
    }

private:
    uint32_t m_last{};
};

// Whether a receipt from the main thread is for the work the copy waits for (acOut, out since aWorkOutAtMs). One from earlier work,
// given up on or replaced since, or from work for the copy's earlier seat state or another actor, changes nothing.
inline bool ReceiptIsFor(const bool aHasSeat, const uint64_t aWorkOutAtMs, const WorkId& acOut, const WorkId& acReceipt) noexcept
{
    return aHasSeat && aWorkOutAtMs != 0 && acReceipt.Serial != 0 && acOut == acReceipt;
}

// The sit/sleep state's bits in the actor state's first flags (bits 14-17).
constexpr uint32_t kSitSleepShift = 14;
constexpr uint32_t kSitSleepMask = 0xFu << kSitSleepShift;

// The character save writes the sit/sleep state, and AIProcess's save (AE 1.7.104 0x1406F72C0) the middle-high process's furniture
// (+0x208, written as a reference) and marker (+0x2E8). A load seats an actor saved seated, and gives one saved with furniture that
// furniture again, reserved. A copy this game seated is another game's NPC: the save gets it standing in no furniture, as it was
// before it was seated.
inline bool HideSeatInSave(const bool aSeatedHere, const bool aIsCopy, const uint32_t aFlags1) noexcept
{
    return aSeatedHere && aIsCopy && (aFlags1 & kSitSleepMask) != 0;
}

inline uint32_t WithoutSeat(const uint32_t aFlags1) noexcept
{
    return aFlags1 & ~kSitSleepMask;
}

// After the write: the seat put back, whatever else changed in the flags meanwhile.
inline uint32_t WithSeat(const uint32_t aFlags1, const uint32_t aSavedFlags1) noexcept
{
    return (aFlags1 & ~kSitSleepMask) | (aSavedFlags1 & kSitSleepMask);
}

// A copy left where it stood when its owner's NPC is beyond the loaded area here (two saves at different places) is not where its
// owner sits: the owner's way into a seat, replayed on it there, held it sitting mid-road. Within this distance of
// where its owner was last placed, it is at its seat and the replay plays as usual.
constexpr float kAwayFromOwnerUnits = 1000.f;

// Whether an animation's name has aPart in it, in any case. aName may be null.
inline bool NameHas(const char* apName, const char* apPart) noexcept
{
    if (!apName)
        return false;
    for (const char* p = apName; *p; ++p)
    {
        size_t i = 0;
        while (apPart[i] && p[i] && (p[i] | 0x20) == (apPart[i] | 0x20))
            ++i;
        if (!apPart[i])
            return true;
    }
    return false;
}

// Whether an idle's name is one of a seat: a chair, bed, bench, lean, throne or stool idle.
inline bool NamesSeat(const char* apName) noexcept
{
    for (const char* cpSeat : {"chair", "bed", "bench", "sit", "lean", "throne", "stool", "furniture"})
        if (NameHas(apName, cpSeat))
            return true;
    return false;
}

// Whether an owner's replayed action takes its NPC into a seat: the owner's state word says seated, or the idle's name is a way in
// ("IdleChairEnterInstant", "IdleBedEnter...", "IdleLeanTableEnter"...). aName may be null.
inline bool IsSeatReplay(const char* apName, const uint32_t aOwnerFlags1) noexcept
{
    if ((aOwnerFlags1 & kSitSleepMask) != 0)
        return true;
    return NameHas(apName, "enter") && NamesSeat(apName);
}

// Whether an owner's replayed action is its NPC's way out of a seat ("IdleChairExitStart", "IdleBedExitStart"...). aName may be null.
inline bool IsSeatExitReplay(const char* apName) noexcept
{
    return NameHas(apName, "exit") && NamesSeat(apName);
}

// Whether a copy is in the space its owner had its NPC in: outside, the same world space (its cells share one set of coordinates);
// inside, the same cell. Ids are form ids here, 0 for none; an owner's place this game does not have is no space the copy is in.
inline bool SameSpace(const uint32_t aOwnerWorldSpaceId, const uint32_t aOwnerCellId, const uint32_t aCopyWorldSpaceId, const uint32_t aCopyCellId) noexcept
{
    if (aOwnerWorldSpaceId)
        return aOwnerWorldSpaceId == aCopyWorldSpaceId;
    return aOwnerCellId != 0 && aCopyWorldSpaceId == 0 && aOwnerCellId == aCopyCellId;
}

// Whether that replay is left out on a copy: only one left away from its owner's place (aAway), and in another space than that place
// or, in the same space, farther than kAwayFromOwnerUnits from it (two interiors' coordinates say nothing of each other).
// The copy then keeps its own seat state too, so a later action does not write the owner's seat into it.
inline bool SkipSeatReplay(const bool aIsSeatReplay, const bool aAway, const bool aSameSpace, const float aDistanceToOwnerPlace) noexcept
{
    return aIsSeatReplay && aAway && (!aSameSpace || aDistanceToOwnerPlace > kAwayFromOwnerUnits);
}

// What the process holds of an actor in no furniture: the invalid handle, and the marker a never-seated actor's save has.
constexpr uint32_t kNoFurnitureHandle = 0;
constexpr uint32_t kNoFurnitureMarker = 0xFFFFFFFFu;

/**
 * @brief A seated copy written to the save standing in no furniture, for as long as this lives.
 *
 * Its seat, furniture and marker are put back when it goes, however the save is left. The furniture and marker are null when the
 * actor has no process holding them.
 */
class SeatSaveMask
{
public:
    SeatSaveMask() noexcept = default;
    SeatSaveMask(const SeatSaveMask&) = delete;
    SeatSaveMask& operator=(const SeatSaveMask&) = delete;

    ~SeatSaveMask() noexcept { Restore(); }

    void Lift(uint32_t* apFlags1, uint32_t* apFurnitureHandle, uint32_t* apFurnitureMarker, const bool aSeatedHere, const bool aIsCopy) noexcept
    {
        if (m_pFlags1 || !apFlags1 || !HideSeatInSave(aSeatedHere, aIsCopy, *apFlags1))
            return;
        m_pFlags1 = apFlags1;
        m_flags1 = *apFlags1;
        *apFlags1 = WithoutSeat(m_flags1);
        m_pFurnitureHandle = apFurnitureHandle;
        if (apFurnitureHandle)
        {
            m_furnitureHandle = *apFurnitureHandle;
            *apFurnitureHandle = kNoFurnitureHandle;
        }
        m_pFurnitureMarker = apFurnitureMarker;
        if (apFurnitureMarker)
        {
            m_furnitureMarker = *apFurnitureMarker;
            *apFurnitureMarker = kNoFurnitureMarker;
        }
    }

    [[nodiscard]] bool Lifted() const noexcept { return m_pFlags1 != nullptr; }

private:
    void Restore() noexcept
    {
        if (!m_pFlags1)
            return;
        *m_pFlags1 = WithSeat(*m_pFlags1, m_flags1);
        if (m_pFurnitureHandle)
            *m_pFurnitureHandle = m_furnitureHandle;
        if (m_pFurnitureMarker)
            *m_pFurnitureMarker = m_furnitureMarker;
        m_pFlags1 = nullptr;
    }

    uint32_t* m_pFlags1{};
    uint32_t* m_pFurnitureHandle{};
    uint32_t* m_pFurnitureMarker{};
    uint32_t m_flags1{};
    uint32_t m_furnitureHandle{};
    uint32_t m_furnitureMarker{};
};
} // namespace NpcFurniture
