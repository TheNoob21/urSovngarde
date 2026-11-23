#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Services/HandMotion.h>
#include <Services/ReferenceAngles.h>
#include <Services/WorldObjectBodies.h>
#include <Systems/PlaybackClock.h>

// An object another player is moving, as this game shows it: on the mover's clock rather than one jump per update, and,
// while it sits in a hand of the mover's copy, drawn from that hand so it moves with it.
// Plain values in and out, so it has unit tests (Code/tests/held_objects.cpp).
namespace HeldObject
{
struct Pose
{
    glm::vec3 Position{};
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
};

inline bool IsFinite(const Pose& acPose) noexcept
{
    return std::isfinite(acPose.Position.x) && std::isfinite(acPose.Position.y) && std::isfinite(acPose.Position.z) && std::isfinite(acPose.Rotation.w) &&
           std::isfinite(acPose.Rotation.x) && std::isfinite(acPose.Rotation.y) && std::isfinite(acPose.Rotation.z);
}

// The turn a reference's angles give its 3D, Rx(-x) * Ry(-y) * Rz(-z): the inverse of ReferenceAngles::FromTurn.
inline glm::quat FromAngles(const glm::vec3& acAngles) noexcept
{
    const glm::quat cX = glm::angleAxis(-acAngles.x, glm::vec3(1.f, 0.f, 0.f));
    const glm::quat cY = glm::angleAxis(-acAngles.y, glm::vec3(0.f, 1.f, 0.f));
    const glm::quat cZ = glm::angleAxis(-acAngles.z, glm::vec3(0.f, 0.f, 1.f));
    return glm::normalize(cX * cY * cZ);
}

inline glm::vec3 ToAngles(const glm::quat& acRotation) noexcept
{
    return ReferenceAngles::FromTurn(glm::mat3_cast(glm::normalize(acRotation)));
}

// The angle in radians between two turns.
inline float AngleBetween(const glm::quat& acFrom, const glm::quat& acTo) noexcept
{
    return 2.f * std::acos(std::clamp(std::fabs(glm::dot(glm::normalize(acFrom), glm::normalize(acTo))), 0.f, 1.f));
}

// The shortest way from one turn to the other, aT of the way.
inline glm::quat Slerp(const glm::quat& acFrom, glm::quat aTo, const float aT) noexcept
{
    float cosine = glm::dot(acFrom, aTo);
    if (cosine < 0.f)
    {
        aTo = -aTo;
        cosine = -cosine;
    }
    // Nearly the same turn: a straight blend is as good and has no division by a tiny sine.
    if (cosine > 0.9995f)
        return glm::normalize(acFrom + aT * (aTo - acFrom));
    const float cAngle = std::acos(std::clamp(cosine, -1.f, 1.f));
    const float cSine = std::sin(cAngle);
    return glm::normalize(acFrom * (std::sin((1.f - aT) * cAngle) / cSine) + aTo * (std::sin(aT * cAngle) / cSine));
}

inline Pose Blend(const Pose& acFrom, const Pose& acTo, const float aT) noexcept
{
    const float cT = std::clamp(aT, 0.f, 1.f);
    return {acFrom.Position + cT * (acTo.Position - acFrom.Position), Slerp(acFrom.Rotation, acTo.Rotation, cT)};
}

//! The fastest an object leaves a hand, thrown or let go from a copy's hand, and spins: a hard throw, not a bad read.
inline constexpr float kMaxThrowSpeed = WorldObjectBodies::kMaxSpeed;
inline constexpr float kMaxThrowSpin = WorldObjectBodies::kMaxSpin;

//! A speed capped at aMax, or none when it is not a number.
[[nodiscard]] inline glm::vec3 CapSpeed(const glm::vec3& acVelocity, const float aMax) noexcept
{
    const float cSpeed = glm::length(acVelocity);
    if (!std::isfinite(cSpeed))
        return {};
    return cSpeed > aMax ? acVelocity * (aMax / cSpeed) : acVelocity;
}

// The last few updates of an object, by the time they are put at (ms) on one line, shown a little behind the newest.
// The line is the sender's tick when the updates carry one (see Timeline), else when they arrived here.
class Playback
{
public:
    static constexpr size_t kUpdates = 8;
    // Shown at least this far behind the newest update: 33 ms apart on average, up to two of the sender's frames when it skips one (SendGrid.h).
    static constexpr double kMinDelay = 50.0;
    // On a bad connection it moves late rather than ever later.
    static constexpr double kMaxDelay = 200.0;
    static constexpr double kMargin = 10.0;
    // A gap this long is a pause (a hand held still sends nothing), not a late update.
    static constexpr double kPause = 500.0;
    // When the shown moment passes the newest update, Show goes on at the speed of the last two for this long, then holds.
    static constexpr double kExtrapolate = 100.0;
    // ... but never further than this from the newest update.
    static constexpr float kExtrapolateMax = 200.f;
    // The late update then puts the object where it should be; it is eased there over this long, never snapped,
    // unless it is further than kCorrectMax.
    static constexpr double kCorrectFor = 100.0;
    static constexpr float kCorrectMax = 64.f;

