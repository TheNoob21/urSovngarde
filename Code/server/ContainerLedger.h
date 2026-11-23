#pragma once

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <Structs/Inventory.h>
#include <Structs/LockData.h>

#include <algorithm>

// What a container held when the server forgot it, as no player had its cell in range any more.
// The next game in gets the content the party left there, not its own older copy: a chest one player looted does not come back
// full from another player's save. Kept for the server's session; ObjectService clears it once the last player leaves.
// Plain values in and out, so the unit tests check it without a server.
class ContainerLedger
{
public:
    // A long session's worth of containers. The one remembered longest ago goes first.
    static constexpr size_t kMax = 4096;

    // Where an object or a player is, as the server's CellIdComponent has it.
    struct Place
    {
        GameId Cell{};
        GameId WorldSpaceId{};
        GridCellCoords CenterCoords{};
    };

    struct Entry
    {
        Inventory Content{};
        LockData Lock{};
        Place Where{};
    };

    // Whether a container the server is about to forget is remembered.
    // Not a merchant's chest: the game restocks it, and the vendor sync sends it whole at each barter.
    // Nor one a game ever sent whole (RequestObjectInventoryChanges), which only a merchant's chest is.
    // Nor an object that never held anything, as a door: its lock is the game's own (a shop locked for the night).
    // A chest a player emptied held something before, so "looted everything" is remembered.
    static bool ShouldRemember(bool aIsVendor, bool aSentWhole, bool aHadContent) noexcept { return !aIsVendor && !aSentWhole && aHadContent; }

    // The rule of CellIdComponent::IsInRange(acViewer, false), called on the object's place as the door states are kept.
    static bool IsInRange(const Place& acObject, const Place& acViewer) noexcept
    {
        if (!acObject.WorldSpaceId)
            return acObject.Cell == acViewer.Cell;

        if (acObject.WorldSpaceId != acViewer.WorldSpaceId)
            return false;

        return GridCellCoords::IsCellInGridCell(acViewer.CenterCoords, acObject.CenterCoords, false);
    }

    // No player has the object's place in range, so the server forgets the object.
    // Not only the cell a player left: an object announced from the next cell over is in a cell nobody may ever enter.
    static bool InNobodysRange(const Place& acObject, const TiltedPhoques::Vector<Place>& acPlayers) noexcept
    {
        return std::none_of(acPlayers.begin(), acPlayers.end(), [&acObject](const Place& acPlayer) { return IsInRange(acObject, acPlayer); });
    }

    // Remembered again, a container keeps only its latest content, and counts as remembered now.
    // An empty one is remembered too.
    void Remember(const GameId& acObject, const Inventory& acContent, const LockData& acLock, const Place& acWhere) noexcept
    {
        if (m_entries.find(acObject) != m_entries.end())
        {
            std::erase(m_order, acObject);
        }
        else if (m_entries.size() >= kMax)
        {
            m_entries.erase(m_order.front());
            m_order.erase(m_order.begin());
        }

        m_entries[acObject] = Entry{acContent, acLock, acWhere};
        m_order.push_back(acObject);
    }

    // The container as it was last remembered, or null when it was not. It stays remembered.
    const Entry* Recall(const GameId& acObject) const noexcept
    {
        const auto cIter = m_entries.find(acObject);
        return cIter != m_entries.end() ? &cIter->second : nullptr;
    }

    // A container the server no longer keeps whole as the ledger has it (it became a merchant's chest), so it is not recalled stale.
    void Forget(const GameId& acObject) noexcept
    {
        if (m_entries.erase(acObject) > 0)
            std::erase(m_order, acObject);
    }

    void Clear() noexcept
    {
        m_entries.clear();
        m_order.clear();
    }

    size_t Size() const noexcept { return m_entries.size(); }

private:
    TiltedPhoques::Map<GameId, Entry> m_entries;
    // Oldest first.
    TiltedPhoques::Vector<GameId> m_order;
};
