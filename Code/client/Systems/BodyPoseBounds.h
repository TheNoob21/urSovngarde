#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

// World bounds for the SE body commit: the commit writes bone worlds after the native update, which left each object's
// world bound around the native pose. A union with a sphere around the written bones only grows a bound, so a stored
// union can never cull more than the native bound did (over-draw, never cull).
namespace BodyPoseBounds
{
// NiAVObject::worldBound, an NiBound (centre x y z, radius; 16 bytes), at +E4 on SE; the flags follow at +F4.
// Source: CommonLibSSE-NG include/RE/N/NiAVObject.h ("NiBound worldBound; // 0E4", flags "// 0F4" outside VR) and
// include/RE/N/NiBound.h (centre +00, radius +0C, size 0x10).
inline constexpr size_t kNodeWorldBound = 0xE4, kBoundBytes = 16;
// Skin reaches past the written bone origins (fingertips past the last finger bone, the palm, the limbs' thickness).
inline constexpr float kWrittenPaddingUnits = 20.f;
using Sphere = std::array<float, 4>; // x y z radius, the NiBound byte layout
static_assert(sizeof(Sphere) == kBoundBytes);

inline bool Usable(const Sphere& acSphere) noexcept
{
    return std::all_of(acSphere.begin(), acSphere.end(), [](float aValue) { return std::isfinite(aValue); }) && acSphere[3] > 0.f;
}
inline bool SameBits(const Sphere& acLeft, const Sphere& acRight) noexcept
{
    return std::memcmp(acLeft.data(), acRight.data(), sizeof(Sphere)) == 0;
}

// The box of a set of points; its sphere (centre the box middle, radius half the diagonal) holds every point. Not minimal.
struct PointBox
{
    std::array<double, 3> Low{}, High{};
    size_t Count{};
    bool Finite = true;
    void Add(float aX, float aY, float aZ) noexcept
    {
        const std::array<double, 3> p{aX, aY, aZ};
        for (size_t k = 0; k < 3; ++k)
        {
            Finite = Finite && std::isfinite(p[k]);
            Low[k] = Count ? std::min(Low[k], p[k]) : p[k];
            High[k] = Count ? std::max(High[k], p[k]) : p[k];
        }
        ++Count;
    }
    // False: no point, a non-finite point or a non-finite result (nothing to store).
    bool Enclosing(float aPadding, Sphere& aOutput) const noexcept
    {
        if (!Count || !Finite || !std::isfinite(aPadding) || aPadding < 0.f)
            return false;
        double half{};
        for (size_t k = 0; k < 3; ++k)
        {
            aOutput[k] = static_cast<float>((Low[k] + High[k]) * 0.5);
            half += (High[k] - Low[k]) * (High[k] - Low[k]);
        }
        // float centre rounding moves it by under 1e-7 of its size; the slack keeps every point inside after it.
        const double centre = std::max({std::abs(Low[0]), std::abs(High[0]), std::abs(Low[1]), std::abs(High[1]), std::abs(Low[2]), std::abs(High[2])});
        aOutput[3] = static_cast<float>(std::sqrt(half) * 0.5 + aPadding + 1e-3 + 1e-6 * centre);
        return Usable(aOutput);
    }
};

// The smallest sphere holding both, rounded outward so the stored floats still hold both. One inside the other: that
// one, bit for bit. False: an input or the result is not usable (non-finite or radius not positive).
inline bool Union(const Sphere& acLeft, const Sphere& acRight, Sphere& aOutput) noexcept
{
    if (!Usable(acLeft) || !Usable(acRight))
        return false;
    const double dx = static_cast<double>(acRight[0]) - acLeft[0], dy = static_cast<double>(acRight[1]) - acLeft[1], dz = static_cast<double>(acRight[2]) - acLeft[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz), ra = acLeft[3], rb = acRight[3];
    if (d + rb <= ra)
    {
        aOutput = acLeft;
        return true;
    }
    if (d + ra <= rb)
    {
        aOutput = acRight;
        return true;
    }
    const double r = (d + ra + rb) * 0.5, t = (r - ra) / d; // d > 0 here: equal centres take one of the branches above
    const double x = acLeft[0] + dx * t, y = acLeft[1] + dy * t, z = acLeft[2] + dz * t;
    const double size = std::max({std::abs(x), std::abs(y), std::abs(z)}) + r;
    aOutput = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(r + 1e-3 + 1e-6 * size)};
    return Usable(aOutput);
}
// True when outer holds inner (centre distance + inner radius within outer radius, both in double).
inline bool Contains(const Sphere& acOuter, const Sphere& acInner) noexcept
{
    const double dx = static_cast<double>(acInner[0]) - acOuter[0], dy = static_cast<double>(acInner[1]) - acOuter[1], dz = static_cast<double>(acInner[2]) - acOuter[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz) + acInner[3] <= static_cast<double>(acOuter[3]);
}
} // namespace BodyPoseBounds