    void Add(const double aArrival, const Pose& acPose, const bool aAtRest) noexcept
    {
        // Out of order: the line started again, and what was eased out on the old one means nothing on it.
        if (m_count && aArrival < m_updates[m_count - 1].Arrival)
            Clear();
        // After a pause it goes on from where it stopped instead of jumping to the new place.
        if (m_count && aArrival - m_updates[m_count - 1].Arrival > kPause)
        {
            Update last = m_updates[m_count - 1];
            last.Arrival = aArrival - kMinDelay;
            m_updates[0] = last;
            m_count = 1;
        }
        if (m_count == kUpdates)
        {
            std::move(m_updates.begin() + 1, m_updates.end(), m_updates.begin());
            --m_count;
        }
        m_updates[m_count++] = {aArrival, acPose, aAtRest};

        // An update that arrives while the object is carried on past the last one moves where it should have been.
        // The difference is eased out from what was drawn, so the late update never shows as a snap. It is measured
        // against what Show now draws at that moment from the new updates (carried on again if this one is late too),
        // so the path and the correction never count the same distance twice.
        if (m_extrapolating)
        {
            bool extrapolating = false;
            const Pose cNow = Sample(m_shownMoment, extrapolating);
            const glm::vec3 cError = m_drawn.Position - cNow.Position;
            const float cLength = glm::length(cError);
            m_error = std::isfinite(cLength) && cLength <= kCorrectMax ? cError : glm::vec3(0.f);
            m_errorSince = m_shownMoment;
            m_extrapolating = false;
        }
    }

    void Clear() noexcept
    {
        m_count = 0;
        m_extrapolating = false;
        m_error = glm::vec3(0.f);
        m_errorSince = -1e12;
    }

    //! The newest update, where it ends up.
    [[nodiscard]] Pose Newest() const noexcept { return m_count ? m_updates[m_count - 1].Value : Pose{}; }
    [[nodiscard]] bool IsEmpty() const noexcept { return m_count == 0; }
    [[nodiscard]] size_t GetCount() const noexcept { return m_count; }

    // Long enough behind for the next update to have arrived, from the longest recent gap between two of them.
    [[nodiscard]] double GetDelay() const noexcept
    {
        double gap = 0.0;
        for (size_t i = 1; i < m_count; ++i)
            gap = std::max(gap, m_updates[i].Arrival - m_updates[i - 1].Arrival);
        return std::clamp(gap + kMargin, kMinDelay, kMaxDelay);
    }

    // Where it is at aNow shown aDelay behind: between the two updates around that moment, or the nearest end.
    [[nodiscard]] Pose At(const double aNow, const double aDelay) const noexcept
    {
        if (!m_count)
            return {};
        const double cShown = aNow - aDelay;
        if (cShown <= m_updates[0].Arrival)
            return m_updates[0].Value;
        for (size_t i = 1; i < m_count; ++i)
        {
            const Update& cTo = m_updates[i];
            if (cShown >= cTo.Arrival)
                continue;
            const Update& cFrom = m_updates[i - 1];
            const double cSpan = cTo.Arrival - cFrom.Arrival;
            const float cT = cSpan > 0.0 ? static_cast<float>((cShown - cFrom.Arrival) / cSpan) : 1.f;
            return Blend(cFrom.Value, cTo.Value, cT);
        }
        return m_updates[m_count - 1].Value;
    }

