#pragma once

// A map location one party member discovers is discovered for the whole party, as the party explores together: plain
// values, so the rules are unit-tested.
//
// The party's fast travel goes by the leader's map (RequestPartyFastTravel), so a place only a member found could not be
// chosen there. Only what a member discovers while in the party is shared; each save's own map from before stays its own.

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MapDiscoveries
{
// The map flags of a marker reference (ExtraMapMarker), as PlayerMapRules has them.
inline constexpr uint8_t kVisible = 1;
inline constexpr uint8_t kCanTravel = 2;

// Whether a marker with aFlags here still needs to be put on the map for a discovery the party made.
[[nodiscard]] constexpr bool NeedsAdding(uint8_t aFlags, bool aCanTravel) noexcept
{
    return (aFlags & kVisible) == 0 || (aCanTravel && (aFlags & kCanTravel) == 0);
}

// The markers a party discovered, kept by the server for a member who joins later.
class Party
{
public:
    // aMarker: the marker reference's server id, its mod id in the upper half.
    // Whether this is news for the party: a marker it did not have, or one it may now travel to.
    bool Add(uint64_t aMarker, bool aCanTravel)
    {
        const auto [it, inserted] = m_markers.emplace(aMarker, aCanTravel);
        if (inserted)
            return true;
        if (!aCanTravel || it->second)
            return false;
        it->second = true;
        return true;
    }

    [[nodiscard]] std::vector<std::pair<uint64_t, bool>> All() const { return {m_markers.begin(), m_markers.end()}; }
    [[nodiscard]] size_t Size() const noexcept { return m_markers.size(); }

private:
    std::unordered_map<uint64_t, bool> m_markers;
};

// What this game's map discovered since the last look, by the flags of its markers.
class Watch
{
public:
    struct Found
    {
        uint32_t FormId;
        bool CanTravel;

        bool operator==(const Found& acRhs) const noexcept { return FormId == acRhs.FormId && CanTravel == acRhs.CanTravel; }
    };

    // acMarkers: each marker reference's form id and flags now. A marker not seen before is noted, not news: the first look
    // after a load or in another world space only learns what the save holds.
    [[nodiscard]] std::vector<Found> Look(const std::vector<std::pair<uint32_t, uint8_t>>& acMarkers)
    {
        std::vector<Found> found;
        for (const auto& [cFormId, cFlags] : acMarkers)
        {
            const auto [it, inserted] = m_flags.emplace(cFormId, cFlags);
            if (inserted)
                continue;
            const uint8_t cWas = it->second;
            it->second = cFlags;
            const bool cShown = (cWas & kVisible) == 0 && (cFlags & kVisible) != 0;
            const bool cTravel = (cWas & kCanTravel) == 0 && (cFlags & kCanTravel) != 0;
            if (cShown || cTravel)
                found.push_back({cFormId, (cFlags & kCanTravel) != 0});
        }
        return found;
    }

    // A discovery of the party's put on the map here: not this game's news when the next look sees it.
    // A marker not looked at yet is left to that first look, which takes it as it is.
    void Learned(uint32_t aFormId, bool aCanTravel)
    {
        if (const auto cIt = m_flags.find(aFormId); cIt != m_flags.end())
            cIt->second |= static_cast<uint8_t>(kVisible | (aCanTravel ? kCanTravel : 0));
    }

    // A save loaded, a connection, a party joined or left.
    void Reset() noexcept { m_flags.clear(); }

private:
    std::unordered_map<uint32_t, uint8_t> m_flags;
};
} // namespace MapDiscoveries
