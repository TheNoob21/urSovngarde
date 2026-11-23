#pragma once

#include <Games/Skyrim/CarriedBodyFollow.h>
#include <Structs/RagdollPose.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

//! A ragdoll the humanoid pose cannot bend (a wolf, a chicken, a spider), sent body by body while it is moved (RagdollPoseService).
//! Each rigid body goes where its owner's game has it. They are in the order CollectRagdoll finds them from the 3D root: the same
//! skeleton gives the same order on every game, and a ragdoll with other bodies, or the same in another order, is refused.
//! Pure: tested in tests/ragdoll_bodies_pose.cpp.
namespace RagdollBodiesPose
{
using Clock = std::chrono::steady_clock;

//! A body is sent at most this often: ten times a second, like a person's bones, with room for a late update.
inline constexpr auto kSendGap = std::chrono::milliseconds(90);
//! The bodies are shown this far behind the newest pose, so that there are two poses to go between.
inline constexpr int64_t kDelayMs = 120;
//! A pose this long after the newest arrived is not used: its owner stopped sending.
inline constexpr auto kStale = std::chrono::milliseconds(300);
//! A tick this far behind the newest is a clock set back, not a late pose.
inline constexpr uint64_t kResyncMs = 1000;

//! One rigid body: where it is in the world, in game units, and how it is turned.
struct Body
{
    glm::vec3 Position{};
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
};

[[nodiscard]] inline bool ArticulationChanged(const std::vector<Body>& acPrevious, const std::vector<Body>& acCurrent) noexcept
{
    if (acPrevious.size() != acCurrent.size())
        return false;
    constexpr float cMinimumTravelSquared = 4.f;
    constexpr float cMinimumRotationDot = 0.9986f;
    for (size_t i = 0; i < acCurrent.size(); ++i)
    {
        const glm::vec3 cTravel = acCurrent[i].Position - acPrevious[i].Position;
        const float cTurn = std::abs(glm::dot(acCurrent[i].Rotation, acPrevious[i].Rotation));
        if (glm::dot(cTravel, cTravel) > cMinimumTravelSquared || (std::isfinite(cTurn) && cTurn < cMinimumRotationDot))
            return true;
    }
    return false;
}

//! Whether a ragdoll already lies in the bodies held for it (the first ones, as many as were sent): none of them moved or turned
//! past what ArticulationChanged counts (CorpseRestPose's still tolerances). A reading that is not a number does not lie in it.
[[nodiscard]] inline bool LiesIn(const std::vector<Body>& acHeld, const std::vector<Body>& acHere) noexcept
{
    if (acHeld.empty() || acHeld.size() > acHere.size())
        return false;
    const std::vector<Body> cHere(acHere.begin(), acHere.begin() + static_cast<std::ptrdiff_t>(acHeld.size()));
    const auto cIsNumber = [](const Body& acBody)
    {
        return std::isfinite(glm::dot(acBody.Position, acBody.Position)) && std::isfinite(glm::dot(acBody.Rotation, acBody.Rotation));
    };
    return std::all_of(acHeld.begin(), acHeld.end(), cIsNumber) && std::all_of(cHere.begin(), cHere.end(), cIsNumber) && !ArticulationChanged(acHeld, cHere);
}

//! Which skeleton a ragdoll's bodies make: their nodes' names in order, and how many.
class Layout
{
public:
    void Add(const char* acpName) noexcept
    {
        for (const char* p = acpName; p && *p; ++p)
            Mix(static_cast<uint8_t>(*p));
        Mix(0);
        ++m_count;
    }

    [[nodiscard]] uint32_t Key() const noexcept { return (m_key ^ m_count) * kPrime; }
    [[nodiscard]] uint32_t Count() const noexcept { return m_count; }

private:
    static constexpr uint32_t kPrime = 16777619u;

    void Mix(const uint8_t aByte) noexcept { m_key = (m_key ^ aByte) * kPrime; }

