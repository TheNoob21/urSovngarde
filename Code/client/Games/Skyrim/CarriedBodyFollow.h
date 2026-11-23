#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

//! A body another player carries, set into the carrier's pose on flat each frame.
//! Each rigid body is given the speed it moves at, so a body let go carries on in its own ragdoll.
//! Pure: tested in tests/carried_body_follow.cpp.
namespace CarriedBodyFollow
{
using Clock = std::chrono::steady_clock;

//! At most this many bodies are followed at once, the nearest to the player.
inline constexpr size_t kMaxFollowed = 4;
//! A body newly followed eases from where its ragdoll lies into its owner's pose, wholly by this long.
inline constexpr auto kBlendIn = std::chrono::milliseconds(150);
//! A followed body not set for this long is left to its ragdoll.
inline constexpr auto kLapse = std::chrono::milliseconds(250);
//! A ragdoll this far from where its owner has the body is a desync, not a carry.
inline constexpr float kMaxGapUnits = 512.f;
//! The fastest a body is set moving, in units a second.
inline constexpr float kMaxSpeed = 3000.f;
//! A ragdoll's root body farther than this from its 3D root is a bad read: it is put at the owner's root.
inline constexpr float kMaxRootOffset = 128.f;
//! Ragdolls have at most this many bodies here (VRBodySync's CollectRagdoll).
inline constexpr size_t kMaxBodies = 64;
//! A follow asked for longer ago than this is a delayed frame: its pose is stale, and it is not set.
inline constexpr auto kMaxAskAge = std::chrono::milliseconds(100);

//! A follow as the world's update asked for it, or the copy as it is when it is set: when, which reference, and the cells.
struct Asked
{
    Clock::time_point At{};
    uint32_t Handle{};
    uint32_t ActorCell{};
    uint32_t PlayerCell{};
};

//! Whether a follow asked for is still set a frame later: soon enough, on the same reference, with neither it nor the player gone to
//! another cell.
[[nodiscard]] constexpr bool StillAsked(const Asked& acAsked, const Asked& acNow) noexcept
{
    return acAsked.Handle != 0 && acNow.Handle == acAsked.Handle && acNow.ActorCell == acAsked.ActorCell && acNow.PlayerCell == acAsked.PlayerCell && acNow.At >= acAsked.At &&
           acNow.At - acAsked.At <= kMaxAskAge;
}

//! Which rigid bodies a ragdoll has, as one number: a skeleton made again has other bodies, even as many as before.
[[nodiscard]] inline uint64_t SkeletonOf(const void* const* apBodies, const size_t aCount) noexcept
{
    uint64_t key = 14695981039346656037ull;
    for (size_t i = 0; i < aCount; ++i)
    {
        key ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(apBodies[i]));
        key *= 1099511628211ull;
    }
    return key;
}

//! A copy's body as of a frame it may be followed.
struct BodyNow
{
    bool RemoteNpc{}; //!< another game owns it, and it is not a player
    bool Dead{};
    bool KnockRagdoll{}; //!< alive, lying in its ragdoll
    bool Dying{};
    bool BleedingOut{};
    //! The pose shown lies between two of the owner's poses. Held past the newest, the body is not set and keeps its speed.
    bool OwnerBonesMove{};
    bool HeldHere{}; //!< this player holds it with the grab key
    bool Loading{};
};

//! Whether a copy's ragdoll is set into its owner's bones this frame. Never a dying or bleeding-out body.
[[nodiscard]] constexpr bool Follows(const BodyNow& acBody) noexcept
{
    return acBody.RemoteNpc && (acBody.Dead || acBody.KnockRagdoll) && !acBody.Dying && !acBody.BleedingOut && acBody.OwnerBonesMove && !acBody.HeldHere && !acBody.Loading;
}

struct Candidate
{
    uint32_t FormId{};
    float Distance{}; //!< from the player
    bool Followed{};  //!< followed already
};

