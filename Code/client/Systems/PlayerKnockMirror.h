#pragma once

#include <chrono>
#include <cstdint>

// Another player's copy knocked down as its owner is.
//
// A copy used to be stood up at once whenever it was knocked here, so neither player saw the other fall (a giant, a shout,
// a PLANCK ragdoll). Now the owner's own fall, read from the owner's actions, knocks the copy down the game's way, keeps it
// down while the owner lies, and lets the game's own get-up run once the owner gets up. A knock a hit here leaves on a copy
// whose owner stands is still taken off at once: only the owner's game decides whether that player goes down.
// On VR a player copy is knocked by its owner's replayed "Ragdoll" (AdmitRagdoll). An owner's fall told only by its knock bits
// sends the copy's graph its own "Ragdoll" instead, once per fall, after a short wait for the event (TakeGraphKnock).
//
// Pure: plain values in and out, tested in tests/player_knock_mirror.cpp.
namespace PlayerKnockMirror
{
using Clock = std::chrono::steady_clock;

//! KNOCK_STATE_ENUM, bits 25-27 of flags1: 0 normal, 1-4 thrown and falling, 5 at rest with the get-up queued, 6 getting up.
constexpr uint32_t KnockState(const uint32_t aFlags1) noexcept
{
    return (aFlags1 >> 25) & 0x7;
}

//! ACTOR_LIFE_STATE, bits 21-24 of flags1: 0 alive. Dying and dead belong to death sync, and a player downed in a bleedout
//! (3, 7, 8) to the revive's own sync, not to a fall.
constexpr uint32_t LifeState(const uint32_t aFlags1) noexcept
{
    return (aFlags1 >> 21) & 0xF;
}

//! The game's get-up waits in the queue once the ragdoll rests.
inline constexpr uint32_t kKnockQueued = 5;
inline constexpr uint32_t kKnockGettingUp = 6;

//! What one of the owner's actions says of the owner's fall.
enum class Word : uint8_t
{
    None,
    //! "Ragdoll", or a knock state of the owner's while alive: the owner is down.
    Down,
    //! "GetUpBegin", or the owner's knock state getting up.
    GetUp,
    //! Alive with no knock state: the owner stands, unless this came with the fall itself.
    Stands,
};

[[nodiscard]] constexpr Word WordOf(const bool aRagdollEvent, const bool aGetUpEvent, const uint32_t aOwnerFlags1) noexcept
{
    if (LifeState(aOwnerFlags1) != 0)
        return Word::None;
    if (aGetUpEvent)
        return Word::GetUp;
    if (aRagdollEvent)
        return Word::Down;
    const uint32_t cKnock = KnockState(aOwnerFlags1);
    if (cKnock == kKnockGettingUp)
        return Word::GetUp;
    if (cKnock != 0)
        return Word::Down;
    return Word::Stands;
}

//! Whether the copy may be knocked down here: alive and not dying or bleeding out here (the flat frame is not reached then), on its
//! feet and not pinned to a seat or a horse.
[[nodiscard]] constexpr bool MayFall(const bool aDeadHere, const bool aDyingOrDownHere, const bool aOnFoot, const bool aPinned) noexcept
{
    return !aDeadHere && !aDyingOrDownHere && aOnFoot && !aPinned;
}

//! What a frame does with the copy.
enum class Act : uint8_t
{
    //! Nothing about a fall: it follows its owner.
    None,
    //! Knocked here while its owner stands: the knock is taken off at once (the old rule).
    StandNow,
    //! Its owner is down and it is not yet: knock it down the game's way.
    Knock,
    //! It lies (or falls) while its owner lies: it stays where it is, and its queued get-up waits.
    Hold,
    //! Its owner is up: its own get-up runs.
    LetUp,
    //! Its get-up has waited queued too long while its owner is up: its graph is sent the get-up.
    NudgeGetUp,
    //! It is on its feet again after a mirrored fall: it walks back to where its owner has it.
    Risen,
};

//! A word that the owner stands counts this long after the fall at the earliest: the fall's own actions can still read standing.
inline constexpr auto kMinDown = std::chrono::milliseconds(600);
//! The longest a copy is held down with no word that its owner got up, in case that word was lost.
inline constexpr auto kMaxDown = std::chrono::seconds(20);
//! How long its get-up may wait queued before its graph is sent one, how many times, and the longest a get-up may take
//! before it is stood up at once.
inline constexpr auto kNudgeAfter = std::chrono::milliseconds(1500);
inline constexpr uint32_t kMaxNudges = 3;
inline constexpr auto kMaxGetUp = std::chrono::seconds(8);
//! Knocks tried on a copy that did not go down, and the time between them.
inline constexpr uint32_t kMaxKnocks = 2;
inline constexpr auto kKnockRetry = std::chrono::milliseconds(1000);
//! VR: how long a fall told only by knock bits waits for its owner's own "Ragdoll" before the copy's graph is sent one.
inline constexpr auto kGraphKnockWait = std::chrono::milliseconds(300);

//! One player copy's mirrored fall.
class Mirror
{
public:
    enum class Phase : uint8_t
    {
        Standing,
        Down,
        GettingUp,
    };

    //! One of the owner's actions, as it plays here (in order, not from the history a copy is spawned with).
    void OnOwnerWord(const Word aWord, const Clock::time_point aNow) noexcept
    {
        m_ownerAlive = aWord != Word::None;
        switch (aWord)
        {
        case Word::Down:
            if (m_phase != Phase::Down)
            {
                m_phase = Phase::Down;
                m_since = aNow;
                m_knocks = 0;
                m_lastKnock = {};
            }
            break;
        case Word::GetUp: GetUp(aNow); break;
        case Word::Stands:
            if (m_phase == Phase::Down && aNow - m_since >= kMinDown)
                GetUp(aNow);
            break;
        case Word::None: break;
        }
    }