    uint32_t m_key{2166136261u};
    uint32_t m_count{};
};

//! Whether a pose fits the ragdoll here: as many bodies in all, the same layout, and no more sent than that.
[[nodiscard]] constexpr bool Fits(const size_t aCount, const uint32_t aLayout, const size_t aSent, const size_t aCountHere, const uint32_t aLayoutHere) noexcept
{
    return aCount > 0 && aCount == aCountHere && aLayout == aLayoutHere && aSent > 0 && aSent <= aCount;
}

//! Whether the owner's first body lies near enough to this copy's to be the same body carried, not a desync or a bad pose.
//! Every other body lies within an offset's reach of it, so this bounds them all.
[[nodiscard]] inline bool Near(const glm::vec3& acOwner, const glm::vec3& acHere) noexcept
{
    return glm::distance(acOwner, acHere) <= CarriedBodyFollow::kMaxGapUnits;
}

//! The wire form of a ragdoll's bodies, from its 3D root at acRoot; only the first RagdollPose::kMaxBodies go.
//! False when there are none, more than a byte counts, a reading is not a number, or a body lies too far from the root.
[[nodiscard]] inline bool Encode(const uint64_t aTick, const uint32_t aLayout, const glm::vec3& acRoot, const std::vector<Body>& acBodies, RagdollPose& aOut) noexcept
{
    if (acBodies.empty() || acBodies.size() > UINT8_MAX || !std::isfinite(acRoot.x) || !std::isfinite(acRoot.y) || !std::isfinite(acRoot.z))
        return false;
    aOut.Tick = aTick;
    aOut.Layout = aLayout;
    aOut.BodyCount = static_cast<uint8_t>(acBodies.size());
    aOut.Root[0] = acRoot.x;
    aOut.Root[1] = acRoot.y;
    aOut.Root[2] = acRoot.z;
    aOut.Bodies.resize(std::min(acBodies.size(), RagdollPose::kMaxBodies));
    for (size_t i = 0; i < aOut.Bodies.size(); ++i)
    {
        const glm::vec3 cSteps = (acBodies[i].Position - acRoot) / RagdollPose::kOffsetStep;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!(std::abs(cSteps[axis]) <= static_cast<float>(INT16_MAX)))
                return false;
            aOut.Bodies[i].Offset[axis] = static_cast<int16_t>(std::lround(cSteps[axis]));
        }
        const glm::quat& cTurn = acBodies[i].Rotation;
        if (!std::isfinite(cTurn.x) || !std::isfinite(cTurn.y) || !std::isfinite(cTurn.z) || !std::isfinite(cTurn.w) || !(glm::length(cTurn) > 0.5f))
            return false;
        aOut.Bodies[i].Rotation = glm::normalize(cTurn);
    }
    return true;
}

//! The bodies a pose sends, in the world. False when its root is not a number.
[[nodiscard]] inline bool Decode(const RagdollPose& acPose, std::vector<Body>& aOut) noexcept
{
    const glm::vec3 cRoot{acPose.Root[0], acPose.Root[1], acPose.Root[2]};
    if (!std::isfinite(cRoot.x) || !std::isfinite(cRoot.y) || !std::isfinite(cRoot.z))
        return false;
    aOut.resize(acPose.Bodies.size());
    for (size_t i = 0; i < aOut.size(); ++i)
    {
        const auto& cOffset = acPose.Bodies[i].Offset;
        aOut[i].Position = cRoot + glm::vec3{cOffset[0], cOffset[1], cOffset[2]} * RagdollPose::kOffsetStep;
        aOut[i].Rotation = glm::normalize(static_cast<glm::quat>(acPose.Bodies[i].Rotation));
    }
    return true;
}

//! Two poses of the same bodies, aT of the way from the first to the second.
[[nodiscard]] inline std::vector<Body> Between(const std::vector<Body>& acFrom, const std::vector<Body>& acTo, const float aT)
{
    const float cT = std::clamp(aT, 0.f, 1.f);
    std::vector<Body> bodies(std::min(acFrom.size(), acTo.size()));
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        bodies[i].Position = glm::mix(acFrom[i].Position, acTo[i].Position, cT);
        bodies[i].Rotation = glm::normalize(glm::slerp(acFrom[i].Rotation, acTo[i].Rotation, cT));
    }
    return bodies;
}

