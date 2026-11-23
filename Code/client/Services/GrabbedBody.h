#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include <Services/HandMotion.h>

// A body another player owns that this player picks up with the grab key (Skyrim SE).
// Only a body's owner sends how it moves and bends, so a body grabbed here is asked for, once per grab:
// a VR hand on a body asks once per grab too (VRBodySync::IsRemoteBodyGrabbedHere).
// Asking again while it is held would take it back from a player who grabbed it after us.

//! Whether a body another player owns, moved here by a VR hand, is asked for: a dead one, or a living NPC lying in its ragdoll
//! (knocked down, paralysed), as the grab key asks for in Skyrim SE. Never a living player or a summon of one, and never an NPC
//! kneeling in its bleedout, which gets up where it is.
[[nodiscard]] constexpr bool AsksForGrabbed(const bool aDead, const bool aKnockRagdoll, const bool aBleedingOut, const bool aPlayerOrSummon) noexcept
{
    return aDead || (aKnockRagdoll && !aBleedingOut && !aPlayerOrSummon);
}

// A VR hand carrying a living NPC's ragdoll rather than hitting or pushing it: within kReach of the body and moving with it,
// sample after sample, over kHeldFor ms and at least kSamples samples (VRBodySync samples every 100 ms).
class HandHold
{
public:
    static constexpr float kReach = 40.f;
    static constexpr uint32_t kSamples = 3;
    static constexpr double kHeldFor = 300.0;

    //! A sample at aNow (ms): whether a hand of this player's is within kReach of the body, how each hand moved since the sample
    //! before (none for a hand not read), and how the body moved. Returns whether a hand holds it.
    bool Sample(const double aNow, const bool aHandNear, const std::array<glm::vec3, 2>& acHandSteps, const glm::vec3& acBodyStep) noexcept
    {
        // Each step began at the sample before, so the hold is timed from there.
        const double cStepFrom = m_hasLast ? m_lastAt : aNow;
        m_lastAt = aNow;
        m_hasLast = true;
        const bool cWith = aHandNear && (HandMotion::MovesWith(acHandSteps[0], acBodyStep) || HandMotion::MovesWith(acHandSteps[1], acBodyStep));
        if (!cWith)
        {
            m_samples = 0;
            return false;
        }
        if (m_samples++ == 0)
            m_since = cStepFrom;
        return m_samples >= kSamples && aNow - m_since >= kHeldFor;
    }

private:
    uint32_t m_samples{0};
    double m_since{0.0};
    double m_lastAt{0.0};
    bool m_hasLast{false};
};

class GrabbedBody
{
public:
    // What the game says the player holds now (its form id, 0 for nothing) and whether another player owns it; given once a frame.
    void Update(const uint32_t aHeldFormId, const bool aHeldIsRemote) noexcept
    {
        if (aHeldFormId == m_held)
            return;
        m_held = aHeldFormId;
        // A body already ours when the grab starts needs no asking, and if it is taken from us during the grab, the other player keeps it.
        m_asked = aHeldIsRemote ? 0 : aHeldFormId;
    }

    // Whether that actor is the body the player holds and has not been asked for in this grab.
    [[nodiscard]] bool ShouldAsk(const uint32_t aFormId) const noexcept { return aFormId != 0 && aFormId == m_held && aFormId != m_asked; }

    // The request went out; a request that could not be sent is tried again next frame.
    void Asked(const uint32_t aFormId) noexcept { m_asked = aFormId; }

private:
    uint32_t m_held = 0;
    uint32_t m_asked = 0;
};
