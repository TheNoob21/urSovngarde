#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <iosfwd>
#include <string_view>
#include <Structs/AnimationVariables.h>

//! How a remote actor's copy moves on from what its owner sent, where the plain interpolation would jump or step.
namespace RemoteSmoothing
{
//! A copy that changed hands is taken from where it stood to where its new owner has it over this long.
inline constexpr double kHandOffBlendMs = 150.0;
//! A wider gap at a hand-off is closed at once, as the movement's own corrections are.
inline constexpr float kMaxHandOffGap = 256.f;

//! How much of a hand-off's gap is still shown aSinceMs after the copy's first placement: all of it at first, none from
//! kHandOffBlendMs on, easing in and out so that the copy starts and stops gliding gently.
inline float HandOffWeight(const double aSinceMs) noexcept
{
    if (!(aSinceMs > 0.0))
        return 1.f;
    if (aSinceMs >= kHandOffBlendMs)
        return 0.f;
    const double cShare = aSinceMs / kHandOffBlendMs;
    return static_cast<float>(1.0 - cShare * cShare * (3.0 - 2.0 * cShare));
}

//! The gap a copy glides over when it changes hands: from where it is shown to its new owner's first target. None when it
//! is too wide to glide, or not a number.
inline glm::vec3 HandOffGap(const glm::vec3& acShown, const glm::vec3& acTarget) noexcept
{
    const glm::vec3 cGap = acShown - acTarget;
    const float cLength = glm::length(cGap);
    return std::isfinite(cLength) && cLength <= kMaxHandOffGap ? cGap : glm::vec3{};
}

//! How one of the graph's float variables is shown between two of its owner's points.
enum class FloatBlend : uint8_t
{
    //! Taken whole from the newer point: a switch, a weight or anything else not known to move smoothly.
    Newer,
    //! A speed or a turn rate: blended as the position is.
    Linear,
    //! A heading as a share of a full turn, 0 to 1: blended the short way round, across the wrap.
    Turn,
};

//! How the graph float named aName is shown between two points. Only the legs' speed, turn and direction are blended.
inline FloatBlend BlendOf(const std::string_view aName) noexcept
{
    if (aName == "Direction")
        return FloatBlend::Turn;
    constexpr std::string_view cLinear[]{"Speed", "SpeedSampled", "SampledSpeed", "HorseSpeedSampled", "SpeedDamped", "TurnDelta", "TurnDeltaDamped"};
    for (const auto cName : cLinear)
        if (aName == cName)
            return FloatBlend::Linear;
    return FloatBlend::Newer;
}

//! One of the graph's float variables between two of its owner's points, by its kind. A heading outside 0 to 1 is not one
//! this knows how to wrap: it is the newer point's.
inline float BlendFloat(const FloatBlend aKind, const float aOlder, const float aNewer, const float aDelta) noexcept
{
    if (aKind == FloatBlend::Newer || !std::isfinite(aOlder) || !std::isfinite(aNewer))
        return aNewer;
    if (aKind == FloatBlend::Linear)
        return aOlder + (aNewer - aOlder) * aDelta;
    if (aOlder < 0.f || aOlder > 1.f || aNewer < 0.f || aNewer > 1.f)
        return aNewer;
    float turn = aNewer - aOlder;
    turn -= std::round(turn);
    float heading = aOlder + turn * aDelta;
    heading -= std::floor(heading);
    // A hair below 0 lands on 1, the same heading.
    return heading < 1.f ? heading : 0.f;
}

//! The graph variables to show between two points: the newer point's booleans and integers, and the floats acKinds names
//! as continuous blended by aDelta, as the position is. acKinds holds one kind per float slot of the copy's own graph; lists
//! of another length (another graph) are the newer point's whole.
inline void
BlendVariables(const AnimationVariables& acOlder, const AnimationVariables& acNewer, const float aDelta, const TiltedPhoques::Vector<FloatBlend>& acKinds, AnimationVariables& aOut)
{
    aOut = acNewer;
    const float cDelta = std::isfinite(aDelta) ? std::clamp(aDelta, 0.f, 1.f) : 1.f;
    if (acOlder.Floats.size() != acNewer.Floats.size() || acKinds.size() != acNewer.Floats.size() || cDelta >= 1.f)
        return;
    for (size_t i = 0; i < aOut.Floats.size(); ++i)
        aOut.Floats[i] = BlendFloat(acKinds[i], acOlder.Floats[i], acNewer.Floats[i], cDelta);
}

//! At most this many of the owner's actions are played on a copy in one frame.
inline constexpr uint32_t kMaxActionsPerFrame = 3;

//! Whether another action is played on a copy in the same frame: one more was due, the owner performed it in the same
//! frame as the last one (the same tick), the last one was taken off the queue, and the copy is not replaying the history
//! it was spawned with, which plays one a frame as before. An action from a later frame of the owner's waits for the copy's
//! next frame, so its graph has the frame between them that the owner's had: a bash after its block, an attack after a draw.
constexpr bool PlayAnotherAction(const uint32_t aPlayed, const bool aLastTaken, const bool aNextDue, const bool aSameOwnerFrame, const bool aReplaying) noexcept
{
    return aPlayed < kMaxActionsPerFrame && aLastTaken && aNextDue && aSameOwnerFrame && !aReplaying;
}
} // namespace RemoteSmoothing