//! The bodies followed this frame: those followed already first, then the nearest.
[[nodiscard]] inline std::vector<uint32_t> Choose(std::vector<Candidate> aCandidates, const size_t aMax = kMaxFollowed)
{
    std::stable_sort(
        aCandidates.begin(), aCandidates.end(),
        [](const Candidate& acA, const Candidate& acB) { return acA.Followed != acB.Followed ? acA.Followed : acA.Distance < acB.Distance; });
    std::vector<uint32_t> chosen;
    for (const Candidate& candidate : aCandidates)
    {
        if (chosen.size() >= aMax)
            break;
        if (std::find(chosen.begin(), chosen.end(), candidate.FormId) == chosen.end())
            chosen.push_back(candidate.FormId);
    }
    return chosen;
}

//! One followed body: its blend, its ragdoll root's offset from its 3D root, and where each rigid body was set last frame.
class Follow
{
public:
    //! A frame it is set, with this many rigid bodies (aSkeleton: SkeletonOf them): the weight from where they lie to the owner's pose
    //! (0.1..1). Another skeleton gives no speed from the last one's bodies.
    [[nodiscard]] float Begin(const Clock::time_point aNow, const size_t aBodies, const uint64_t aSkeleton) noexcept
    {
        // Taken up again after a lapse, it blends in afresh.
        if (!m_started || Lapsed(aNow))
        {
            m_started = true;
            m_since = aNow;
            m_hasLast = false;
            m_hasOffset = false;
        }
        const auto cStep = aNow - m_lastAt;
        m_stepValid = m_hasLast && aBodies == m_bodies && aSkeleton == m_skeleton && cStep > Clock::duration::zero() && cStep <= kLapse;
        m_stepSeconds = std::chrono::duration<float>(cStep).count();
        m_bodies = std::min(aBodies, kMaxBodies);
        m_skeleton = aSkeleton;
        m_lastAt = aNow;
        m_hasLast = true;
        const float cBlend = std::chrono::duration<float>(aNow - m_since).count() / std::chrono::duration<float>(kBlendIn).count();
        m_weight = std::clamp(cBlend, 0.1f, 1.f);
        return m_weight;
    }

    //! Where its ragdoll root lies from its 3D root, as when the follow began; kept, so that a 3D root following the ragdoll
    //! a frame late does not push the body on. Zero for a bad reading.
    [[nodiscard]] glm::vec3 RootOffset(const glm::vec3& acRagdollRoot, const glm::vec3& acRoot) noexcept
    {
        if (!m_hasOffset)
        {
            m_hasOffset = true;
            const glm::vec3 cOffset = acRagdollRoot - acRoot;
            const float cLength = glm::length(cOffset);
            m_offset = std::isfinite(cLength) && cLength <= kMaxRootOffset ? cOffset : glm::vec3{};
        }
        return m_offset;
    }

    //! Body aIndex set at acAt this frame (game units): the speed to give it, in units a second.
    //! None on its first frame and while blending in: a body let go then drops, it is not flung by the blend.
    [[nodiscard]] glm::vec3 Step(const size_t aIndex, const glm::vec3& acAt) noexcept
    {
        if (aIndex >= kMaxBodies)
            return {};
        glm::vec3 velocity{};
        if (m_stepValid && m_stepSeconds > 0.f && m_weight >= 1.f)
        {
            velocity = (acAt - m_last[aIndex]) / m_stepSeconds;
            const float cSpeed = glm::length(velocity);
            if (!std::isfinite(cSpeed))
                velocity = {};
            else if (cSpeed > kMaxSpeed)
                velocity *= kMaxSpeed / cSpeed;
        }
        m_last[aIndex] = acAt;
        return velocity;
    }

    //! Not set since kLapse: left to its ragdoll.
    [[nodiscard]] bool Lapsed(const Clock::time_point aNow) const noexcept { return !m_hasLast || aNow - m_lastAt > kLapse; }

private:
    bool m_started{};
    bool m_hasLast{};
    bool m_stepValid{};
    bool m_hasOffset{};
    float m_stepSeconds{};
    float m_weight{};
    size_t m_bodies{};
    uint64_t m_skeleton{};
    Clock::time_point m_since{};
    Clock::time_point m_lastAt{};
    glm::vec3 m_offset{};
    std::array<glm::vec3, kMaxBodies> m_last{};
};
} // namespace CarriedBodyFollow
