#pragma once

#include <cmath>
#include <TiltedCore/Buffer.hpp>
#include <Structs/VRPose.h>

// Values only: the flat update calls heading then pitch, on whichever thread updates that spell.
namespace SpellAimRules
{
inline constexpr double kPairLastsMs = 25.0;
inline constexpr double kAimLastsMs = 400.0;

inline bool Finite(float aHeading, float aPitch) noexcept
{
    return std::isfinite(aHeading) && std::isfinite(aPitch);
}

inline bool Recent(double aAt, double aNow, double aLasts) noexcept
{
    return std::isfinite(aAt) && std::isfinite(aNow) && aNow >= aAt && aNow - aAt <= aLasts;
}

// Only this player's own cast, with a server to send it to, is noted.
inline bool CanNote(bool aLocalPlayer, bool aConnected) noexcept
{
    return aLocalPlayer && aConnected;
}

inline bool NeedsBodyPresentation(bool aNoBones, bool aHasRootPosition) noexcept
{
    return !aNoBones || aHasRootPosition;
}

class Pair
{
public:
    void Heading(bool aLocalPlayer, float aHeading, double aNow) noexcept
    {
        m_valid = aLocalPlayer && std::isfinite(aHeading) && std::isfinite(aNow);
        m_heading = aHeading;
        m_at = aNow;
    }

    bool Pitch(bool aLocalPlayer, float aPitch, double aNow, float& aHeading) noexcept
    {
        const bool valid = m_valid;
        m_valid = false; // a missing heading in the next update must not reuse this one
        if (!valid || !aLocalPlayer || !std::isfinite(aPitch) || !Recent(m_at, aNow, kPairLastsMs))
            return false;
        aHeading = m_heading;
        return true;
    }

private:
    bool m_valid{};
    float m_heading{};
    double m_at{};
};

class LocalAim
{
public:
    void Note(float aHeading, float aPitch, double aNow) noexcept
    {
        if (!Finite(aHeading, aPitch) || !std::isfinite(aNow))
            return;
        m_heading = aHeading;
        m_pitch = aPitch;
        m_at = aNow;
        m_valid = true;
    }

    void Fill(VRPose& aPose, double aNow) const noexcept
    {
        aPose.HasSpellAim = m_valid && Recent(m_at, aNow, kAimLastsMs);
        aPose.SpellAim[0] = aPose.HasSpellAim ? m_heading : 0.f;
        aPose.SpellAim[1] = aPose.HasSpellAim ? m_pitch : 0.f;
    }

    VRPose AimOnly(double aNow) const noexcept
    {
        VRPose pose{};
        pose.HasData = true;
        pose.NoBones = true;
        Fill(pose, aNow);
        return pose;
    }

    // A new session starts with no aim and nothing pending to clear.
    void Reset() noexcept { *this = LocalAim{}; }

    // The aim-only snapshot, only while a spell is held and once more when it ends, so that the others drop its aim.
    bool Capture(VRPose& aPose, double aNow) noexcept
    {
        aPose = AimOnly(aNow);
        const bool send = aPose.HasSpellAim || m_sent;
        m_sent = aPose.HasSpellAim;
        return send;
    }

private:
    bool m_sent{};
    bool m_valid{};
    float m_heading{};
    float m_pitch{};
    double m_at{};
};
} // namespace SpellAimRules
