#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Structs/PropBodyState.h>

// A placed object's hinged or hanging bodies. A shop sign's root body is fixed to the wall bracket and
// the board, rings and charms hang below it on hinges, each with its own dynamic body (Riverwood Trader, SignRiverwoodRiverwoodTrader01.nif:
// a fixed root and seven dynamic bodies on limited hinges). A hit swings those bodies while the root stays put, so a world object's
// root move never starts. Their poses travel in the root's space, and the other games drive their own bodies there with a speed,
// so the hinges keep working between updates.
namespace WorldObjectBodies
{
using Clock = std::chrono::steady_clock;

//! A body that left where it rested by this much, or turned this much (about 7 degrees), is swung, not settling.
inline constexpr float kStartDistance = 4.f;
inline constexpr float kStartAngle = 0.12f;
//! Less than this since the last send is a body at rest.
inline constexpr float kMovedDistance = 0.5f;
inline constexpr float kMovedAngle = 0.02f;
//! Sent ten times a second while they swing, on the 30 Hz pass grid with some slack.
inline constexpr auto kSendEvery = std::chrono::milliseconds(100);
inline constexpr auto kSendSlack = std::chrono::milliseconds(10);
//! Still this long: at rest.
inline constexpr auto kRestAfter = std::chrono::milliseconds(1000);
//! Driven here until half a second after the other side's rest, or two seconds after its last update.
inline constexpr auto kHeldForAtRest = std::chrono::milliseconds(500);
inline constexpr auto kHeldFor = std::chrono::seconds(2);
//! How soon a driven body is to reach its target, and the fastest it is driven.
inline constexpr float kReachSeconds = 0.1f;
inline constexpr float kMaxSpeed = 2000.f;
inline constexpr float kMaxSpin = 30.f;
//! Between updates a driven target carries on at its own speed for at most this long.
inline constexpr float kMaxLead = 0.15f;
//! After another player's swing is let go, the bodies are settled until still for kRestAfter, or at most this long.
inline constexpr auto kHoldCap = std::chrono::seconds(5);

//! How long a sample of the bodies read between frames is good for: their poses, and a probe of how many there are.
inline constexpr auto kPoseSampleFor = std::chrono::milliseconds(100);
inline constexpr auto kProbeSampleFor = std::chrono::seconds(2);
//! Poses are read again once older than this, about one 30 Hz pass: a long frame cannot outlast them, and they go out stamped
//! with the time they are sent.
inline constexpr auto kPoseReuseFor = std::chrono::milliseconds(33);

//! Whether a sample of the bodies read at aReadAt, good for aLifetime, is still good for every reading until the frame after the next
//! (frames aFrame apart). It is read again only once it would not be, so a reader between frames never finds it stale.
inline bool SampleServesNextFrame(const Clock::time_point aReadAt, const Clock::duration aLifetime, const Clock::time_point aNow, const Clock::duration aFrame) noexcept
{
    return aNow - aReadAt + 2 * aFrame < aLifetime;
}

//! Whether a sample read at aReadAt is kept this frame rather than read again, aFrame after the last frame: a probe while it
//! serves until the frame after the next, poses only while younger than kPoseReuseFor as well.
inline bool KeepsSample(const Clock::time_point aReadAt, const bool aProbe, const Clock::time_point aNow, const Clock::duration aFrame) noexcept
{
    if (aProbe)
        return SampleServesNextFrame(aReadAt, kProbeSampleFor, aNow, aFrame);
    return aNow - aReadAt < kPoseReuseFor && SampleServesNextFrame(aReadAt, kPoseSampleFor, aNow, aFrame);
}

//! What a read of the bodies found: no sample yet, or a stale one (it is read between frames), nothing to read, or their poses.
enum class Sample : uint8_t
{
    Waiting,
    Empty,
    Read
};

//! Whether a read ends what this game knew of the bodies, so a swing sent from here is sent at rest and they are settled: they
//! could not be read, or are laid out otherwise now. A sample not read yet, or gone stale, ends nothing: a rest needs poses.
inline bool EndsSwing(const Sample aSample, const uint32_t aLayout, const uint32_t aKnownLayout) noexcept
{
    return aSample == Sample::Empty || (aSample == Sample::Read && aLayout != aKnownLayout);
}

using Pose = PropBodyPose;
using Poses = std::vector<Pose>;

//! A body that Havok moves (hkpMotion::MotionType): dynamic, sphere, box and thin box inertia. Keyframed (4) and fixed (5) never swing.
[[nodiscard]] inline bool IsDynamicMotion(const uint8_t aType) noexcept
{
    return (aType >= 1 && aType <= 3) || aType == 6;
}

//! The placed objects whose meshes hang bodies on hinges (a census of Skyrim.esm's models): movable statics (36: most shop and
//! inn signs), activators (24: Windhelm's signs, swinging traps), containers (28: hanging spit pots), flora (39: hanging game and
//! fish) and furniture (40). Items keep their own root moves, and statics never hang anything in the world.
[[nodiscard]] inline bool IsBodiesType(const uint8_t aFormType) noexcept
{
    return aFormType == 24 || aFormType == 28 || aFormType == 36 || aFormType == 39 || aFormType == 40;
}

//! The angle in radians between two rotations, the short way.
[[nodiscard]] inline float AngleBetween(const glm::quat& acA, const glm::quat& acB) noexcept
{
    const float cDot = std::fabs(glm::dot(glm::normalize(acA), glm::normalize(acB)));
    return 2.f * std::acos(std::min(1.f, cDot));
}

//! The biggest move and turn of any body between two readings of the same bodies. False when the counts differ.
[[nodiscard]] inline bool Compare(const Poses& acFrom, const Poses& acTo, float& aDistance, float& aAngle) noexcept
{
    aDistance = 0.f;
    aAngle = 0.f;
    if (acFrom.size() != acTo.size())
        return false;
    for (size_t i = 0; i < acFrom.size(); ++i)
    {
        aDistance = std::max(aDistance, glm::distance(acFrom[i].Position, acTo[i].Position));
        aAngle = std::max(aAngle, AngleBetween(acFrom[i].Rotation, acTo[i].Rotation));
    }
    return true;
}

//! A world pose taken into a root's space (rotation, position and uniform scale of its 3D), and back.
[[nodiscard]] inline Pose ToRoot(const glm::mat3& acRootTurn, const glm::vec3& acRootAt, const float aRootScale, const glm::vec3& acAt, const glm::quat& acTurn) noexcept
{
    const float cScale = aRootScale > 0.f ? aRootScale : 1.f;
    return {glm::transpose(acRootTurn) * (acAt - acRootAt) / cScale, glm::normalize(glm::conjugate(glm::quat_cast(acRootTurn)) * acTurn)};
}

[[nodiscard]] inline Pose FromRoot(const glm::mat3& acRootTurn, const glm::vec3& acRootAt, const float aRootScale, const Pose& acPose) noexcept
{
    const float cScale = aRootScale > 0.f ? aRootScale : 1.f;
    return {acRootAt + acRootTurn * (acPose.Position * cScale), glm::normalize(glm::quat_cast(acRootTurn) * acPose.Rotation)};
}

//! The speed that goes from one place to another in aSeconds, capped at kMaxSpeed.
[[nodiscard]] inline glm::vec3 VelocityBetween(const glm::vec3& acFrom, const glm::vec3& acTo, const float aSeconds) noexcept
{
    if (!(aSeconds > 0.f))
        return {};
    glm::vec3 velocity = (acTo - acFrom) / aSeconds;
    const float cSpeed = glm::length(velocity);
    if (!std::isfinite(cSpeed))
        return {};
    if (cSpeed > kMaxSpeed)
        velocity *= kMaxSpeed / cSpeed;
    return velocity;
}

//! The spin (axis times radians a second, in the frame both rotations are in) that turns one into the other in aSeconds, the short way,
//! capped at kMaxSpin.
[[nodiscard]] inline glm::vec3 SpinBetween(const glm::quat& acFrom, const glm::quat& acTo, const float aSeconds) noexcept
{
    if (!(aSeconds > 0.f))
        return {};
    glm::quat delta = glm::normalize(acTo) * glm::conjugate(glm::normalize(acFrom));
    if (delta.w < 0.f)
        delta = -delta;
    const float cHalfSin = glm::length(glm::vec3{delta.x, delta.y, delta.z});
    if (!(cHalfSin > 1e-6f) || !std::isfinite(cHalfSin))
        return {};
    const float cAngle = 2.f * std::atan2(cHalfSin, delta.w);
    const float cSpin = std::min(cAngle / aSeconds, kMaxSpin);
    return glm::vec3{delta.x, delta.y, delta.z} / cHalfSin * cSpin;
}

//! The speed that takes a body from where it is to its target in aReach seconds, capped. A frame longer than kReachSeconds is
//! taken as the reach, so a hitch does not overshoot.
[[nodiscard]] inline glm::vec3 LinearToward(const glm::vec3& acFrom, const glm::vec3& acTo, const float aReach = kReachSeconds) noexcept
{
    return VelocityBetween(acFrom, acTo, std::max(aReach, kReachSeconds));
}

//! The spin (world space) that turns a body to its target in aReach seconds, the short way, capped.
[[nodiscard]] inline glm::vec3 AngularToward(const glm::quat& acFrom, const glm::quat& acTo, const float aReach = kReachSeconds) noexcept
{
    return SpinBetween(acFrom, acTo, std::max(aReach, kReachSeconds));
}

//! Where a driven body goes, and how fast its target moves there (root space: game units a second, radians a second).
struct Target
{
    Pose At{};
    glm::vec3 Velocity{};
    glm::vec3 Spin{};
};
using Targets = std::vector<Target>;

//! The targets carried on at their own speed for aSeconds (0 to kMaxLead).
[[nodiscard]] inline Targets Lead(const Targets& acTargets, const float aSeconds) noexcept
{
    const float cLead = std::clamp(aSeconds, 0.f, kMaxLead);
    Targets led = acTargets;
    for (Target& target : led)
    {
        target.At.Position += target.Velocity * cLead;
        if (const float cSpin = glm::length(target.Spin); cSpin > 1e-6f)
            target.At.Rotation = glm::normalize(glm::angleAxis(cSpin * cLead, target.Spin / cSpin) * target.At.Rotation);
    }
    return led;
}

//! What a watched object's bodies are doing here.
enum class Step
{
    None,
    //! They left where they rested: the caller judges the start with the other starts of the pass, then calls Begin.
    Start,
    //! Still swinging, and a send is due.
    Move,
    //! Still for kRestAfter: send them at rest. Already settled where they lie.
    Rest,
};

//! One object's bodies as this game moves them.
struct Track
{
    //! Where they lay when last still: a start is measured from here.
    Poses Rested;
    //! What was sent last.
    Poses Sent;
    bool Moving{};
    Clock::time_point LastMove{};
    Clock::time_point LastSent{};
    //! After another player's swing is let go: settled until still for kRestAfter since StillSince, at most until HoldUntil.
    Clock::time_point HoldUntil{};
    Clock::time_point StillSince{};