    // Like At, for the moment aNow - aDelay, with two differences that keep it moving when the network does not:
    // - past the newest update that did not put the object down it goes on at the speed of the last two, for kExtrapolate;
    // - what a late update then corrects is eased out over kCorrectFor, not jumped.
    // Called once per frame, with the shown moment moving forward.
    Pose Show(const double aNow, const double aDelay) noexcept
    {
        const double cShown = aNow - aDelay;
        bool extrapolating = false;
        Pose pose = Sample(cShown, extrapolating);
        m_extrapolating = extrapolating;
        m_shownMoment = cShown;

        // The correction left by the last late update fades out; what is drawn includes it.
        const double cFaded = cShown - m_errorSince;
        if (cFaded >= 0.0 && cFaded < kCorrectFor)
            pose.Position += m_error * static_cast<float>(1.0 - cFaded / kCorrectFor);
        m_drawn = pose;
        return pose;
    }

    // The shown moment has reached the newest update, and that one put the object down.
    [[nodiscard]] bool IsRestShown(const double aNow, const double aDelay) const noexcept
    {
        return m_count && m_updates[m_count - 1].AtRest && aNow - aDelay >= m_updates[m_count - 1].Arrival;
    }

private:
    struct Update
    {
        double Arrival{};
        Pose Value{};
        bool AtRest{};
    };

    // What Show draws at the moment aShown before any correction: At, carried on past the newest update as Show says.
    // aExtrapolating says whether it was carried on.
    [[nodiscard]] Pose Sample(const double aShown, bool& aExtrapolating) const noexcept
    {
        Pose pose = At(aShown, 0.0);
        aExtrapolating = false;
        if (m_count >= 2 && !m_updates[m_count - 1].AtRest)
        {
            const Update& cNewest = m_updates[m_count - 1];
            const double cSpan = cNewest.Arrival - m_updates[m_count - 2].Arrival;
            const double cExcess = aShown - cNewest.Arrival;
            if (cExcess > 0.0 && cSpan >= 1.0 && cSpan <= kPause)
            {
                glm::vec3 step = (cNewest.Value.Position - m_updates[m_count - 2].Value.Position) * static_cast<float>(std::min(cExcess, kExtrapolate) / cSpan);
                const float cLength = glm::length(step);
                if (cLength > kExtrapolateMax)
                    step *= kExtrapolateMax / cLength;
                if (std::isfinite(cLength))
                {
                    pose.Position = cNewest.Value.Position + step;
                    aExtrapolating = true;
                }
            }
        }
        return pose;
    }

    std::array<Update, kUpdates> m_updates{};
    size_t m_count{0};

    // What Show drew last, to ease out what a late update changes.
    Pose m_drawn{};
    bool m_extrapolating{false};
    double m_shownMoment{0.0};
    glm::vec3 m_error{0.f};
    double m_errorSince{-1e12};
};

// The newest sender tick taken for an object. Moves travel unreliable and can arrive after the put-down sent after them:
// an update older than one already taken is dropped, so a late move never undoes a rest.
// An update without a tick (a resent resting place, a take) always counts and leaves the order as it was.
class Order
{
public:
    bool Take(const uint64_t aTick) noexcept
    {
        if (!aTick)
            return true;
        if (aTick < m_newest)
            return false;
        m_newest = aTick;
        return true;
    }

private:
    uint64_t m_newest{0};
};

// Which line of time an object's updates are put on, and which moment of it to show.
// An update that carries the sender's tick goes at that tick: the network's delays and jitter then do not bend the speed
// of a throw, as they would if each update went at the time it arrived. The moment shown is the one a PlaybackClock picks
// from the ticks and arrivals, a little behind (at least as far as the other player's copy, so a carried object stays
// with its carrier), the same way the other players' movement is shown. Updates without a tick (a resent resting place)
// go at the time they arrived, taken to the line by what the clock knows of the network's delay.
// Without a tick ever, everything stays on arrival times.
class Timeline
{
public:
    // The time to Add an update at, for the tick it came with (0: none) and when it arrived here (ms).
    double Note(const double aArrival, const uint64_t aTick) noexcept
    {
        if (aTick)
        {
            m_clock.AddSample(aArrival, aTick);
            m_timed = true;
            return static_cast<double>(aTick);
        }
        return m_timed && m_clock.IsRunning() ? aArrival - m_clock.GetMinTransit() : aArrival;
    }

