#pragma once

#include <cstdint>
#include <vector>

#include <TiltedCore/Stl.hpp>

// The rules for whether an NPC sees a player across games (DetectionService), with no game in them.
// The game that runs an NPC (its owner) is the one truth: it reads its NPC's detection of each other player's copy and tells
// that player's game, whose copy of the NPC runs no AI and so sees no one. That game's own detection query then answers with
// the owner's word for that NPC and its player: the crime alarm's witnesses and pickpocketing ask it.
namespace DetectionLevels
{
enum class Band : uint8_t
{
    kUnseen = 0,
    kNoticed = 1,
    kSeen = 2,
};
constexpr uint8_t kBandCount = 3;

// The game's detection level: above 0 the actor is detected, as Papyrus IsDetectedBy has it.
// Noticed is the last stretch below that, the stealth eye's caution. A guess, not the game's number.
constexpr int32_t kNoticedFrom = -20;

inline Band BandOf(const int32_t aLevel) noexcept
{
    if (aLevel > 0)
        return Band::kSeen;
    return aLevel > kNoticedFrom ? Band::kNoticed : Band::kUnseen;
}

// The level this game's detection query gives for a band: within it, and on the same side of detected.
// Noticed (0) is not detected: no witness for a theft. If the sneak attack bonus asks the same query with the same line,
// a copy with no fresh word also takes the full sneak attack bonus, as copies do today; not proven either way.
inline int32_t LevelOf(const Band aBand) noexcept
{
    switch (aBand)
    {
    case Band::kSeen: return 100;
    case Band::kNoticed: return 0;
    default: return -100;
    }
}

// How often the owner reads its NPCs, how often it sends every word again (a word is sent only when it changes),
// how long the other game keeps a word, and how near an NPC must be to another player's copy to be read at all.
constexpr uint64_t kReadEveryMs = 250;
constexpr uint64_t kHeartbeatMs = 2000;
constexpr uint64_t kExpireMs = 5000;
constexpr float kRange = 4096.f;
// At most this many NPC and player pairs are read in a round, the rest kept as they were, in turn: the game's query may
// trace lines of sight, and a town's cost is not measured yet.
constexpr size_t kPairsPerRound = 64;

struct Word
{
    uint32_t NpcId{};
    uint32_t PlayerId{};
    Band Value{};
};

// The owner's words: those that changed since the last round, and all of them at a heartbeat.
// A pair not put in a round (out of range, gone, no longer owned) is forgotten, and is new again when it comes back.
class Outbox
{
public:
    void Put(const uint32_t aNpcId, const uint32_t aPlayerId, const Band aBand) noexcept { m_round[Key(aNpcId, aPlayerId)] = aBand; }

    // A pair still near but not read this round: its last word stands, and goes again at a heartbeat.
    void Keep(const uint32_t aNpcId, const uint32_t aPlayerId) noexcept
    {
        const uint64_t cKey = Key(aNpcId, aPlayerId);
        if (const auto cIt = m_sent.find(cKey); cIt != m_sent.end())
            m_round.emplace(cKey, cIt->second);
    }

    std::vector<Word> Take(const uint64_t aNowMs) noexcept
    {
        const bool cHeartbeat = aNowMs >= m_nextHeartbeat;
        if (cHeartbeat)
            m_nextHeartbeat = aNowMs + kHeartbeatMs;

        std::vector<Word> words;
        for (const auto& [cKey, cBand] : m_round)
        {
            const auto cLast = m_sent.find(cKey);
            if (cHeartbeat || cLast == m_sent.end() || cLast->second != cBand)
                words.push_back({static_cast<uint32_t>(cKey >> 32), static_cast<uint32_t>(cKey), cBand});
        }
        m_sent = std::move(m_round);
        m_round = {};
        return words;
    }

private:
    static uint64_t Key(const uint32_t aNpcId, const uint32_t aPlayerId) noexcept { return static_cast<uint64_t>(aNpcId) << 32 | aPlayerId; }

    TiltedPhoques::Map<uint64_t, Band> m_round;
    TiltedPhoques::Map<uint64_t, Band> m_sent;
    uint64_t m_nextHeartbeat{};
};

// The owner's last word about an NPC copy and this game's player, and when it came.
struct Heard
{
    Band Value{};
    uint64_t AtMs{};
};

// This game's answer for an NPC copy: the owner's word while it is fresh. A word never heard, or older than kExpireMs, is
// "noticed": just short of detected, so the crime alarm does not count the copy as a witness.
// Whether the sneak attack bonus and the stealth eye read this answer is not proven yet (no named caller on 1.7.104).
inline Band Answer(const Heard* apHeard, const uint64_t aNowMs) noexcept
{
    if (!apHeard || aNowMs - apHeard->AtMs > kExpireMs)
        return Band::kNoticed;
    return apHeard->Value;
}

// The server passes a word on only for an NPC the sender runs, about a player in the sender's party, near that NPC.
inline bool ServerTakes(const bool aIsCharacter, const bool aIsPlayer, const bool aSenderOwns, const bool aSeenInSendersParty, const bool aSeenInRange) noexcept
{
    return aIsCharacter && !aIsPlayer && aSenderOwns && aSeenInSendersParty && aSeenInRange;
}
} // namespace DetectionLevels