//! Whether this game sends a body's pose again, aLastSent being when it last did (epoch: never).
[[nodiscard]] inline bool SendDue(const Clock::time_point aLastSent, const Clock::time_point aNow) noexcept
{
    return aLastSent == Clock::time_point{} || aNow - aLastSent >= kSendGap;
}

//! One copy's poses as they arrive, shown kDelayMs behind the newest.
class Track
{
public:
    //! A pose received at aAt. One no newer than the newest is dropped, as updates arrive out of order. A clock set back starts over,
    //! and so does another skeleton, which is never blended with the last, and a stream that had stopped: another owner's clock may be behind.
    //! False when it was dropped.
    bool Push(const uint64_t aTick, const uint32_t aLayout, const size_t aCount, std::vector<Body> aBodies, const Clock::time_point aAt)
    {
        if (!m_samples.empty())
        {
            const Sample& cNewest = m_samples.back();
            const bool cSkeleton = cNewest.Layout != aLayout || cNewest.Count != aCount || cNewest.Bodies.size() != aBodies.size();
            const bool cSetBack = aTick < cNewest.Tick && cNewest.Tick - aTick > kResyncMs;
            if (cSkeleton || cSetBack || aAt - m_newestAt > kStale)
                m_samples.clear();
            else if (aTick <= cNewest.Tick)
                return false;
        }
        m_samples.push_back(Sample{aTick, aLayout, aCount, std::move(aBodies)});
        while (m_samples.size() > kKept)
            m_samples.pop_front();
        m_newestAt = aAt;
        return true;
    }

    //! A pose arrived lately.
    [[nodiscard]] bool Fresh(const Clock::time_point aNow) const noexcept { return !m_samples.empty() && aNow - m_newestAt <= kStale; }

    [[nodiscard]] Clock::time_point NewestAt() const noexcept { return m_newestAt; }

    //! The bodies as shown at aNow, for a ragdoll of aCountHere bodies whose layout is aLayoutHere. Those past the sent ones are not given.
    //! False when no pose arrived lately, or it is another skeleton's.
    [[nodiscard]] bool BodiesAt(const Clock::time_point aNow, const size_t aCountHere, const uint32_t aLayoutHere, std::vector<Body>& aOut) const
    {
        if (!Fresh(aNow))
            return false;
        const Sample& cNewest = m_samples.back();
        if (!Fits(cNewest.Count, cNewest.Layout, cNewest.Bodies.size(), aCountHere, aLayoutHere))
            return false;

        const int64_t cSince = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(aNow - m_newestAt).count());
        const int64_t cShown = static_cast<int64_t>(cNewest.Tick) + cSince - kDelayMs;
        if (cShown >= static_cast<int64_t>(cNewest.Tick))
        {
            aOut = cNewest.Bodies;
            return true;
        }
        for (size_t i = m_samples.size() - 1; i > 0; --i)
        {
            const Sample& cFrom = m_samples[i - 1];
            const Sample& cTo = m_samples[i];
            if (cShown < static_cast<int64_t>(cFrom.Tick))
                continue;
            const float cSpan = static_cast<float>(cTo.Tick - cFrom.Tick);
            aOut = Between(cFrom.Bodies, cTo.Bodies, cSpan > 0.f ? static_cast<float>(cShown - static_cast<int64_t>(cFrom.Tick)) / cSpan : 1.f);
            return true;
        }
        aOut = m_samples.front().Bodies;
        return true;
    }

private:
    static constexpr size_t kKept = 3;

    struct Sample
    {
        uint64_t Tick{};
        uint32_t Layout{};
        size_t Count{};
        std::vector<Body> Bodies;
    };

    std::deque<Sample> m_samples;
    Clock::time_point m_newestAt{};
};
} // namespace RagdollBodiesPose