    // Updates before the first tick are on arrival times and cannot be mixed with ticks.
    [[nodiscard]] bool IsTimed() const noexcept { return m_timed && m_clock.IsRunning(); }

    // The moment on the line to show at aNow, at least aMinDelay behind what has arrived. Once per frame.
    double Moment(const double aNow, const double aMinDelay) noexcept
    {
        m_clock.SetMinTargetDelay(aMinDelay);
        m_clock.Advance(aNow);
        return m_clock.GetRenderTick();
    }

    [[nodiscard]] double GetTargetDelay() const noexcept { return m_clock.GetTargetDelay(); }

private:
    PlaybackClock m_clock{};
    bool m_timed{false};
};

// How far behind an object is shown, eased towards the wanted delay at most a tenth as fast as time passes (as
// PlaybackClock does), so a change of delay never makes the object skip ahead or step back.
class Delay
{
public:
    static constexpr double kMaxRate = 0.1;

    double Follow(const double aNow, const double aWanted) noexcept
    {
        if (!m_started)
        {
            m_started = true;
            m_delay = aWanted;
        }
        else
        {
            const double cStep = kMaxRate * std::max(0.0, aNow - m_lastTime);
            m_delay += std::clamp(aWanted - m_delay, -cStep, cStep);
        }
        m_lastTime = aNow;
        return m_delay;
    }

private:
    bool m_started{false};
    double m_delay{0.0};
    double m_lastTime{0.0};
};

// Whether the object sits in a hand of the mover's copy, and then where that hand holds it.
// It is taken as held once it keeps one place against the hand for a moment; from then it is drawn at the hand, so
// it moves with the hand this player sees rather than a little ahead of or behind it. Updates that move it away from
// the hand (a throw, a drop, another hand) let it go again, blended back onto them. The blend starts out moving the way
// the hand last carried it, so a throw leaves the hand at the hand's speed rather than stopping dead for a moment.
class Grip
{
public:
    // Further from the hand than this, the object is not in it.
    static constexpr float kReach = 40.f;
    // Held once it stays this still against the hand for kSteadyFor ms.
    static constexpr float kSteadyDistance = 2.5f;
    static constexpr float kSteadyAngle = 0.15f;
    static constexpr double kSteadyFor = 200.0;
    // Let go when the updates are this far from where the hand holds it for kApartFor ms in a row. By time, not by frames:
    // three frames are 33 ms at 90 fps and 100 ms at 30, and the let-go should not depend on the frame rate.
    static constexpr float kApartDistance = 12.f;
    static constexpr double kApartFor = 100.0;
    // From the hand back onto the updates in this many ms, so letting go does not jump.
    static constexpr double kBlendFor = 150.0;
    // The hand's speed is eased over about this long, so one jittery read does not fling the object.
    static constexpr double kVelocityEaseMs = 50.0;
    // Reads further apart than this say nothing of the hand's speed.
    static constexpr double kMaxVelocityGap = 100.0;
    // A let-go never carries the object faster than this (units a second), as a throw: a hard throw, not a hitch.
    static constexpr float kMaxReleaseSpeed = kMaxThrowSpeed;

