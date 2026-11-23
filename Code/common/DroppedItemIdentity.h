#pragma once

#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>
#include <glm/geometric.hpp>
#include <unordered_set>

namespace DroppedItemIdentity
{
inline bool SameExtras(const Inventory::Entry& acSaved, const Inventory::Entry& acAnnounced)
{
    if (acSaved.ExtraWorn == acAnnounced.ExtraWorn && acSaved.ExtraWornLeft == acAnnounced.ExtraWornLeft)
        return acSaved.IsExtraDataEquals(acAnnounced);

    // A live drop is read before the item leaves inventory; a reannouncement reads the object on the floor.
    Inventory::Entry onFloor = acSaved;
    onFloor.ExtraWorn = acAnnounced.ExtraWorn;
    onFloor.ExtraWornLeft = acAnnounced.ExtraWornLeft;
    return onFloor.IsExtraDataEquals(acAnnounced);
}

inline bool SameStack(const Inventory::Entry& acSaved, const Inventory::Entry& acAnnounced) noexcept
{
    const auto cCount = [](int64_t aCount)
    {
        return aCount < 0 ? -aCount : aCount;
    };
    return cCount(acSaved.Count) == cCount(acAnnounced.Count) && acSaved.BaseId == acAnnounced.BaseId && SameExtras(acSaved, acAnnounced) &&
           acSaved.Data.Base.IsSet() == acAnnounced.Data.Base.IsSet() && (!acSaved.Data.Base.IsSet() || acSaved.Data.Base.Key() == acAnnounced.Data.Base.Key()) &&
           acSaved.EnchantData.IsWeapon == acAnnounced.EnchantData.IsWeapon && acSaved.EnchantData.Effects == acAnnounced.EnchantData.Effects;
}

inline bool SameKnownStack(const Inventory::Entry& acSaved, const Inventory::Entry& acAnnounced, bool aLegacyItemData)
{
    if (!aLegacyItemData)
        return SameStack(acSaved, acAnnounced);
    Inventory::Entry known = acAnnounced;
    known.Data = acSaved.Data;
    return SameStack(acSaved, known);
}

inline bool IsReply(uint32_t aPendingId, const Inventory::Entry& acPending, uint32_t aRequestId, const Inventory::Entry& acReply)
{
    return aPendingId && aPendingId == aRequestId && SameStack(acPending, acReply);
}

struct Match
{
    uint32_t Id{};
    bool Ambiguous{};
};

struct Pending
{
    uint32_t RefFormId{};
    uint32_t RefHandle{};
    uint32_t RequestId{};
    Inventory::Entry Item{};
    bool Announcement{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    bool PickedUp{};
};

inline bool WaitingForReplay(
    const Pending& acPending, const Inventory::Entry& acEntry, bool aReplay, bool aLegacyItemData, const GameId& acCellId, const GameId& acWorldSpaceId,
    const glm::vec3& acPosition, float aRadius)
{
    const bool cSamePlace = acWorldSpaceId ? acPending.WorldSpaceId == acWorldSpaceId : acPending.CellId == acCellId && !acPending.WorldSpaceId;
    return aReplay && cSamePlace && SameKnownStack(acEntry, acPending.Item, aLegacyItemData) && glm::distance(glm::vec3(acPending.Position), glm::vec3(acPosition)) <= aRadius;
}

class Nearby
{
public:
    //! aRequireUnique: two or more matching copies are ambiguous, not the nearest taken (used at an item's earlier resting
    //! places, where a copy is moved away: the wrong stack must never be moved).
    Nearby(const Inventory::Entry& acEntry, bool aLegacyItemData, float aRadius, bool aReplay, bool aRequireUnique = false)
        : m_entry(acEntry)
        , m_legacyItemData(aLegacyItemData)
        , m_requireUnique(aRequireUnique)
        , m_replay(aReplay)
        , m_radius(aRadius)
        , m_nearest(aRadius)
    {
    }

    //! aRunTime: a reference made while the game runs (a dropped item, IsTemporary), not one a plugin placed in the world.
    //! At an earlier resting place (aRequireUnique) only those count: a world-placed look-alike is never moved, and never
    //! makes the match ambiguous.
    void Consider(uint32_t aId, const Inventory::Entry& acEntry, float aDistance, bool aReserved = false, bool aRunTime = true)
    {
        if (!m_replay || aReserved || !(aDistance <= m_radius) || (!m_legacyItemData && !m_requireUnique && aDistance > m_nearest))
            return;
        if (m_requireUnique && !aRunTime)
            return;
        if (!SameKnownStack(m_entry, acEntry, m_legacyItemData) || !m_candidates.insert(aId).second)
            return;
        if (!m_id || aDistance < m_nearest || (aDistance == m_nearest && aId < m_id))
        {
            m_id = aId;
            m_nearest = aDistance;
        }
    }

    Match Get() const noexcept
    {
        const bool cAmbiguous = (m_legacyItemData || m_requireUnique) && m_candidates.size() > 1;
        return {cAmbiguous ? 0 : m_id, cAmbiguous};
    }

private:
    const Inventory::Entry& m_entry;
    bool m_legacyItemData{};
    bool m_requireUnique{};
    bool m_replay{};
    float m_radius{};
    float m_nearest{};
    uint32_t m_id{};
    std::unordered_set<uint32_t> m_candidates;
};

//! Exact records win; an old record lacking ItemData can be enriched only when it is the sole compatible candidate.
//! A record that keeps its earlier resting places also matches at one of them (a save made while it lay there announces
//! it there), but a record lying at the announced place now wins over one that only rested there before.
template <class T>
Match Find(const T& acItems, const Inventory::Entry& acEntry, const GameId& acCellId, const GameId& acWorldSpaceId, const Vector3_NetQuantize& acPosition, float aRadius)
{
    uint32_t exact{};
    uint32_t earlier{};
    uint32_t legacy{};
    size_t legacyCount{};
    float nearest = aRadius;
    float nearestEarlier = aRadius;
    for (const auto& [id, item] : acItems)
    {
        const bool cSamePlace = acWorldSpaceId ? item.WorldSpaceId == acWorldSpaceId : item.CellId == acCellId && !item.WorldSpaceId;
        if (!cSamePlace || !SameKnownStack(item.Entry, acEntry, item.LegacyItemData))
            continue;
        const float cDistance = glm::distance(glm::vec3(item.Position), glm::vec3(acPosition));
        float earlierDistance = cDistance;
        if constexpr (requires { item.NearestPlaceDistance(glm::vec3{}); })
            earlierDistance = item.NearestPlaceDistance(glm::vec3(acPosition));
        if (!(cDistance <= aRadius) && !(earlierDistance <= aRadius))
            continue;
        if (item.LegacyItemData)
        {
            legacy = id;
            ++legacyCount;
        }
        else if (cDistance <= aRadius)
        {
            if (cDistance < nearest || (cDistance == nearest && (!exact || id < exact)))
            {
                exact = id;
                nearest = cDistance;
            }
        }
        else if (earlierDistance < nearestEarlier || (earlierDistance == nearestEarlier && (!earlier || id < earlier)))
        {
            earlier = id;
            nearestEarlier = earlierDistance;
        }
    }
    if (exact)
        return {exact, false};
    if (earlier)
        return {earlier, false};
    return {legacyCount == 1 ? legacy : 0, legacyCount > 1};
}
} // namespace DroppedItemIdentity
