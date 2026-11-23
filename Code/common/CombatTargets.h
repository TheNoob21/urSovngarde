#pragma once

#include <cstdint>
#include <vector>

#include <TiltedCore/Stl.hpp>

// The rules for whom an NPC fights across games (CombatService), with no game in them.
// The game that runs an NPC (its owner) is the one truth: it reads whom each of its NPCs fights and tells the other games,
// whose copies of the NPC then look at that target, and are pointed at it when they have a combat controller of their own.
// The presence report says the same every 2 s, which is too slow for a copy that swings: this is sent when it changes.
namespace CombatTargets
{
// How often the owner reads its NPCs, how often it sends every fight again (a word is sent only when it changes),
// how often the server passes on a word that did not change, and how long a copy keeps a word.
constexpr uint64_t kReadEveryMs = 250;
constexpr uint64_t kHeartbeatMs = 2000;
constexpr uint64_t kRelayEveryMs = 1500;
constexpr uint64_t kExpireMs = 5000;

struct Word
{
    uint32_t NpcId{};
    //! Whom it fights, by server id; 0 for nobody.
    uint32_t TargetId{};
    uint32_t OwnershipEpoch{};
};

// The owner's words: those that changed since the last round, and every fight at a heartbeat.
// A fight that ends sends one 0 and is then dropped. An NPC not put in a round (gone, no longer owned) is forgotten without a word.
class Outbox
{
public:
    //! An NPC this game runs, and whom it fights now (0: nobody).
    void Put(const uint32_t aNpcId, const uint32_t aTargetId, const uint32_t aOwnershipEpoch) noexcept { m_round[aNpcId] = {aTargetId, aOwnershipEpoch}; }

    std::vector<Word> Take(const uint64_t aNowMs) noexcept
    {
        const bool cHeartbeat = aNowMs >= m_nextHeartbeat;
        if (cHeartbeat)
            m_nextHeartbeat = aNowMs + kHeartbeatMs;

        std::vector<Word> words;
        TiltedPhoques::Map<uint32_t, Fight> fighting;
        for (const auto& [cNpcId, cFight] : m_round)
        {
            const auto cLast = m_sent.find(cNpcId);
            const bool cWasFighting = cLast != m_sent.end();
            if (!cFight.TargetId)
            {
                if (cWasFighting)
                    words.push_back({cNpcId, 0, cFight.OwnershipEpoch});
                continue;
            }

            // A new epoch is a change too: the server and the copies hold only the new one.
            if (cHeartbeat || !cWasFighting || cLast->second.TargetId != cFight.TargetId || cLast->second.OwnershipEpoch != cFight.OwnershipEpoch)
                words.push_back({cNpcId, cFight.TargetId, cFight.OwnershipEpoch});
            fighting.emplace(cNpcId, cFight);
        }
        m_sent = std::move(fighting);
        m_round = {};
        return words;
    }

private:
    struct Fight
    {
        uint32_t TargetId{};
        uint32_t OwnershipEpoch{};
    };

    TiltedPhoques::Map<uint32_t, Fight> m_round;
    //! Only the NPCs that were fighting when last sent.
    TiltedPhoques::Map<uint32_t, Fight> m_sent;
    uint64_t m_nextHeartbeat{};
};

enum class ServerVerdict : uint8_t
{
    //! Not about an NPC, or about a target that is not a character: dropped.
    kSkip,
    //! Not from the NPC's owner at its current epoch: dropped, and logged as an epoch drop.
    kEpochDrop,
    //! Kept as the NPC's fight, but not passed on now: the same target went less than kRelayEveryMs ago.
    kStore,
    //! Kept and passed on to the other games near the NPC.
    kRelay,
};

// The server takes a word only for an NPC (not a player's character) from its owner at the epoch the server holds,
// about nobody or another character. The epoch test is OwnerComponent::IsCurrentOwner's: 0 is never valid.
// A changed target goes on at once; an unchanged one at most every kRelayEveryMs, which lets the heartbeat through and nothing faster.
inline ServerVerdict ServerTakes(
    const bool aIsCharacter, const bool aIsPlayer, const bool aSenderOwns, const uint32_t aHeldEpoch, const Word& acWord, const bool aTargetIsCharacter, const bool aChanged,
    const uint64_t aSinceRelayedMs) noexcept
{
    if (!aIsCharacter || aIsPlayer)
        return ServerVerdict::kSkip;
    if (!aSenderOwns || acWord.OwnershipEpoch == 0 || acWord.OwnershipEpoch != aHeldEpoch)
        return ServerVerdict::kEpochDrop;
    if (acWord.TargetId && (!aTargetIsCharacter || acWord.TargetId == acWord.NpcId))
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

enum class CopyStep : uint8_t
{
    //! Head and eyes on the target.
    kLookAt,
    //! The same, and the copy's own combat controller pointed at the target.
    kLookAtAndPoint,
    //! The target is not here: the target is kept for the next word, and the copy looks at nothing.
    kKeep,
    //! The target cannot be fought (dead, disabled, not drawn, the copy itself): the word is dropped and the copy looks at nothing.
    kClear,
};

// What a copy does with its owner's target. Never a start or a stop of combat: a copy's controller, when it has one, is only re-pointed.
inline CopyStep CopyAction(const bool aHasController, const bool aTargetHere, const bool aTargetAlive) noexcept
{
    if (!aTargetHere)
        return CopyStep::kKeep;
    if (!aTargetAlive)
        return CopyStep::kClear;
    return aHasController ? CopyStep::kLookAtAndPoint : CopyStep::kLookAt;
}

// Whether the game may start an actor's own fight here. An NPC copy's fights are its owner's: the game starting one for it
// (it saw an enemy) would be a decision of its own, which it then keeps, as its end is processed in the update a copy skips.
// A player's copy is left to the game, as before.
inline bool MayStartOwnCombat(const bool aIsCopy, const bool aIsPlayerCopy) noexcept
{
    return !aIsCopy || aIsPlayerCopy;
}
} // namespace CombatTargets
