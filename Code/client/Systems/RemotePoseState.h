#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <Structs/VRPose.h>

// Receive-thread/main-thread handoff values only. No actor, node or renderer pointers belong here.
namespace RemotePose
{
enum class Policy
{
    LiveVRPlayer,
    // CaptureBodyPose deliberately omits unchanged ragdolls. Their last pose is retained until reset/teardown.
    SparseBody
};

inline constexpr double kLiveTimeoutMs = 1000.0;

//! Whether a pose raises its copy's playback floor to the longer delay a tracked VR body needs. Only a pose with bones does:
//! a flat player's spell aim comes as a pose with no bones, and that player's movement keeps the shorter delay.
[[nodiscard]] inline bool RaisesBodyDelay(bool aHasData, bool aNoBones, Policy aPolicy) noexcept
{
    return aHasData && !aNoBones && aPolicy == Policy::LiveVRPlayer;
}

[[nodiscard]] inline bool SparseBodyEligible(bool aDead, bool aKnockRagdoll, bool aBleedingOut, bool aPlayer) noexcept
{
    return !aPlayer && !aBleedingOut && (aDead || aKnockRagdoll);
}

struct Provenance
{
    uint64_t Generation{};
    uint64_t SourceTick{};
    double ReceivedAtMs{}; // original local monotonic receive time, never the render/sample time
    bool Valid{};
};

struct Sample
{
    VRPose Pose{};
    Provenance Before{};
    Provenance After{};
    uint64_t Generation{};
};

[[nodiscard]] inline bool
IsFresh(const Provenance& acSource, const uint64_t aGeneration, const double aNowMs, const Policy aPolicy, const double aTimeoutMs = kLiveTimeoutMs) noexcept
{
    if (!acSource.Valid || !aGeneration || acSource.Generation != aGeneration || !std::isfinite(acSource.ReceivedAtMs) || !std::isfinite(aNowMs) || aNowMs < acSource.ReceivedAtMs)
        return false;
    return aPolicy == Policy::SparseBody || (std::isfinite(aTimeoutMs) && aTimeoutMs >= 0.0 && aNowMs - acSource.ReceivedAtMs <= aTimeoutMs);
}

struct Identity
{
    uint64_t Session{};
    uint64_t Owner{};
    uint64_t Entity{};

    bool operator==(const Identity& acOther) const noexcept { return Session == acOther.Session && Owner == acOther.Owner && Entity == acOther.Entity; }
};

// Mutate only with the interpolation component's owner. Publish Sample by value to native presentation.
// Call Accept only AFTER the movement point passes Clock.AddSample and positional rejection checks.
class State
{
public:
    struct Accepted
    {
        bool AcceptedPoint{};
        VRPose Pose{};
        Provenance Source{};
    };

    State() noexcept
        : m_generation(NextGeneration())
    {
    }

    [[nodiscard]] uint64_t Generation() const noexcept { return m_generation; }
    [[nodiscard]] const Identity& GetIdentity() const noexcept { return m_identity; }

    // Identity changes must originate from trusted session/ownership lifecycle, never an arriving stale packet.
    // The caller also clears TimePoints and PlaybackClock when this returns true.
    bool Bind(const Identity& acIdentity) noexcept
    {
        if (m_bound && m_identity == acIdentity)
            return false;
        m_identity = acIdentity;
        m_bound = true;
        Reset();
        return true;
    }

    // Teardown, skeleton/ownership replacement, or an explicitly authenticated source-clock epoch change.
    // A backward tick alone is NOT evidence of a new epoch. Drop queued old-owner/session messages externally.
    void Reset() noexcept
    {
        m_generation = NextGeneration();
        m_haveTick = false;
        m_highTick = 0;
        m_lastArrivalMs = 0.0;
        m_lastPose = {};
        m_sparse = {};
    }

    [[nodiscard]] Accepted
    Accept(const uint64_t aSourceTick, const double aReceivedAtMs, const VRPose& acWirePose, const Policy aPolicy, const double aTimeoutMs = kLiveTimeoutMs) noexcept
    {
        Accepted result{};
        // Keep a lifetime watermark independent of buffer pruning/PlaybackClock's finite duplicate window.
        if (!std::isfinite(aReceivedAtMs) || (m_haveTick && (aSourceTick <= m_highTick || aReceivedAtMs < m_lastArrivalMs)))
            return result;
        m_haveTick = true;
        m_highTick = aSourceTick;
        m_lastArrivalMs = aReceivedAtMs;
        result.AcceptedPoint = true;
        if (!acWirePose.HasData)
            return result; // missing tracking is not a new pose, nor an unconditional ragdoll reset

        if (!IsFresh(m_lastPose, m_generation, aReceivedAtMs, aPolicy, aTimeoutMs))
        {
            // A resumed live stream must also clear an adapter that did not render during the tracking gap.
            if (m_lastPose.Valid)
                m_generation = NextGeneration();
            m_sparse = {};
        }
        result.Pose = acWirePose;
        if (result.Pose.NoBones)
            result.Pose.HasLegs = false;
        // Only these fields have an explicitly sparse wire policy. Never carry bones/root/hips forward here.
        if (!result.Pose.HasFingers && m_sparse.HasFingers)
        {
            result.Pose.HasFingers = true;
            result.Pose.Fingers = m_sparse.Fingers;
        }
        if (!result.Pose.HasScale && m_sparse.HasScale)
        {
            result.Pose.HasScale = true;
            result.Pose.RootScale = m_sparse.RootScale;
        }
        if (!result.Pose.HasWeapons && m_sparse.HasWeapons)
        {
            result.Pose.HasWeapons = true;
            result.Pose.WeaponHeld = m_sparse.WeaponHeld;
            result.Pose.WeaponRotation = m_sparse.WeaponRotation;
            for (size_t side = 0; side < 2; ++side)
                std::copy(std::begin(m_sparse.WeaponOffset[side]), std::end(m_sparse.WeaponOffset[side]), std::begin(result.Pose.WeaponOffset[side]));
        }
        result.Source = {m_generation, aSourceTick, aReceivedAtMs, true};
        m_lastPose = result.Source;
        m_sparse = result.Pose;
        return result;
    }

private:
    static uint64_t NextGeneration() noexcept
    {
        // Distinguishes component destruction/recreation even when entity/owner/form identifiers are reused.
        static std::atomic<uint64_t> s_next{1};
        return s_next.fetch_add(1, std::memory_order_relaxed);
    }

    Identity m_identity{};
    bool m_bound{};
    uint64_t m_generation{};
    bool m_haveTick{};
    uint64_t m_highTick{};
    double m_lastArrivalMs{};
    Provenance m_lastPose{};
    VRPose m_sparse{};
};
} // namespace RemotePose
