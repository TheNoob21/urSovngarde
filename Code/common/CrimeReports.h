#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

// The rules for a crime in one game reaching the games that run its witnesses (CrimeService), with no game in them.
// The offender's game reports its own player's crime when NPC copies there saw it (DetectionService said "seen"). Each game
// that runs one of those NPCs lets its civilian witnesses react to the offender's copy. A bounty stays the offender's own,
// in the offender's game; the receiving game's player is never substituted as the offender.
namespace CrimeReports
{
// The game's own crime numbers.
enum class Kind : uint8_t
{
    kTheft = 0,
    kPickpocket = 1,
    kTrespass = 2,
    kAssault = 3,
    kMurder = 4,
};

// Every kind but trespass, which no game function here reports.
inline bool Taken(const Kind aKind) noexcept
{
    return aKind == Kind::kTheft || aKind == Kind::kPickpocket || aKind == Kind::kAssault || aKind == Kind::kMurder;
}

inline bool Taken(const uint8_t aKind) noexcept
{
    return aKind <= static_cast<uint8_t>(Kind::kMurder) && Taken(static_cast<Kind>(aKind));
}

// The theft alarm rejects an ordinary non-player before checking witnesses (AE 37422, +0x681E46..+0x681E89 on 1.7.104), so it cannot
// be replayed for a copy. A civilian who sees a theft in the base game calls it out (the "Steal" dialogue, Skyrim.esm 0002A45B) and does not fight,
// and that call-out is what a witness here does. The pickpocket alarm also requires the local player; its witnesses fight or flee.
// An assault uses its own alarm with the copy as attacker. A murder uses that assault alarm because its own alarm is player-only.
enum class Reaction : uint8_t
{
    kNone,
    kReplayAssault,
    kStartCombat,
    kCallOut,
};

inline Reaction ReactionOf(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::kTheft: return Reaction::kCallOut;
    case Kind::kPickpocket: return Reaction::kStartCombat;
    case Kind::kAssault:
    case Kind::kMurder: return Reaction::kReplayAssault;
    default: return Reaction::kNone;
    }
}

// A copy cannot be arrested here. Civilians still fight or flee; guards leave its crime to the offender's own game.
inline bool WitnessMayFight(const bool aIsGuard) noexcept
{
    return !aIsGuard;
}

// A guard leaves a copy's theft to the offender's own game too: its call-out is not spoken here.
inline bool WitnessMayCallOut(const bool aIsGuard) noexcept
{
    return !aIsGuard;
}

enum class GuardCombat : uint8_t
{
    kKeep,
    kStop,
    kRestoreTarget,
};

// For a guard not targeting the copy before the alarm: undo only a new copy target, preserving any prior fight.
inline GuardCombat GuardCombatAfterCrime(const bool aWasInCombat, const bool aHadTarget, const bool aTargetsOffender) noexcept
{
    if (!aTargetsOffender)
        return GuardCombat::kKeep;
    return !aWasInCombat && !aHadTarget ? GuardCombat::kStop : GuardCombat::kRestoreTarget;
}

// A guard of another game who saw this game's player's crime is asked for: run here,
// it comes to arrest the player as in the base game, for the bounty this game holds, and the other game sees its approach.
inline bool OffenderAsksForWitness(const bool aIsGuard, const bool aAlive) noexcept
{
    return aIsGuard && aAlive;
}

// How long after a report the server hands a witness it named to the offender's game.
constexpr auto kWitnessClaimFor = std::chrono::seconds(10);

// What the server knows of a claim for a witness of the claimant's crime.
struct WitnessClaim
{
    bool On{};            // Gameplay:bCrimeGuardHandoff
    bool ForCrime{};      // the claim is made for the crime (RequestOwnershipClaim::ForCrime); an ordinary claim never is
    bool NamedBySender{}; // a report of the claimant's named it
    bool EpochAsNamed{};  // the claim's epoch is the witness's when that report named it
    std::chrono::steady_clock::duration SinceNamed{};
    bool Alive{};
};

// The server gives a living witness to the player whose report named it, lately, when that player's game asks for it for the crime.
// Every other claim rule still holds (CanClaimOwnership).
inline bool ServerGivesWitness(const WitnessClaim& acClaim) noexcept
{
    return acClaim.On && acClaim.ForCrime && acClaim.NamedBySender && acClaim.EpochAsNamed && acClaim.SinceNamed >= std::chrono::steady_clock::duration::zero() &&
           acClaim.SinceNamed < kWitnessClaimFor && acClaim.Alive;
}

// A claim for a crime's witness (RequestOwnershipClaim::ForCrime) is answered by the witness rule alone, before the ride, hold,
// killing-blow and follower rules, whatever else it says: Gameplay:bCrimeGuardHandoff governs every one.
template <class TClaim> constexpr bool ByWitnessRuleOnly(const TClaim& acClaim) noexcept
{
    return acClaim.ForCrime;
}

// The witnesses crime reports named lately, by witness and reporter: one player's report never replaces another's.
class WitnessBook
{
public:
    struct Entry
    {
        uint32_t Witness{};
        uint32_t Reporter{};
        uint32_t Epoch{};
        std::chrono::steady_clock::time_point At{};
    };

    static constexpr size_t kMaxNoted = 1024;

