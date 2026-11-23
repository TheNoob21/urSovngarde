#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

#include <Structs/RagdollPose.h>
#include <Structs/VRPose.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

//! The pose a dead body came to rest in, kept by the server for the players sent the body later (CharacterService::SendSpawn).
//! Its owner sends a body's pose while it moves and repeats the last one for a while after it comes to rest: a pose the same
//! as the one before is the body at rest. Pure: tested in tests/corpse_rest_pose.cpp.
namespace CorpseRestPose
{
//! At most this many bodies are kept; the one kept longest ago goes first.
inline constexpr size_t kMaxBodies = 512;
//! A root that moved less than this has not moved: a settled ragdoll still twitches (CaptureBodyPose's 4 units).
inline constexpr float kStillUnits = 4.f;
//! A bone that turned less than this has not moved (CopyBodyRules::kBoneRestDegrees).
inline constexpr float kStillDegrees = 2.f;
//! A creature's body that moved less than this from its root has not moved (RagdollBodiesPose::ArticulationChanged).
inline constexpr float kStillBodyUnits = 2.f;
//! A creature's body turned less than this (the dot of its two turns) has not turned (RagdollBodiesPose::ArticulationChanged).
inline constexpr float kStillBodyDot = 0.9986f;

//! What a report from a body's owner does to the pose kept for it.
enum class Report : uint8_t
{
    kIgnore, //!< nothing: it carries no pose, or one with no place in the world
    kTake,   //!< taken: kept when the body is at rest (Store::Take)
    kForget, //!< what is kept is dropped: the actor is alive
};

//! A report of an actor from its owner. aPosed: it carries the body's pose; aUsableRoot: the pose's root is a place in the world.
[[nodiscard]] constexpr Report OnReport(const bool aDead, const bool aPosed, const bool aUsableRoot) noexcept
{
    if (!aDead)
        return Report::kForget;
    if (!aPosed || !aUsableRoot)
        return Report::kIgnore;
    return Report::kTake;
}

//! Whether a person's pose is the one before it again: the same bones, the root where it was.
[[nodiscard]] inline bool SameRest(const VRPose& acBefore, const VRPose& acNow) noexcept
{
    if (!acBefore.HasData || !acNow.HasData || acBefore.NoBones || acNow.NoBones || acBefore.HasLegs != acNow.HasLegs || acBefore.HasRootPosition != acNow.HasRootPosition)
        return false;
    if (acNow.HasRootPosition)
    {
        const glm::vec3 cMoved = glm::vec3{acNow.RootPosition[0], acNow.RootPosition[1], acNow.RootPosition[2]} -
                                 glm::vec3{acBefore.RootPosition[0], acBefore.RootPosition[1], acBefore.RootPosition[2]};
        if (!(glm::dot(cMoved, cMoved) < kStillUnits * kStillUnits))
            return false;
    }
    const size_t cCount = acNow.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
    for (size_t i = 0; i < cCount; ++i)
    {
        const float cDot = std::abs(glm::dot(static_cast<const glm::quat&>(acBefore.Bones[i]), static_cast<const glm::quat&>(acNow.Bones[i])));
        if (!(glm::degrees(2.f * std::acos(std::min(1.f, cDot))) < kStillDegrees))
            return false;
    }
    return true;
}

//! Whether a creature's bodies are where they were before: the same skeleton, the root where it was, each body where it was.
[[nodiscard]] inline bool SameRest(const RagdollPose& acBefore, const RagdollPose& acNow) noexcept
{
    if (acBefore.Layout != acNow.Layout || acBefore.BodyCount != acNow.BodyCount || acBefore.Bodies.size() != acNow.Bodies.size() || acNow.Bodies.empty())
        return false;
    const glm::vec3 cMoved = glm::vec3{acNow.Root[0], acNow.Root[1], acNow.Root[2]} - glm::vec3{acBefore.Root[0], acBefore.Root[1], acBefore.Root[2]};
    if (!(glm::dot(cMoved, cMoved) < kStillUnits * kStillUnits))
        return false;
    constexpr float cStillSteps = kStillBodyUnits / RagdollPose::kOffsetStep;
    for (size_t i = 0; i < acNow.Bodies.size(); ++i)
    {
        const auto& cFrom = acBefore.Bodies[i];
        const auto& cTo = acNow.Bodies[i];
        const glm::vec3 cShift{
            static_cast<float>(cTo.Offset[0] - cFrom.Offset[0]), static_cast<float>(cTo.Offset[1] - cFrom.Offset[1]), static_cast<float>(cTo.Offset[2] - cFrom.Offset[2])};
        const float cDot = std::abs(glm::dot(static_cast<const glm::quat&>(cFrom.Rotation), static_cast<const glm::quat&>(cTo.Rotation)));
        if (glm::dot(cShift, cShift) > cStillSteps * cStillSteps || !(cDot >= kStillBodyDot))
            return false;
    }
    return true;
}

//! The poses of dead bodies by their entity id: the last one each body's owner sent, and whether the body was at rest in it.
//! At most TMax bodies; a new one past that drops the body taken longest ago.
template <class TPose, size_t TMax = kMaxBodies> class Store
{
public:
    //! A pose of a dead body from its owner. Kept as where it rests when aSame says it is the one before it again; another means the
    //! body moves, and nothing is kept for it until it rests again.
    template <class TSame> void Take(const uint32_t aId, const TPose& acPose, TSame&& aSame)
    {
        auto it = m_bodies.find(aId);
        if (it == m_bodies.end())
        {
            if (m_bodies.size() >= TMax && !m_byAge.empty())
            {
                m_bodies.erase(m_byAge.begin()->second);
                m_byAge.erase(m_byAge.begin());
            }
            it = m_bodies.emplace(aId, Body{acPose, 0, false}).first;
        }
        else
        {
            it->second.Resting = aSame(it->second.Last, acPose);
            it->second.Last = acPose;
            m_byAge.erase(it->second.Age);
        }
        it->second.Age = ++m_age;
        m_byAge.emplace(it->second.Age, aId);
    }

    //! The same, at rest when SameRest says so.
    void Take(const uint32_t aId, const TPose& acPose)
    {
        Take(aId, acPose, [](const TPose& acBefore, const TPose& acNow) { return SameRest(acBefore, acNow); });
    }

    //! The pose the body rests in, or null while it moves or nothing was taken.
    [[nodiscard]] const TPose* Resting(const uint32_t aId) const noexcept
    {
        const auto it = m_bodies.find(aId);
        return it != m_bodies.end() && it->second.Resting ? &it->second.Last : nullptr;
    }

    //! The body is alive again, or gone.
    void Forget(const uint32_t aId) noexcept
    {
        const auto it = m_bodies.find(aId);
        if (it == m_bodies.end())
            return;
        m_byAge.erase(it->second.Age);
        m_bodies.erase(it);
    }

    [[nodiscard]] size_t Size() const noexcept { return m_bodies.size(); }

private:
    struct Body
    {
        TPose Last;
        uint64_t Age;
        bool Resting;
    };

    std::unordered_map<uint32_t, Body> m_bodies;
    std::map<uint64_t, uint32_t> m_byAge;
    uint64_t m_age{};
};
} // namespace CorpseRestPose
