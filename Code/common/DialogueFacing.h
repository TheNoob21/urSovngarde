#pragma once

// A player talking to an NPC is shown facing it in the other players' games: plain values, so the rule is unit-tested.
// The talking game sends the heading toward the one it talks to while its Dialogue Menu is open; the copy's own
// interpolation turns it there, and back to the body's own heading once the menu closes.

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <optional>

namespace DialogueFacing
{
// Farther than this, in game units, the speaker is not the one talked to face to face (a shout across a hall, a mod's
// remote dialogue), and the body keeps its own heading.
inline constexpr float kMaxDistance = 1500.f;

// Closer than this the direction means nothing, and the body keeps its own heading.
inline constexpr float kMinDistance = 1.f;

/**
 * Give the heading that faces a speaker, as the game counts it: radians clockwise from +Y, in [0, 2 pi).
 *
 * @param acFrom Where the talking player stands.
 * @param acTo Where the speaker stands.
 * @return The heading; nothing when the speaker is nearer than kMinDistance or farther than kMaxDistance on the ground.
 */
inline std::optional<float> HeadingTo(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const float cDx = acTo.x - acFrom.x;
    const float cDy = acTo.y - acFrom.y;
    const float cDistanceSquared = cDx * cDx + cDy * cDy;
    if (!(cDistanceSquared >= kMinDistance * kMinDistance) || cDistanceSquared > kMaxDistance * kMaxDistance)
        return std::nullopt;

    float heading = std::atan2(cDx, cDy);
    if (heading < 0.f)
        heading += glm::two_pi<float>();
    return heading;
}
} // namespace DialogueFacing