    //! Each frame, with the copy's own knock state here and whether it may fall now (MayFall).
    //! aFrameKnocks: this game knocks the copy from here (flat). VR has no safe knock of its own for a player copy, so there
    //! a frame never asks for a Knock: the owner's replayed "Ragdoll" knocks it (AdmitRagdoll), or its graph's own (TakeGraphKnock).
    [[nodiscard]] Act Frame(const uint32_t aKnockHere, const bool aMayFall, const Clock::time_point aNow, const bool aFrameKnocks = true) noexcept
    {
        switch (m_phase)
        {
        case Phase::Standing: return aKnockHere != 0 ? Act::StandNow : Act::None;
        case Phase::Down:
            if (aNow - m_since > kMaxDown)
            {
                GetUp(aNow);
                return Frame(aKnockHere, aMayFall, aNow, aFrameKnocks);
            }
            if (aKnockHere != 0)
            {
                m_wentDown = true;
                return Act::Hold;
            }
            return aFrameKnocks && TakeKnock(aKnockHere, aMayFall, aNow) ? Act::Knock : Act::None;
        case Phase::GettingUp:
            if (aKnockHere == 0)
            {
                m_phase = Phase::Standing;
                const bool cWasDown = m_wentDown;
                m_wentDown = false;
                return cWasDown ? Act::Risen : Act::None;
            }
            if (aNow - m_upSince > kMaxGetUp)
            {
                m_phase = Phase::Standing;
                m_wentDown = false;
                return Act::StandNow;
            }
            if (aKnockHere != kKnockQueued)
                m_queuedSince = {};
            else if (m_queuedSince == Clock::time_point{})
                m_queuedSince = aNow;
            else if (m_nudges < kMaxNudges && aNow - m_queuedSince > kNudgeAfter)
            {
                ++m_nudges;
                m_queuedSince = {};
                return Act::NudgeGetUp;
            }
            return Act::LetUp;
        }
        return Act::None;
    }

    //! Whether the copy's own process runs this frame (the game's fall and get-up run there, its AI does not).
    //! Held while its owner lies once its ragdoll rests, so that its get-up waits for its owner's.
    [[nodiscard]] bool RunsProcess(const uint32_t aKnockHere) const noexcept
    {
        if (aKnockHere == 0)
            return false;
        if (m_phase == Phase::Down)
            return aKnockHere != kKnockQueued;
        return m_phase == Phase::GettingUp;
    }

    //! VR: whether the owner's replayed "Ragdoll" plays on its copy. It is the same decision as a flat frame's Knock, and uses up
    //! one of the same tries: the owner's live word has put this mirror down (an alive owner, not the history a copy is spawned
    //! with), the copy may fall (MayFall) and is not knocked already.
    //! aWord: what this "Ragdoll" itself says (WordOf). A dying or downed owner's Ragdoll says nothing and leaves an earlier fall's
    //! Down standing, so only one that says Down plays.
    [[nodiscard]] bool AdmitRagdoll(const Word aWord, const bool aHistory, const uint32_t aKnockHere, const bool aMayFall, const Clock::time_point aNow) noexcept
    {
        return !aHistory && aWord == Word::Down && TakeKnock(aKnockHere, aMayFall, aNow);
    }

    //! VR: whether the copy's graph is sent its own "Ragdoll" for a fall its owner told only by knock bits. The same decision as a
    //! flat frame's Knock, and one of the same tries, but only the first of the fall, once its owner's "Ragdoll" had kGraphKnockWait
    //! to come, and only while the owner's latest word is an alive one.
    [[nodiscard]] bool TakeGraphKnock(const uint32_t aKnockHere, const bool aMayFall, const Clock::time_point aNow) noexcept
    {
        return m_phase == Phase::Down && m_ownerAlive && m_knocks == 0 && aNow - m_since >= kGraphKnockWait && TakeKnock(aKnockHere, aMayFall, aNow);
    }

    [[nodiscard]] Phase GetPhase() const noexcept { return m_phase; }

private:
    //! A knock is due: the owner is down, the copy is not, may fall, and has a try left.
    bool TakeKnock(const uint32_t aKnockHere, const bool aMayFall, const Clock::time_point aNow) noexcept
    {
        if (m_phase != Phase::Down || aKnockHere != 0 || !aMayFall || m_knocks >= kMaxKnocks || (m_knocks != 0 && aNow - m_lastKnock < kKnockRetry))
            return false;
        ++m_knocks;
        m_lastKnock = aNow;
        m_wentDown = true;
        return true;
    }

    void GetUp(const Clock::time_point aNow) noexcept
    {
        if (m_phase != Phase::Down)
            return;
        m_phase = Phase::GettingUp;
        m_upSince = aNow;
        m_queuedSince = {};
        m_nudges = 0;
    }

    Phase m_phase{Phase::Standing};
    Clock::time_point m_since{};
    Clock::time_point m_upSince{};
    Clock::time_point m_queuedSince{};
    Clock::time_point m_lastKnock{};
    uint32_t m_knocks{};
    uint32_t m_nudges{};
    //! It was knocked down for this fall (or found knocked), so its rising is a mirrored one.
    bool m_wentDown{};
    //! The owner's latest word came while it was alive (not dying, dead or downed for a revive).
    bool m_ownerAlive{};
};
} // namespace PlayerKnockMirror