    // aHandKey names the hand apHand is (0 or a null apHand: no hand near). acPath is where the updates put the
    // object now. Returns where to draw it.
    Pose Update(const double aNow, const uint64_t aHandKey, const Pose* apHand, const Pose& acPath) noexcept
    {
        if (m_held)
        {
            // A hand missing for a frame (one it was not posed in) is no more a let-go than one moment apart is.
            const bool cHandHere = apHand && aHandKey == m_handKey && IsFinite(*apHand);
            const Pose cDrawn = cHandHere ? Apply(*apHand, m_grip) : m_lastHeld;
            const bool cApart = !cHandHere || !(glm::distance(cDrawn.Position, acPath.Position) <= kApartDistance);
            if (!cApart)
                m_apartSince = -1.0;
            else if (m_apartSince < 0.0)
                m_apartSince = aNow;
            if (!cApart || aNow - m_apartSince < kApartFor)
            {
                if (cHandHere)
                {
                    NoteHeldMotion(aNow, cDrawn.Position);
                    m_lastHeld = cDrawn;
                }
                return Show(aNow, cDrawn);
            }
            LetGo(aNow);
        }

        if (!apHand || !aHandKey || !IsFinite(*apHand))
        {
            m_steadySince = -1.0;
            return Show(aNow, acPath);
        }

        const Relative cNow = RelativeTo(*apHand, acPath);
        const bool cNear = glm::length(cNow.Offset) <= kReach;
        const bool cSteady = cNear && m_steadySince >= 0.0 && aHandKey == m_steadyKey && glm::distance(cNow.Offset, m_steadyAt.Offset) <= kSteadyDistance &&
                             AngleBetween(cNow.Turn, m_steadyAt.Turn) <= kSteadyAngle;
        if (!cSteady)
        {
            m_steadyKey = aHandKey;
            m_steadyAt = cNow;
            m_steadySince = cNear ? aNow : -1.0;
        }
        else if (aNow - m_steadySince >= kSteadyFor)
        {
            // Held where it is now, so taking hold of it does not move it.
            m_held = true;
            m_handKey = aHandKey;
            m_grip = cNow;
            m_apartSince = -1.0;
            m_lastHeld = Apply(*apHand, m_grip);
            m_lastHeldAt = aNow;
            m_heldVelocity = {};
            return Show(aNow, m_lastHeld);
        }
        return Show(aNow, acPath);
    }

    // Put down or no longer moved: let go now.
    void LetGo(const double aNow) noexcept
    {
        if (m_held && m_hasShown)
        {
            m_blendFrom = m_shown;
            m_blendSince = aNow;
            m_blendVelocity = ReleaseVelocity(aNow);
        }
        m_held = false;
        m_handKey = 0;
        m_apartSince = -1.0;
        m_steadySince = -1.0;
    }

    [[nodiscard]] bool IsHeld() const noexcept { return m_held; }
    [[nodiscard]] uint64_t GetHandKey() const noexcept { return m_handKey; }
    [[nodiscard]] float GetGripDistance() const noexcept { return m_held ? glm::length(m_grip.Offset) : 0.f; }
    // How fast the hand holding it moves (units a second), eased; zero when not held.
    [[nodiscard]] glm::vec3 GetHeldVelocity() const noexcept { return m_held ? m_heldVelocity : glm::vec3{}; }

private:
    struct Relative
    {
        glm::vec3 Offset{};
        glm::quat Turn{1.f, 0.f, 0.f, 0.f};
    };

    static Relative RelativeTo(const Pose& acHand, const Pose& acObject) noexcept
    {
        const glm::quat cInverse = glm::inverse(glm::normalize(acHand.Rotation));
        return {cInverse * (acObject.Position - acHand.Position), glm::normalize(cInverse * acObject.Rotation)};
    }

    static Pose Apply(const Pose& acHand, const Relative& acGrip) noexcept
    {
        const glm::quat cHand = glm::normalize(acHand.Rotation);
        return {acHand.Position + cHand * acGrip.Offset, glm::normalize(cHand * acGrip.Turn)};
    }

    // The hand's speed from where it held the object last time and now.
    void NoteHeldMotion(const double aNow, const glm::vec3& acHeldAt) noexcept
    {
        const double cGap = aNow - m_lastHeldAt;
        if (m_lastHeldAt < 0.0 || !(cGap > 0.0) || cGap > kMaxVelocityGap)
        {
            if (!(cGap >= 0.0) || cGap > kMaxVelocityGap)
                m_heldVelocity = {};
            m_lastHeldAt = aNow;
            return;
        }
        const glm::vec3 cSample = (acHeldAt - m_lastHeld.Position) * static_cast<float>(1000.0 / cGap);
        if (std::isfinite(cSample.x) && std::isfinite(cSample.y) && std::isfinite(cSample.z))
            m_heldVelocity += (cSample - m_heldVelocity) * static_cast<float>(1.0 - std::exp(-cGap / kVelocityEaseMs));
        m_lastHeldAt = aNow;
    }