    //! Takes where they lie as at rest: first seen, quiet, or driven for another player.
    void Settle(const Poses& acNow) noexcept
    {
        Rested = acNow;
        Sent = acNow;
        Moving = false;
    }

    //! Settled until still, after another player's swing is let go here: its tail is not sent back as a swing.
    void Hold(const Clock::time_point aNow) noexcept
    {
        HoldUntil = aNow + kHoldCap;
        StillSince = aNow;
        Moving = false;
    }

    //! Another player's swing came: theirs is shown, this game's own is no longer sent.
    void GiveWay() noexcept { Moving = false; }

    //! A start was sent.
    void Begin(const Poses& acNow, const Clock::time_point aNow) noexcept
    {
        Sent = acNow;
        Moving = true;
        LastMove = aNow;
        LastSent = aNow;
    }

    //! A reading of this pass. aDistance and aAngle: how far they are from where they rested (not moving) or from the last send.
    [[nodiscard]] Step Next(const Poses& acNow, const Clock::time_point aNow, float& aDistance, float& aAngle) noexcept
    {
        if (aNow < HoldUntil)
        {
            if (!Compare(Rested, acNow, aDistance, aAngle) || aDistance > kMovedDistance || aAngle > kMovedAngle)
                StillSince = aNow;
            Rested = acNow;
            Sent = acNow;
            if (aNow - StillSince >= kRestAfter)
                HoldUntil = {};
            return Step::None;
        }
        // Another count is another 3D (rebuilt, or a body left the world): start over from it.
        if (!Compare(Moving ? Sent : Rested, acNow, aDistance, aAngle))
        {
            Settle(acNow);
            return Step::None;
        }
        if (!Moving)
            return aDistance > kStartDistance || aAngle > kStartAngle ? Step::Start : Step::None;

        if (aDistance > kMovedDistance || aAngle > kMovedAngle)
        {
            LastMove = aNow;
            if (aNow - LastSent < kSendEvery - kSendSlack)
                return Step::None;
            Sent = acNow;
            LastSent = aNow;
            return Step::Move;
        }
        if (aNow - LastMove >= kRestAfter)
        {
            Settle(acNow);
            return Step::Rest;
        }
        return Step::None;
    }

