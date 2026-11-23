#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

// Which placed objects near the players a game watches for moves, and how it sends many that start at once (a blast,
// a shout, a body falling into a cluttered room). Plain values in and out, so it has unit tests (Code/tests/world_object_watch.cpp).
namespace WorldObjectWatch
{
using Clock = std::chrono::steady_clock;

//! Objects watched at most. The list is rebuilt once a second, the watching runs thirty times a second.
inline constexpr size_t kMaxWatched = 128;
//! An outdoor grid cell is this wide.
inline constexpr float kCellSize = 4096.f;

//! Starting in one pass, this many is a blast or a cell settling rather than a hand.
inline constexpr size_t kManyStart = 5;
//! Objects first watched this recently that start together are a cell settling as it attaches: nobody sends them.
inline constexpr auto kSettleFor = std::chrono::seconds(3);
//! Part of a blast this long after it, so its objects keep the blast's rules while they wait to be sent.
inline constexpr auto kBlastFor = std::chrono::seconds(3);
//! Nearer another player's copy than this player, and within this of it, a start is theirs.
inline constexpr float kTheirsReach = 150.f;
//! In a blast, the game whose player stands nearer an object by more than this sends it; within it, the game whose player has the
//! lower server id does. Each game measures its own copy of the object against its own late view of the other player.
inline constexpr float kClaimMargin = 64.f;
//! A blast object left to another game that still moves here this long after, with no move of it from there, is sent from here,
//! for as long as kClaimUntil after it was left.
inline constexpr auto kClaimAfter = std::chrono::seconds(1);
inline constexpr auto kClaimUntil = std::chrono::seconds(4);

//! Whether any of a grid cell lies within aRadius of a place, the cell's square against the circle.
[[nodiscard]] inline bool IsCellNear(const int32_t aX, const int32_t aY, const glm::vec2& acAt, const float aRadius) noexcept
{
    const float cMinX = static_cast<float>(aX) * kCellSize;
    const float cMinY = static_cast<float>(aY) * kCellSize;
    const float cDx = std::max({cMinX - acAt.x, 0.f, acAt.x - (cMinX + kCellSize)});
    const float cDy = std::max({cMinY - acAt.y, 0.f, acAt.y - (cMinY + kCellSize)});
    return cDx * cDx + cDy * cDy <= aRadius * aRadius;
}

//! How near an object is to the players: to this player or the nearest other player's copy, whichever is nearer.
[[nodiscard]] inline float Nearness(const glm::vec3& acAt, const glm::vec3& acPlayer, const std::vector<glm::vec3>& acOthers) noexcept
{
    float nearest = glm::distance(acAt, acPlayer);
    for (const glm::vec3& cOther : acOthers)
        nearest = std::min(nearest, glm::distance(acAt, cOther));
    return nearest >= 0.f ? nearest : std::numeric_limits<float>::infinity();
}

struct Candidate
{
    uint32_t FormId{};
    float Nearness{};
};

//! The objects to watch out of those found in all the cells: each once, nearest first, at most aMax.
[[nodiscard]] inline std::vector<uint32_t> Pick(std::vector<Candidate> aFound, const size_t aMax = kMaxWatched) noexcept
{
    for (Candidate& candidate : aFound)
        if (!(candidate.Nearness >= 0.f))
            candidate.Nearness = std::numeric_limits<float>::infinity();
    std::sort(
        aFound.begin(), aFound.end(),
        [](const Candidate& acA, const Candidate& acB) { return acA.Nearness != acB.Nearness ? acA.Nearness < acB.Nearness : acA.FormId < acB.FormId; });

    std::vector<uint32_t> picked;
    std::unordered_set<uint32_t> seen;
    for (const Candidate& cCandidate : aFound)
    {
        if (picked.size() >= aMax)
            break;
        if (seen.insert(cCandidate.FormId).second)
            picked.push_back(cCandidate.FormId);
    }
    return picked;
}

//! Whether this pass's starts are a blast: placed objects and dropped items knocked about together count as one.
[[nodiscard]] inline bool IsBlast(const size_t aObjectStarts, const size_t aItemStarts) noexcept
{
    return aObjectStarts + aItemStarts >= kManyStart;
}

//! Whether the object first watched at aFirstSeen is still settling with its cell.
[[nodiscard]] inline bool IsSettling(const Clock::time_point aFirstSeen, const Clock::time_point aNow) noexcept
{
    return aNow - aFirstSeen < kSettleFor;
}

//! Another player's copy, as this game sees it: how far it stands from an object, and the player's server id.
struct Other
{
    float Distance{};
    uint32_t PlayerId{};
};

//! Whether a start is another player's to send rather than this player's (aMine away, server id aPlayerId). A hand's start is theirs
//! when their copy stands nearer and within kTheirsReach of it. A blast's object is theirs when their copy stands nearer by more
//! than kClaimMargin, or within the margin when their id is lower. Near the margin's edge the two games can still both leave it,
//! which ClaimsLeftObject settles.
[[nodiscard]] inline bool IsTheirs(const float aMine, const uint32_t aPlayerId, const std::vector<Other>& acOthers, const bool aBlast) noexcept
{
    return std::any_of(
        acOthers.begin(), acOthers.end(),
        [&](const Other& acOther)
        {
            if (!aBlast)
                return acOther.Distance < aMine && acOther.Distance < kTheirsReach;
            return acOther.Distance < aMine - kClaimMargin || (acOther.Distance <= aMine + kClaimMargin && acOther.PlayerId < aPlayerId);
        });
}

//! Whether a blast object left to another game at aLeftAt, and seen moving here now, is sent from here after all: no move of it
//! has come from there since (aLastRemote), kClaimAfter on. Then it has a sender even when both games left it, or the other
//! game never saw it move.
[[nodiscard]] inline bool ClaimsLeftObject(const Clock::time_point aLeftAt, const Clock::time_point aLastRemote, const Clock::time_point aNow) noexcept
{
    return aLeftAt != Clock::time_point{} && aNow - aLeftAt >= kClaimAfter && aNow - aLeftAt < kClaimUntil && aLastRemote < aLeftAt;
}

//! What a resting object does on a pass once its quiet is over.
enum class Resting : uint8_t
{
    Stays,
    //! It moved clearly from where it lay.
    Starts,
    //! A blast left it to another game, it came to rest here within its quiet, and no move of it came from there: where it lies
    //! is sent from here once, at rest. The server passes on one player's rest, and a move that comes from there first clears
    //! the leaving here.
    SendsPlace,
};

//! aStarted: it moved clearly from where it lay. aLeftAt and aLastRemote as for ClaimsLeftObject.
[[nodiscard]] inline Resting JudgeResting(const bool aStarted, const Clock::time_point aLeftAt, const Clock::time_point aLastRemote, const Clock::time_point aNow) noexcept
{
    if (aStarted)
        return Resting::Starts;
    return ClaimsLeftObject(aLeftAt, aLastRemote, aNow) ? Resting::SendsPlace : Resting::Stays;
}

//! The moves (not the rests) this game sends in a second. A hand's moves always go and are counted; a blast's starts and
//! moves share what is left, a few each pass, so they stay well under what the server passes on from one player.
class Budget
{
public:
    static constexpr uint32_t kPerSecond = 96;
    static constexpr size_t kPerPass = 3;