    // What a let-go at aNow carries the object on with: the hand's last speed, capped, or none when the hand was not read lately.
    [[nodiscard]] glm::vec3 ReleaseVelocity(const double aNow) const noexcept
    {
        if (m_lastHeldAt < 0.0 || aNow - m_lastHeldAt > kMaxVelocityGap)
            return {};
        return CapSpeed(m_heldVelocity, kMaxReleaseSpeed);
    }

    Pose Show(const double aNow, const Pose& acTarget) noexcept
    {
        const double cBlended = aNow - m_blendSince;
        if (cBlended >= 0.0 && cBlended < kBlendFor)
        {
            // Blended from where the hand would have carried it by now.
            Pose from = m_blendFrom;
            from.Position += m_blendVelocity * static_cast<float>(cBlended / 1000.0);
            m_shown = Blend(from, acTarget, static_cast<float>(cBlended / kBlendFor));
        }
        else
            m_shown = acTarget;
        m_hasShown = true;
        return m_shown;
    }

    bool m_held{false};
    uint64_t m_handKey{0};
    Relative m_grip{};
    double m_apartSince{-1.0};
    Pose m_lastHeld{};
    double m_lastHeldAt{-1.0};
    glm::vec3 m_heldVelocity{};

    uint64_t m_steadyKey{0};
    Relative m_steadyAt{};
    double m_steadySince{-1.0};

    Pose m_shown{};
    bool m_hasShown{false};
    Pose m_blendFrom{};
    glm::vec3 m_blendVelocity{};
    double m_blendSince{-1e9};
};

// A hand of the mover's copy is read from its 3D, which holds where the hand was put in the last frame's pass; the object
// drawn from it is shown in this one, so it would trail the hand by a frame. This puts the hand a frame ahead, by the step
// the hand took between the last two reads, capped so a hitch or a jittery read never throws the object far.
class HandLead
{
public:
    // The lead never exceeds this: about 6 units a frame is 3 m/s at 60 fps, a quick swing of the arm.
    static constexpr float kMaxLead = 6.f;
    static constexpr float kMaxTurn = 0.12f;
    // Reads further apart than this (a hitch, a hand not posed in between) start over.
    static constexpr double kMaxGap = 50.0;

    // aRead is the hand as the game has it now, at aNow ms. Returns where the hand will be when this frame is drawn.
    Pose Lead(const double aNow, const Pose& acRead) noexcept
    {
        Pose result = acRead;
        if (m_has && aNow > m_at && aNow - m_at <= kMaxGap)
        {
            glm::vec3 step = acRead.Position - m_read.Position;
            const float cLength = glm::length(step);
            if (cLength > kMaxLead)
                step *= kMaxLead / cLength;
            result.Position = acRead.Position + step;

            const glm::quat cTurn = glm::normalize(acRead.Rotation * glm::inverse(glm::normalize(m_read.Rotation)));
            const float cAngle = AngleBetween(cTurn, glm::quat(1.f, 0.f, 0.f, 0.f));
            if (cAngle > 1e-5f)
                result.Rotation = glm::normalize(Slerp(acRead.Rotation, cTurn * acRead.Rotation, std::min(1.f, kMaxTurn / cAngle)));
        }
        m_read = acRead;
        m_at = aNow;
        m_has = true;
        return result;
    }

