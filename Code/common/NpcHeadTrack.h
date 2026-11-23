#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <TiltedCore/Stl.hpp>

// The rules for where NPCs look across games (HeadTrackService), with no game in them.
// The game that runs an NPC (its owner) turns its head towards the player walking past, the person it talks to, or the player
// it walks up to greet: its AI writes the look into the NPC's animation graph. Copies run no AI, so their heads stayed still.
// The owner reads those graph values and tells the other games, whose copies get the same values written into their graphs.
// The package that picked the look is never copied, and nothing but the copy's graph is written.
namespace NpcHeadTrack
{
// How often the owner reads its NPCs, how often it says a look again that did not change, how often the server passes on
// an unchanged look, and how long a copy keeps a look with no new word.
constexpr uint64_t kReadEveryMs = 100;
constexpr uint64_t kHeartbeatMs = 1000;
constexpr uint64_t kRelayEveryMs = 900;
constexpr uint64_t kExpireMs = 2500;

// A look point that moved less than this (game units) since it was last sent is not sent again before the heartbeat.
constexpr float kMoveToSend = 4.f;
// The copy's head reaches about two thirds of the way to a new point in this time, so 10 words a second turn it smoothly.
constexpr float kEaseMs = 120.f;
// A point this far from the one shown is a new target, not a move: the copy looks there at once.
constexpr float kSnapDistance = 400.f;

// The graph's own flags: bHeadTracking, and bHeadTrackSpine for a look that turns the upper body too.
constexpr uint8_t kTracking = 1;
constexpr uint8_t kSpine = 2;

struct Point
{
    float X{};
    float Y{};
    float Z{};
};

inline float Distance(const Point& acA, const Point& acB) noexcept
{
    const float cX = acA.X - acB.X;
    const float cY = acA.Y - acB.Y;
    const float cZ = acA.Z - acB.Z;
    return std::sqrt(cX * cX + cY * cY + cZ * cZ);
}

inline bool IsTracking(const uint8_t aFlags) noexcept
{
    return (aFlags & kTracking) != 0;
}

//! What an owner's NPC looks at: the graph's head-track flags and its target point.
struct Look
{
    uint8_t Flags{};
    Point At{};
};

struct Word
{
    uint32_t NpcId{};
    Look What{};
    uint32_t OwnershipEpoch{};
};

// What the owner reads from one of its NPCs. A graph that has no look target (a creature's), or that does not track, is not looking.
inline Look ReadLook(const bool aReadTracking, const bool aTracking, const bool aSpine, const bool aReadPoint, const Point& acPoint) noexcept
{
    if (!aReadTracking || !aTracking || !aReadPoint || !std::isfinite(acPoint.X) || !std::isfinite(acPoint.Y) || !std::isfinite(acPoint.Z))
        return {};
    return {static_cast<uint8_t>(kTracking | (aSpine ? kSpine : 0)), acPoint};
}

// The owner's words: a look that started, changed its flags or moved more than kMoveToSend, every look at a heartbeat, and one
// "not looking" when a look ends. An NPC not put in a round (gone, no longer owned) is forgotten without a word: its new owner,
// or the copies' expiry, ends the look.
class Outbox
{
public:
    //! An NPC this game runs, and what it looks at now.
    void Put(const uint32_t aNpcId, const Look& acLook, const uint32_t aOwnershipEpoch) noexcept { m_round[aNpcId] = {acLook, aOwnershipEpoch}; }

    //! An unfinished round is dropped, e.g. when the connection ended part way through it.
    void Abandon() noexcept { m_round = {}; }