    //! Moves sent regardless: a hand's.
    void Spend(const Clock::time_point aNow, const size_t aCount) noexcept
    {
        Roll(aNow);
        m_count += static_cast<uint32_t>(aCount);
    }

    //! How many of aWanted blast moves go this pass, counted as sent.
    size_t Grant(const Clock::time_point aNow, const size_t aWanted) noexcept
    {
        Roll(aNow);
        const size_t cLeft = m_count < kPerSecond ? kPerSecond - m_count : 0;
        const size_t cGranted = std::min({aWanted, kPerPass, cLeft});
        m_count += static_cast<uint32_t>(cGranted);
        return cGranted;
    }

private:
    void Roll(const Clock::time_point aNow) noexcept
    {
        if (aNow - m_since >= std::chrono::seconds(1))
        {
            m_since = aNow;
            m_count = 0;
        }
    }

    Clock::time_point m_since{};
    uint32_t m_count{};
};

//! Which aCount of the waiting sends go: the nearest to this player, as places in acDistances, nearest first.
[[nodiscard]] inline std::vector<size_t> Nearest(const std::vector<float>& acDistances, const size_t aCount) noexcept
{
    std::vector<size_t> order(acDistances.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto cKey = [&](const size_t aIndex)
    {
        return acDistances[aIndex] >= 0.f ? acDistances[aIndex] : std::numeric_limits<float>::infinity();
    };
    std::stable_sort(order.begin(), order.end(), [&](const size_t aA, const size_t aB) { return cKey(aA) < cKey(aB); });
    order.resize(std::min(aCount, order.size()));
    return order;
}
} // namespace WorldObjectWatch