    // The last time a hand was read, to forget a hand nobody asks about.
    [[nodiscard]] double GetLastRead() const noexcept { return m_at; }

private:
    Pose m_read{};
    double m_at{0.0};
    bool m_has{false};
};

// A throw: a hand of the moving player's lets go of an object, which then flies by itself. From the let-go on, the mover's game
// sends its moves as thrown, with how fast it flies and spins, and this game hands the object to its own physics with that speed
// once the let-go is shown here, so it keeps its arc instead of being drawn update by update. The mover's rest place settles it.

//! How an object flies: its speed (units a second) and spin (radians a second, about the world's axes).
struct Flight
{
    glm::vec3 Velocity{};
    glm::vec3 Spin{};
};

//! A flight within those bounds, or none of what is not a number.
[[nodiscard]] inline Flight Capped(const Flight& acFlight) noexcept
{
    return {CapSpeed(acFlight.Velocity, kMaxThrowSpeed), CapSpeed(acFlight.Spin, kMaxThrowSpin)};
}

//! Polls closer together than this, or further apart, tell nothing of how fast it flies.
inline constexpr double kMinPollGap = 10.0;
inline constexpr double kMaxPollGap = 100.0;

//! How an object flies from two polls of it aGapMs apart, for when its body's own speed cannot be read. None when the polls
//! are too close together or too far apart to tell.
[[nodiscard]] inline Flight FlightBetween(const Pose& acFrom, const Pose& acTo, const double aGapMs) noexcept
{
    if (!(aGapMs >= kMinPollGap) || aGapMs > kMaxPollGap || !IsFinite(acFrom) || !IsFinite(acTo))
        return {};
    const auto cSeconds = static_cast<float>(aGapMs / 1000.0);
    return Capped({WorldObjectBodies::VelocityBetween(acFrom.Position, acTo.Position, cSeconds), WorldObjectBodies::SpinBetween(acFrom.Rotation, acTo.Rotation, cSeconds)});
}

// The mover's side, in VR: whether a hand of this player's carries an object. Near a hand is not enough (a hand reaching past a
// plate on a table): it is carried once it has moved with the hand for a few polls in a row.
class Carry
{
public:
    //! Moved with the hand this many polls in a row: carried.
    static constexpr uint32_t kTogetherPolls = 2;
    //! Carried, it is out of the hand only beyond this: a long object held by its far end, turned in the hand, comes and goes
    //! around Grip::kReach.
    static constexpr float kLetGoReach = 60.f;

    //! A hand as read this poll, with how far it moved since the poll before (none when it was not read then).
    struct Hand
    {
        glm::vec3 Position{};
        glm::vec3 Step{};
        bool Has{};
    };
    using Hands = std::array<Hand, 2>;

    //! A poll of the object at acPosition, with this player's hands. Returns whether a hand carries it now.
    bool Poll(const Hands& acHands, const glm::vec3& acPosition) noexcept
    {
        const bool cHasStep = m_hasLast;
        const glm::vec3 cStep = acPosition - m_last;
        m_last = acPosition;
        m_hasLast = true;
        for (size_t side = 0; side < acHands.size(); ++side)
        {
            const Hand& cHand = acHands[side];
            const float cDistance = cHand.Has ? glm::distance(cHand.Position, acPosition) : std::numeric_limits<float>::infinity();
            if (m_held[side])
            {
                m_held[side] = cDistance <= kLetGoReach;
                continue;
            }
            const bool cNear = cDistance <= Grip::kReach;
            m_together[side] = cNear && cHasStep && HandMotion::MovesWith(cHand.Step, cStep) ? m_together[side] + 1 : 0;
            if (m_together[side] >= kTogetherPolls)
            {
                m_held[side] = true;
                m_together[side] = 0;
            }
        }
        return IsHeld();
    }

    [[nodiscard]] bool IsHeld() const noexcept { return m_held[0] || m_held[1]; }

private:
    glm::vec3 m_last{};
    bool m_hasLast{false};
    std::array<uint32_t, 2> m_together{};
    std::array<bool, 2> m_held{};
};

// The mover's side: whether an object one of this player's hands held a moment ago now flies by itself. One that was never held
// (pushed, knocked, a blast) is not thrown, and is drawn update by update as before; neither is one at rest.
class Release
{
public:
    //! Held this lately and free now: let go.
    static constexpr double kHeldWithin = 300.0;
    //! Free in this many polls in a row: one poll without the hand on it is not a let-go.
    static constexpr uint32_t kFreePolls = 2;

