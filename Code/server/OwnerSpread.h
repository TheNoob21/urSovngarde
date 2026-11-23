#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

//! How the server spreads the NPCs it hands out over the party's games, so the leader's game does not run them all.
namespace OwnerSpread
{
//! The party leader's game takes an NPC this near its player (the client's leader claim distance, CharacterService::RunLeaderClaims).
constexpr float kNear = 6000.f;
//! It gives one to a nearer member only once its player is this far, so an NPC at the edge does not go back and forth.
constexpr float kFar = 8000.f;

struct Candidate
{
    //! Lower is better: how well that game can take the character over (PresenceService::NextOwners).
    uint32_t Tier{};
    //! Living characters that game already runs.
    uint32_t Owned{};
};

//! The candidates' indices, best first: the better tier, then the game running the fewest, then the order given.
//! The first candidate alone took all of a leaving owner's NPCs at once.
inline std::vector<size_t> Order(const std::vector<Candidate>& acCandidates) noexcept
{
    std::vector<size_t> order(acCandidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(
        order.begin(), order.end(),
        [&acCandidates](const size_t aLeft, const size_t aRight)
        {
            const Candidate& cLeft = acCandidates[aLeft];
            const Candidate& cRight = acCandidates[aRight];
            return cLeft.Tier != cRight.Tier ? cLeft.Tier < cRight.Tier : cLeft.Owned < cRight.Owned;
        });
    return order;
}

//! Distances are to the character, empty when that player is elsewhere (another worldspace or interior).
inline bool IsNear(const std::optional<float> aDistance) noexcept
{
    return aDistance && *aDistance <= kNear;
}

//! Whether the leader's game is given an NPC it registers: when its player is near it, or when no other member is.
inline bool LeaderTakes(const std::optional<float> aLeader, const std::optional<float> aNearestMember) noexcept
{
    return IsNear(aLeader) || !IsNear(aNearestMember);
}

//! A game that came into a place this lately is not given its NPCs yet: its cells are still loading.
constexpr auto kArrivalGrace = std::chrono::seconds(10);

//! Whether a member's game may take over an NPC another game runs: its own report has the reference loaded (aLoaded), not only on
//! a stand-in (aStandIn), it has not declined that NPC (aDeclined), and it came into its place at least kArrivalGrace ago.
//! A member who may not is passed over for the next nearest.
inline bool MayTakeOver(const bool aLoaded, const bool aStandIn, const bool aDeclined, const std::chrono::steady_clock::duration aSinceArrival) noexcept
{
    return aLoaded && !aStandIn && !aDeclined && aSinceArrival >= kArrivalGrace;
}

//! Whether a member near an NPC the leader's game runs takes it over: once the leader is far from it.
inline bool MemberTakesFromLeader(const std::optional<float> aLeader, const std::optional<float> aMember) noexcept
{
    return IsNear(aMember) && (!aLeader || *aLeader > kFar);
}
} // namespace OwnerSpread