    // A report of aReporter's named aWitness, whose ownership epoch was aEpoch.
    void Note(const uint32_t aWitness, const uint32_t aReporter, const uint32_t aEpoch, const std::chrono::steady_clock::time_point aNow) noexcept
    {
        std::erase_if(m_entries, [aNow](const Entry& acEntry) { return aNow - acEntry.At >= kWitnessClaimFor; });
        Take(aWitness, aReporter);
        if (m_entries.size() == kMaxNoted)
            m_entries.erase(m_entries.begin());
        m_entries.push_back({aWitness, aReporter, aEpoch, aNow});
    }

    [[nodiscard]] const Entry* Find(const uint32_t aWitness, const uint32_t aReporter) const noexcept
    {
        for (const Entry& cEntry : m_entries)
            if (cEntry.Witness == aWitness && cEntry.Reporter == aReporter)
                return &cEntry;
        return nullptr;
    }

    // A claim from aClaimant on aWitness at aEpoch, as this book has it.
    [[nodiscard]] WitnessClaim ClaimOf(
        const bool aOn, const bool aForCrime, const uint32_t aWitness, const uint32_t aClaimant, const uint32_t aEpoch, const bool aAlive,
        const std::chrono::steady_clock::time_point aNow) const noexcept
    {
        const Entry* pEntry = Find(aWitness, aClaimant);
        WitnessClaim claim{};
        claim.On = aOn;
        claim.ForCrime = aForCrime;
        claim.NamedBySender = pEntry != nullptr;
        claim.EpochAsNamed = pEntry && pEntry->Epoch == aEpoch;
        claim.SinceNamed = pEntry ? aNow - pEntry->At : std::chrono::steady_clock::duration{};
        claim.Alive = aAlive;
        return claim;
    }

    // aReporter's claim on aWitness was answered: other reporters keep theirs.
    void Take(const uint32_t aWitness, const uint32_t aReporter) noexcept
    {
        std::erase_if(m_entries, [aWitness, aReporter](const Entry& acEntry) { return acEntry.Witness == aWitness && acEntry.Reporter == aReporter; });
    }

    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
};

// A real blow against a guard still gets the game's normal combat response.
inline bool GuardWasAttacked(const Kind aKind, const bool aVictimIsGuard) noexcept
{
    return aVictimIsGuard && (aKind == Kind::kAssault || aKind == Kind::kMurder);
}

// Only this game's player's own crime is reported, and never one this game is replaying for another player's copy.
inline bool ShouldReport(const bool aOffenderIsLocalPlayer, const bool aReplaying) noexcept
{
    return aOffenderIsLocalPlayer && !aReplaying;
}

constexpr size_t kMaxWitnesses = 16;

// The NPC copies that saw this game's player during one crime alarm, collected by the detection query as the alarm asks it.
// A fixed array: the alarm runs on whichever thread the game chose, and nothing here allocates.
class WitnessCollector
{
public:
    void Start() noexcept
    {
        m_on = true;
        m_count = 0;
    }

    void Stop() noexcept { m_on = false; }

    [[nodiscard]] bool On() const noexcept { return m_on; }

    void Add(const uint32_t aFormId) noexcept
    {
        if (!m_on || !aFormId || m_count == kMaxWitnesses)
            return;
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_ids[i] == aFormId)
                return;
        }
        m_ids[m_count++] = aFormId;
    }

    [[nodiscard]] size_t Count() const noexcept { return m_count; }
    [[nodiscard]] const uint32_t* Ids() const noexcept { return m_ids.data(); }

private:
    std::array<uint32_t, kMaxWitnesses> m_ids{};
    size_t m_count{};
    bool m_on{};
};

// A report goes to the owner of each witness: an NPC, run by another member of the offender's party, near the offender.
inline bool
ServerTakesWitness(const bool aIsCharacter, const bool aIsPlayer, const bool aHasOwner, const bool aOwnerIsSender, const bool aOwnerInSendersParty, const bool aInRange) noexcept
{
    return aIsCharacter && !aIsPlayer && aHasOwner && !aOwnerIsSender && aOwnerInSendersParty && aInRange;
}

// The game that runs a witness reacts only to another player's copy that is no teammate here, that one of its own NPCs still sees
// now, and to a crime it knows enough of: for a theft, the item and the owner (pass true for the other kinds).
inline bool ReceiverRuns(
    const bool aKindTaken, const bool aOffenderFound, const bool aOffenderIsLocalPlayer, const bool aOffenderIsTeammate, const bool aWitnessSees, const bool aObjectFound,
    const bool aOwnerFound) noexcept
{
    return aKindTaken && aOffenderFound && !aOffenderIsLocalPlayer && !aOffenderIsTeammate && aWitnessSees && aObjectFound && aOwnerFound;
}

// Reports one player may send in a second: a thief emptying a shelf sends one for each item, a script in a loop no more than this.
constexpr uint32_t kPerSecond = 8;

class RateCount
{
public:
    bool Take(const std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (aNow - m_since >= std::chrono::seconds(1))
        {
            m_since = aNow;
            m_count = 0;
        }
        if (m_count >= kPerSecond)
            return false;
        ++m_count;
        return true;
    }

private:
    std::chrono::steady_clock::time_point m_since{};
    uint32_t m_count{};
};
} // namespace CrimeReports
