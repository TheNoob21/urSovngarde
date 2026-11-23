#pragma once

#include <chrono>
#include <cstdint>

// Whether this VR player's body is fitted to the headset and controllers by a body plugin (VRIK).
//
// The pose sent to the other games is the game's own body as this game holds it: its bone rotations and where its hands are.
// VRIK bends that body to the headset and controllers. Without it the game still keeps the body's empty hands near the
// controllers, but from the moment a weapon or spell is drawn its own animation holds both arms up by the head. So without VRIK
// the pose is sent only while nothing is drawn; from the draw until the weapon is away again the copy plays its own drawn stance,
// as when the bones cannot be read.
//
// Pure: plain values in and out, tested in tests/body_solver.cpp.
namespace BodySolver
{
using Clock = std::chrono::steady_clock;

//! How often the loaded plugins are asked again. VRIK loads with the game, so the answer rarely changes.
inline constexpr auto kAskEvery = std::chrono::seconds(5);

//! Whether the body pose is sent: always with VRIK, and without it only while the weapon state (bits 5-7 of the actor's flags2:
//! 0 sheathed, 1 want to draw, 2 drawing, 3 drawn, 4 want to sheathe, 5 sheathing) is sheathed.
[[nodiscard]] constexpr bool SendsPose(const bool aSolverLoaded, const uint32_t aWeaponState) noexcept
{
    return aSolverLoaded || aWeaponState == 0;
}

//! The last answer, asked again at most every kAskEvery.
class Watch
{
public:
    //! What a new answer means for the log: said once when the plugin is missing at first, and at every change after.
    enum class Said
    {
        kNothing,
        kChanged
    };

    [[nodiscard]] bool Due(const Clock::time_point aNow) const noexcept { return !m_asked || aNow >= m_next; }

    Said Take(const bool aLoaded, const Clock::time_point aNow) noexcept
    {
        const bool cSay = m_asked ? aLoaded != m_loaded : !aLoaded;
        m_asked = true;
        m_loaded = aLoaded;
        m_next = aNow + kAskEvery;
        return cSay ? Said::kChanged : Said::kNothing;
    }

    [[nodiscard]] bool Loaded() const noexcept { return m_loaded; }

private:
    bool m_asked{};
    bool m_loaded{};
    Clock::time_point m_next{};
};
} // namespace BodySolver