    //! A pass's reading. aBusy: driven for another player, held, carried whole or quiet; the bodies are then only settled, and a
    //! swing this game was sending ends with a rest (Step::Rest), so the other games do not hold its last pose.
    [[nodiscard]] Step Observe(const Poses& acNow, const bool aBusy, const Clock::time_point aNow, float& aDistance, float& aAngle) noexcept
    {
        aDistance = 0.f;
        aAngle = 0.f;
        if (!aBusy)
            return Next(acNow, aNow, aDistance, aAngle);
        const bool cWasMoving = Moving;
        Settle(acNow);
        return cWasMoving ? Step::Rest : Step::None;
    }
};

//! The other side's poses for an object's bodies, driven here until it lets them go.
struct Driven
{
    Targets Target;
    uint32_t Layout{};
    //! The sender's tick (milliseconds) of Target, to order updates and time their speed.
    uint64_t Tick{};
    Clock::time_point LastRemote{};
    bool AtRest{};
    bool On{};

    //! False for an update older than the one shown. Each body's speed comes from the last update to this one, by the sender's ticks.
    bool Take(const Poses& acPoses, const bool aAtRest, const uint64_t aTick, const Clock::time_point aNow) noexcept
    {
        if (On && aTick && Tick && aTick <= Tick)
            return false;
        const float cSeconds = On && aTick && Tick && Target.size() == acPoses.size() ? static_cast<float>(aTick - Tick) / 1000.f : 0.f;
        const bool cTimed = cSeconds > 0.f && cSeconds <= 1.f && !aAtRest;
        Targets next(acPoses.size());
        for (size_t i = 0; i < acPoses.size(); ++i)
        {
            next[i].At = acPoses[i];
            if (cTimed)
            {
                next[i].Velocity = VelocityBetween(Target[i].At.Position, acPoses[i].Position, cSeconds);
                next[i].Spin = SpinBetween(Target[i].At.Rotation, acPoses[i].Rotation, cSeconds);
            }
        }
        Target = std::move(next);
        Tick = aTick;
        AtRest = aAtRest;
        LastRemote = aNow;
        On = true;
        return true;
    }

    //! Where the bodies go now: the last update carried on at its own speed since it came.
    [[nodiscard]] Targets Now(const Clock::time_point aNow) const noexcept { return Lead(Target, std::chrono::duration<float>(aNow - LastRemote).count()); }

    [[nodiscard]] bool LetGo(const Clock::time_point aNow) const noexcept
    {
        return On && aNow - LastRemote > (AtRest ? Clock::duration(kHeldForAtRest) : Clock::duration(kHeldFor));
    }

    bool Release(Track& aTrack, const Clock::time_point aNow) noexcept
    {
        if (!LetGo(aNow))
            return false;
        On = false;
        aTrack.Hold(aNow);
        return true;
    }
};
} // namespace WorldObjectBodies