    TiltedPhoques::Vector<Word> Take(const uint64_t aNowMs) noexcept
    {
        const bool cHeartbeat = aNowMs >= m_nextHeartbeat;
        if (cHeartbeat)
            m_nextHeartbeat = aNowMs + kHeartbeatMs;

        TiltedPhoques::Vector<Word> words;
        TiltedPhoques::Map<uint32_t, Held> looking;
        for (const auto& [cNpcId, cHeld] : m_round)
        {
            const auto cLast = m_sent.find(cNpcId);
            const bool cWasLooking = cLast != m_sent.end();
            if (!IsTracking(cHeld.What.Flags))
            {
                if (cWasLooking)
                    words.push_back({cNpcId, {}, cHeld.OwnershipEpoch});
                continue;
            }

            // A point that moved only a little keeps the one last sent, so small moves add up until they are worth a word.
            Held kept = cHeld;
            const bool cChanged = !cWasLooking || cLast->second.What.Flags != cHeld.What.Flags || cLast->second.OwnershipEpoch != cHeld.OwnershipEpoch ||
                                  Distance(cLast->second.What.At, cHeld.What.At) > kMoveToSend;
            if (cHeartbeat || cChanged)
                words.push_back({cNpcId, cHeld.What, cHeld.OwnershipEpoch});
            else
                kept.What.At = cLast->second.What.At;
            looking.emplace(cNpcId, kept);
        }
        m_sent = std::move(looking);
        m_round = {};
        return words;
    }

private:
    struct Held
    {
        Look What{};
        uint32_t OwnershipEpoch{};
    };

    TiltedPhoques::Map<uint32_t, Held> m_round;
    //! Only the NPCs that were looking when last sent, with the look last sent.
    TiltedPhoques::Map<uint32_t, Held> m_sent;
    uint64_t m_nextHeartbeat{};
};

// One pass over this game's NPCs, a few per frame so that a scene full of them is not read in a single frame: it starts every
// kReadEveryMs at the soonest, and its words go out when the last NPC has been read.
class ReadRound
{
public:
    //! How many NPCs one frame reads.
    static constexpr size_t kPerFrame = 24;

    struct Slice
    {
        size_t Begin{};
        size_t End{};
    };

    bool Active() const noexcept { return m_active; }
    bool Due(const uint64_t aNowMs) const noexcept { return !m_active && aNowMs >= m_nextStart; }

    void Start(const uint64_t aNowMs, const size_t aCount) noexcept
    {
        m_active = true;
        m_cursor = 0;
        m_count = aCount;
        m_nextStart = aNowMs + kReadEveryMs;
    }

    //! The NPCs to read this frame, in the order they were put in the round.
    Slice Next() noexcept
    {
        const Slice cSlice{m_cursor, std::min(m_count, m_cursor + kPerFrame)};
        m_cursor = cSlice.End;
        return cSlice;
    }

