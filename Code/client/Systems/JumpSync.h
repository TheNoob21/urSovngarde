#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Another player's jump, shown on their copy with the game's own jump animations.
//
// The owner's game performs the jump, its fall and its landing as actions, and every copy replays them. A copy whose graph
// refuses one of those actions is sent the action's graph event itself, as the owner's graph took it. A VR player's body pose
// leaves the legs out while that player is in the air and just after landing, so the copy's legs play the jump, fall and land
// instead of standing straight under the rising body. The owner's landing is its controller's, not an action: a copy still in
// its jump once its owner reads as on the ground is sent the fall it missed and the landing (Landing).
//
// Pure: plain values in and out, tested in tests/jump_sync.cpp.
namespace JumpSync
{
using Clock = std::chrono::steady_clock;

//! The legs come back into the pose this long after the player lands, so the landing animation plays out on the copy.
inline constexpr auto kLandHold = std::chrono::milliseconds(400);

//! The jump, fall and landing graph events (JumpStandingStart, JumpDirectionalStart, JumpFall, JumpLand, JumpLandDirectional...).
[[nodiscard]] constexpr bool IsJumpEvent(const std::string_view acEvent) noexcept
{
    constexpr std::string_view cPrefix = "jump";
    if (acEvent.size() <= cPrefix.size())
        return false;
    for (size_t i = 0; i < cPrefix.size(); ++i)
    {
        const char c = acEvent[i] >= 'A' && acEvent[i] <= 'Z' ? static_cast<char>(acEvent[i] - 'A' + 'a') : acEvent[i];
        if (c != cPrefix[i])
            return false;
    }
    return true;
}

//! Whether a player copy that refused its owner's action is sent the action's graph event instead: a jump, fall or landing,
//! alive and not knocked down here (its own fall belongs to the knock mirror).
[[nodiscard]] constexpr bool SendsRefusedEvent(const bool aPlayerCopy, const bool aTaken, const std::string_view acEvent, const bool aDeadOrDown) noexcept
{
    return aPlayerCopy && !aTaken && !aDeadOrDown && IsJumpEvent(acEvent);
}

//! A jump event's place in the jump: its start, its fall, or its landing.
enum class Phase : uint8_t
{
    Ground,
    Up,
    Falling,
};

//! Lower-case compare of acEvent from aFrom against acPart (lower case).
[[nodiscard]] constexpr bool HasAt(const std::string_view acEvent, const size_t aFrom, const std::string_view acPart) noexcept
{
    if (acEvent.size() < aFrom + acPart.size())
        return false;
    for (size_t i = 0; i < acPart.size(); ++i)
    {
        const char c = acEvent[aFrom + i];
        const char l = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (l != acPart[i])
            return false;
    }
    return true;
}

//! Where a jump event leaves the graph that takes it: JumpLand... on the ground, JumpFall... falling, any other jump event
//! (JumpStandingStart, JumpDirectionalStart...) going up.
[[nodiscard]] constexpr Phase PhaseOf(const std::string_view acEvent) noexcept
{
    if (HasAt(acEvent, 4, "land"))
        return Phase::Ground;
    if (HasAt(acEvent, 4, "fall"))
        return Phase::Falling;
    return Phase::Up;
}

//! The owner has to read as on the ground this long before its copy is landed, so that a landing of the copy's own comes first.
inline constexpr auto kLandGrace = std::chrono::milliseconds(150);
//! A copy whose owner never read as in the air since its jump began (a short hop between two of its owner's updates) is
//! landed this long after its jump began.
inline constexpr auto kNoAirLand = std::chrono::milliseconds(1500);

//! What a copy's graph is sent to land it.
enum class Synth : uint8_t
{
    None,
    //! "JumpFall": its fall was lost, so it is still in its jump's start. The landing follows on a later frame.
    Fall,
    //! "JumpLand".
    Land,
};

//! A player copy's jump against its owner's. The owner's landing comes from its character controller, not an action, so it
//! never reaches the copy as one, and a lost fall left the copy in its jump pose for good. So once the owner reads as on the
//! ground while the copy is still in its jump, the copy is sent the fall it missed and the landing.
class Landing
{
public:
    //! A jump event sent to the copy's graph, taken or not (a refused one changed nothing there).
    void OnCopyEvent(const std::string_view acEvent, const bool aTaken, const Clock::time_point aNow) noexcept
    {
        if (!aTaken || !IsJumpEvent(acEvent))
            return;
        const Phase cPhase = PhaseOf(acEvent);
        if (cPhase != Phase::Ground && m_phase == Phase::Ground)
        {
            m_ownerInAir = false;
            m_since = aNow;
        }
        m_phase = cPhase;
        m_groundSince = {};
    }

    //! The copy is down, seated, mounted or swimming: whatever jump it was in is over.
    void Reset() noexcept { *this = {}; }

    //! What the copy's graph is sent now, from its owner's bInJumpState as shown now.
    [[nodiscard]] Synth Update(const bool aOwnerInJump, const Clock::time_point aNow) noexcept
    {
        if (m_phase == Phase::Ground)
            return Synth::None;
        if (aOwnerInJump)
        {
            m_ownerInAir = true;
            m_groundSince = {};
            return Synth::None;
        }
        if (m_groundSince == Clock::time_point{})
            m_groundSince = aNow;
        if (aNow - m_groundSince < kLandGrace || (!m_ownerInAir && aNow - m_since < kNoAirLand))
            return Synth::None;
        if (m_phase == Phase::Up)
        {
            // The landing goes on the next frame, once the graph is in its fall.
            m_phase = Phase::Falling;
            return Synth::Fall;
        }
        m_phase = Phase::Ground;
        m_groundSince = {};
        return Synth::Land;
    }

    [[nodiscard]] Phase GetPhase() const noexcept { return m_phase; }

private:
    Phase m_phase{Phase::Ground};
    bool m_ownerInAir{};
    Clock::time_point m_since{};
    Clock::time_point m_groundSince{};
};

//! The VR player's legs in the body pose: left out while in the air (bInJumpState) and for kLandHold after landing.
class Legs
{
public:
    [[nodiscard]] bool Sent(const bool aInJump, const Clock::time_point aNow) noexcept
    {
        if (aInJump)
        {
            m_inAir = true;
            return false;
        }
        if (m_inAir)
        {
            m_inAir = false;
            m_landedAt = aNow;
        }
        return m_landedAt == Clock::time_point{} || aNow - m_landedAt >= kLandHold;
    }

private:
    bool m_inAir{};
    Clock::time_point m_landedAt{};
};
} // namespace JumpSync
