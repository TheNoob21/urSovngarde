#pragma once

#include <chrono>

// What the other players are told of this player's arrow: on the string, in a hand, or gone.
// A VR arrow leaves the hand's hold as it touches the bow, a second or more before the draw puts it on the string,
// and it slips out of the hold for a moment while the hand moves. The arrow is still there both times,
// so the hand is said to be empty only once it has stayed empty for a while; on the string and off the bow count at once.
namespace ArrowNocking
{
// Longer than the longest wait seen between the arrow touching the bow and the draw starting (1.4 s).
constexpr std::chrono::milliseconds cHandEmptyAfter{1500};
// Longer than the longest slip out of the hold seen while the hand moves (210 ms).
constexpr std::chrono::milliseconds cOnStringAfter{500};
// Hands farther apart than this are not at the bow: an arrow that left the hold there is at the hip or the quiver, not on the string.
constexpr float cAtBowReach = 35.f;
// A drawn string is never farther from the bow hand than this; the hands this far apart for cLetGoAfter let the arrow go.
constexpr float cDrawReach = 90.f;
constexpr std::chrono::milliseconds cLetGoAfter{1000};

struct Arrow
{
    bool Nocked = false;
    bool InHand = false;

    bool operator==(const Arrow& acRhs) const noexcept { return Nocked == acRhs.Nocked && InHand == acRhs.InHand; }
    bool operator!=(const Arrow& acRhs) const noexcept { return !(*this == acRhs); }
};

class Filter
{
public:
    // What to tell, given where the game has the arrow now, whether the bow is out, and whether this player shot an arrow since the last call.
    // aHandsApart: how far the two hands are from each other, negative when not known.
    // An arrow that leaves the hand with the bow out went onto the string, and stays there until it is shot.
    // Some VR games never set the bow's attack states (a PCVR draw stays at 0), so the hand emptying is the only sign of the nock.
    // An inferred nock needs a recent bow contact and a current hand distance.
    Arrow Update(
        const bool aNocked, const bool aInHand, const bool aWeaponDrawn, const std::chrono::steady_clock::time_point aNow, const bool aShot = false,
        const float aHandsApart = -1.f) noexcept
    {
        const bool cKnown = aHandsApart >= 0.f;
        // The game's own attack state says when it leaves the string: gone then.
        const bool cGameLetGo = m_gameNocked && !aNocked;
        m_gameNocked = aNocked && aWeaponDrawn;
        if (!aWeaponDrawn || aNocked || aInHand || aShot || cGameLetGo || (!m_told.InHand && !m_told.Nocked))
        {
            m_emptySince = {};
            m_farSince = {};
            m_seenAtBow = aInHand && aWeaponDrawn && !aShot && cKnown && aHandsApart <= cAtBowReach ? aNow : std::chrono::steady_clock::time_point{};
            m_told = Arrow{aNocked && aWeaponDrawn, !aNocked && aInHand && aWeaponDrawn};
            return m_told;
        }
        // Already on the string: it stays there until it is shot or the bow is put away, or the hands part for good.
        if (m_told.Nocked)
        {
            if (cKnown && aHandsApart <= cDrawReach)
            {
                m_farSince = {};
                return m_told;
            }
            if (m_farSince == std::chrono::steady_clock::time_point{})
                m_farSince = aNow;
            if (aNow - m_farSince >= (cKnown ? cLetGoAfter : cHandEmptyAfter))
            {
                m_farSince = {};
                m_seenAtBow = {};
                m_told = Arrow{};
            }
            return m_told;
        }

        if (m_emptySince == std::chrono::steady_clock::time_point{})
            m_emptySince = aNow;
        if (cKnown && aHandsApart <= cAtBowReach)
            m_seenAtBow = aNow;
        const bool cSeenAtBow = m_seenAtBow != std::chrono::steady_clock::time_point{} && aNow - m_seenAtBow < cHandEmptyAfter;
        if (cSeenAtBow && cKnown && aHandsApart <= cDrawReach && aNow - m_emptySince >= cOnStringAfter)
        {
            m_emptySince = {};
            m_seenAtBow = {};
            m_told = Arrow{true, false};
        }
        // An arrow that never came near the bow is not on its string: it is put away like any other.
        else if (aNow - m_emptySince >= cHandEmptyAfter)
        {
            m_emptySince = {};
            m_seenAtBow = {};
            m_told = Arrow{};
        }
        return m_told;
    }

private:
    bool m_gameNocked = false;
    Arrow m_told{};
    std::chrono::steady_clock::time_point m_emptySince{};
    std::chrono::steady_clock::time_point m_farSince{};
    std::chrono::steady_clock::time_point m_seenAtBow{};
};
} // namespace ArrowNocking