    bool Finished() const noexcept { return m_active && m_cursor >= m_count; }
    void End() noexcept { m_active = false; }

private:
    bool m_active{false};
    size_t m_cursor{};
    size_t m_count{};
    uint64_t m_nextStart{};
};

enum class ServerVerdict : uint8_t
{
    //! Not about an NPC (a player's character, or nothing), or a look at a point that is not a number: dropped.
    kSkip,
    //! Not from the NPC's owner at its current epoch: dropped, and logged as an epoch drop.
    kEpochDrop,
    //! Kept, but not passed on now: the same look went less than kRelayEveryMs ago.
    kStore,
    //! Kept and passed on to the other games near the NPC.
    kRelay,
};

// The server takes a word only for an NPC from its owner at the epoch the server holds (0 is never valid).
// A changed look goes on at once; an unchanged one at most every kRelayEveryMs.
inline ServerVerdict ServerTakes(
    const bool aIsCharacter, const bool aIsPlayer, const bool aSenderOwns, const uint32_t aHeldEpoch, const Word& acWord, const bool aChanged,
    const uint64_t aSinceRelayedMs) noexcept
{
    if (!aIsCharacter || aIsPlayer)
        return ServerVerdict::kSkip;
    if (!aSenderOwns || acWord.OwnershipEpoch == 0 || acWord.OwnershipEpoch != aHeldEpoch)
        return ServerVerdict::kEpochDrop;
    const Point& cAt = acWord.What.At;
    if (!std::isfinite(cAt.X) || !std::isfinite(cAt.Y) || !std::isfinite(cAt.Z))
        return ServerVerdict::kSkip;
    return aChanged || aSinceRelayedMs >= kRelayEveryMs ? ServerVerdict::kRelay : ServerVerdict::kStore;
}

enum class CopyVerdict : uint8_t
{
    //! A copy of an NPC another game runs, at the epoch the word names.
    kTake,
    //! No character of that id here.
    kUnknown,
    //! This game runs it: its own AI decides where it looks.
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

// A copy's look goes when no new word came for kExpireMs: its owner stopped sending, or it is gone.
inline bool CopyExpired(const uint64_t aHeardAtMs, const uint64_t aNowMs) noexcept
{
    return aNowMs - aHeardAtMs > kExpireMs;
}

enum class CopyStep : uint8_t
{
    //! Write the look into the copy's graph this frame.
    kLook,
    //! Not now (no 3D or graph yet, or the combat look holds it): nothing written, the word is kept.
    kWait,
    //! The look ended, or its word ran out: the copy's graph is told it no longer tracks, then the word goes.
    kStop,
    //! Nothing more to do: the word goes with nothing written (dead, or nothing of ours to undo).
    kForget,
};

// What a copy does with its owner's look each frame. aWritten says whether this game has written a look into its graph since
// the last stop. An NPC this game gained is told once to stop a look this game wrote, so that its owner's look is not left in
// its graph, read back here as its own and sent on; after that its own AI decides where it looks.
// aStale: the word belongs to an owner or a copy that is no longer this one (its epoch moved on, or the copy is another form now).
// Combat owns the graph's look flags while it points the copy at its enemy (aCombatLooks): a look that is over then writes
// nothing, a stop included, and is told to stop once the combat look lets go; one never written is just forgotten.
inline CopyStep CopyAction(
    const bool aIsCopy, const bool aDead, const uint8_t aFlags, const bool aExpired, const bool aReady, const bool aCombatLooks, const bool aWritten,
    const bool aStale = false) noexcept
{
    if (aDead)
        return CopyStep::kForget;
    const bool cOver = !aIsCopy || aExpired || aStale || !IsTracking(aFlags);
    if (aCombatLooks)
        return cOver && !aWritten ? CopyStep::kForget : CopyStep::kWait;
    if (cOver)
        return aWritten && aReady ? CopyStep::kStop : CopyStep::kForget;
    return aReady ? CopyStep::kLook : CopyStep::kWait;
}

// The point the copy's head looks at this frame: eased from the one shown last frame towards its owner's, so that words
// arriving ten times a second still turn the head smoothly. A first look, or a jump to another target, is taken at once.
inline Point Ease(const bool aShowing, const Point& acShown, const Point& acWanted, const float aDeltaMs) noexcept
{
    if (!aShowing || Distance(acShown, acWanted) > kSnapDistance)
        return acWanted;
    if (!(aDeltaMs > 0.f))
        return acShown;
    const float cStep = 1.f - std::exp(-aDeltaMs / kEaseMs);
    return {acShown.X + (acWanted.X - acShown.X) * cStep, acShown.Y + (acWanted.Y - acShown.Y) * cStep, acShown.Z + (acWanted.Z - acShown.Z) * cStep};
}

// A shown look that has hardly moved is not written again every frame, but at least every kRewriteMs, so that a graph that was
// reset (its 3D reloaded) gets the look back. aPendingMs is the time since the last write: the head's step is eased over all of it.
constexpr float kWriteEpsilon = 0.25f;
constexpr float kRewriteMs = 250.f;

inline bool NeedsWrite(const bool aShowing, const uint8_t aShownFlags, const uint8_t aFlags, const Point& acShown, const Point& acNext, const float aPendingMs) noexcept
{
    return !aShowing || aShownFlags != aFlags || aPendingMs >= kRewriteMs || Distance(acShown, acNext) >= kWriteEpsilon;
}
} // namespace NpcHeadTrack
