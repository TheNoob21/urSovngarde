#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstdint>
#include <deque>

// A flat player's block in the defender's rule (BlockService), with no game in it.
// In VR a hit from another player is stopped when this player's blade or shield met that player's weapon (VRBodySync).
// A flat game has no hands to meet anything with: its player holds the block, and the hit is stopped when the block was up as the hit
// was seen here and faced the one who struck.
namespace PvpBlock
{
// The other player's copy shows here as late as its playback clock plays it, so a hit at tick t is seen at t plus that delay.
// The delay before the copy's first update arrived, and the bounds of a measured one (as ActorValueService::RunHeldHits takes them).
constexpr uint64_t kDefaultSeenAfter = 225;
constexpr uint64_t kMinSeenAfter = 16;
constexpr uint64_t kMaxSeenAfter = 400;

// A block counts from this long before the hit was seen here up to this long after.
// Blades meet before the hit lands, so the VR rule looks half a second back; a raised shield has to be up when the blow arrives.
constexpr uint64_t kBlockBefore = 100;
constexpr uint64_t kBlockAfter = 50;

// A block faces what it stops: the one who struck stands within this many degrees of where this player faces.
constexpr float kFrontHalfAngleDegrees = 60.f;

// The defender's rule (ActorValueService::RunHeldHits) drops a blow when a clash on its attacker's copy came from 525 ms before the blow
// was seen to 100 ms after: blades meet before the hit lands. A flat block is told for one blow, so its clash is dated to the far end of
// that window, less a margin for the two reckonings of when the blow was seen: then it stops this blow and one seen up to the margin
// later, and none seen afterwards.
constexpr uint64_t kRuleBlockBefore = 525;
constexpr uint64_t kSeenMargin = 50;

inline uint64_t ClashTick(const uint64_t aSeen) noexcept
{
    constexpr uint64_t cBack = kRuleBlockBefore - kSeenMargin;
    return aSeen > cBack ? aSeen - cBack : 0;
}

// Spans older than this are forgotten: a hit is decided within a second of being seen.
constexpr uint64_t kKeep = 5000;

// How far behind the attacker their copy is shown here: now less the copy's playback tick, or the default when there is none yet.
inline uint64_t SeenAfter(const uint64_t aNow, const uint64_t aPlaybackTick) noexcept
{
    if (!aPlaybackTick || aPlaybackTick >= aNow)
        return kDefaultSeenAfter;
    return std::clamp<uint64_t>(aNow - aPlaybackTick, kMinSeenAfter, kMaxSeenAfter);
}

// When this player held a block, as spans on the server's clock.
class History
{
public:
    // Whether this player blocks now. Called once a frame; a frame that reads the same as the last only stretches the span.
    void Note(const uint64_t aTick, const bool aBlocking) noexcept
    {
        // A clock that went back (set again by the server) makes the spans meaningless.
        if (!m_spans.empty() && aTick < m_spans.back().To)
            Clear();
        if (aBlocking)
        {
            if (m_open && !m_spans.empty())
                m_spans.back().To = std::max(m_spans.back().To, aTick);
            else
                m_spans.push_back({aTick, aTick});
        }
        m_open = aBlocking;
        while (!m_spans.empty() && m_spans.front().To + kKeep < aTick)
            m_spans.pop_front();
    }

    // Whether a block was held at any time from aFrom to aTo (both included).
    bool HeldDuring(const uint64_t aFrom, const uint64_t aTo) const noexcept
    {
        return std::any_of(m_spans.begin(), m_spans.end(), [aFrom, aTo](const Span& acSpan) { return acSpan.From <= aTo && acSpan.To >= aFrom; });
    }

    void Clear() noexcept
    {
        m_spans.clear();
        m_open = false;
    }

    size_t Size() const noexcept { return m_spans.size(); }

private:
    struct Span
    {
        uint64_t From;
        uint64_t To;
    };
    std::deque<Span> m_spans;
    bool m_open = false;
};

// Whether a hit seen here at aSeen was blocked, as far as this player's block goes, knowing the block up to aNow.
enum class Verdict : uint8_t
{
    kWait,
    kBlocked,
    kLands,
};

inline Verdict Decide(const History& acHistory, const uint64_t aSeen, const uint64_t aNow) noexcept
{
    const uint64_t cFrom = aSeen > kBlockBefore ? aSeen - kBlockBefore : 0;
    if (aNow + kBlockBefore < aSeen)
        return Verdict::kWait;
    // A block already seen in the window decides at once; no block needs the whole window.
    if (acHistory.HeldDuring(cFrom, std::min(aNow, aSeen + kBlockAfter)))
        return Verdict::kBlocked;
    return aNow >= aSeen + kBlockAfter ? Verdict::kLands : Verdict::kWait;
}

// Whether a block held by a player at (aX, aY) facing aHeading (the game's: 0 is +y, turning towards +x) faces one standing at (aOtherX, aOtherY).
// One standing on the same spot is faced.
inline bool Faces(const float aX, const float aY, const float aHeading, const float aOtherX, const float aOtherY) noexcept
{
    const float cDx = aOtherX - aX;
    const float cDy = aOtherY - aY;
    const float cLength = std::sqrt(cDx * cDx + cDy * cDy);
    if (!std::isfinite(cLength) || !std::isfinite(aHeading))
        return false;
    if (cLength <= 1.f)
        return true;
    const float cForward = (std::sin(aHeading) * cDx + std::cos(aHeading) * cDy) / cLength;
    return cForward >= std::cos(kFrontHalfAngleDegrees * std::numbers::pi_v<float> / 180.f);
}
} // namespace PvpBlock