    //! A poll of the object at aNow (ms), with whether a hand of this player holds it and whether it moves. Returns whether it is
    //! thrown now.
    bool Poll(const double aNow, const bool aHeld, const bool aMoving) noexcept
    {
        if (aHeld)
        {
            m_heldAt = aNow;
            m_freePolls = 0;
            m_thrown = false;
            return false;
        }
        // Out of the hand and still: nothing flies.
        if (!aMoving)
        {
            Rest();
            return false;
        }
        if (m_thrown || m_heldAt < 0.0)
            return m_thrown;
        if (++m_freePolls < kFreePolls)
            return false;
        m_thrown = aNow - m_heldAt <= kHeldWithin;
        // Let go too long ago to call it a throw: it was pushed since.
        if (!m_thrown)
            m_heldAt = -1.0;
        return m_thrown;
    }

    [[nodiscard]] bool IsThrown() const noexcept { return m_thrown; }

    //! Put down, or taken by another player: the next throw starts from a new hold.
    void Rest() noexcept { *this = {}; }

private:
    double m_heldAt{-1.0};
    uint32_t m_freePolls{0};
    bool m_thrown{false};
};

// This game's side: when the moment shown reaches the first thrown update, the object is handed to physics here with that
// update's flight; the thrown updates after it are not drawn. An update not thrown (a hand took it again, or it is put down)
// takes it back, to be drawn from where it is. So does a flight that goes on too long or too far without a rest (rolling down
// a slope, floating down a river): it is drawn from the updates until the next throw.
class Launch
{
public:
    //! The longest a flight here goes uncorrected (ms on the line), and the furthest from where it was let go (units).
    static constexpr double kMaxFlightFor = 3000.0;
    static constexpr float kMaxFlightDistance = 2500.f;

    enum class Step : uint8_t
    {
        Follow,  //!< drawn as the updates say
        Skip,    //!< flies here: not drawn
        TakeBack //!< flew here: taken back from where it is, then drawn
    };

    //! An update put at aTime on the line, at acPosition, thrown or not. Returns what becomes of it.
    Step Note(const double aTime, const bool aThrown, const Flight& acFlight, const bool aAtRest, const glm::vec3& acPosition) noexcept
    {
        const bool cThrown = aThrown && !aAtRest;
        if (m_flying)
        {
            const bool cSpent = aTime - m_at > kMaxFlightFor || !(glm::distance(acPosition, m_from) <= kMaxFlightDistance);
            if (cThrown && !cSpent)
                return Step::Skip;
            m_flying = false;
            m_spent = cThrown;
            return Step::TakeBack;
        }
        if (!cThrown)
        {
            m_pending = false;
            m_spent = false;
        }
        // The first thrown update that arrives is the let-go: one lost on the way leaves the next.
        else if (!m_pending && !m_spent)
        {
            m_pending = true;
            m_at = aTime;
            m_from = acPosition;
            m_flight = Capped(acFlight);
        }
        return Step::Follow;
    }

    //! Once a frame, with the moment shown: whether it is handed to physics now.
    bool Due(const double aShown) noexcept
    {
        if (!m_pending || aShown < m_at)
            return false;
        m_pending = false;
        m_flying = true;
        return true;
    }

    [[nodiscard]] bool IsPending() const noexcept { return m_pending; }
    [[nodiscard]] bool IsFlying() const noexcept { return m_flying; }
    [[nodiscard]] const Flight& GetFlight() const noexcept { return m_flight; }

private:
    bool m_pending{false};
    bool m_flying{false};
    //! Its flight here ran out: the thrown updates left of it are drawn.
    bool m_spent{false};
    double m_at{0.0};
    glm::vec3 m_from{};
    Flight m_flight{};
};

// A catch: a hand of this player's takes an object another player threw while it flies here by this game's physics. This game
// then moves it and sends its moves (the server keeps one mover), and the thrower's later moves of it, its rest place say, do
// not take it out of the hand while it is held here.
class CatchHold
{
public:
    //! A poll of the object: whether it flies here after another player's throw, and whether a hand of this player holds it.
    //! Returns whether it is caught now.
    bool Poll(const bool aFlyingHere, const bool aHeldHere) noexcept
    {
        if (!aHeldHere)
        {
            m_caught = false;
            return false;
        }
        if (m_caught || !aFlyingHere)
            return false;
        m_caught = true;
        return true;
    }

    //! Caught and still held here: another player's moves of it are not followed.
    [[nodiscard]] bool IsCaught() const noexcept { return m_caught; }

private:
    bool m_caught{false};
};
} // namespace HeldObject
