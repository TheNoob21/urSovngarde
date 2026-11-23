#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/ItemTakeSource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

//! This player's takes the server may yet give to another player who took the same item first (ItemTakeService),
//! kept until its answer has had time to come. A lost take is taken back off the player, but never more than was taken.
//! Plain values in and out, so the unit tests check it without a game.
class TakeUndo
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kKeptFor = std::chrono::seconds(10);

    //! aId: the container's or body's server id, the dropped item's id, or a placed item's reference id (GameId::LogFormat).
    void Note(const ItemTakeSource aFrom, const uint64_t aId, const Inventory::Entry& acItem, const Clock::time_point aNow) noexcept
    {
        Forget(aNow);
        // Each player's quest needs its own quest item, so one is never given back.
        if (acItem.Count <= 0 || acItem.IsQuestItem)
            return;
        m_takes.push_back({aFrom, aId, Taken(acItem), aNow});
    }

    //! What to take back off the player for a lost take, or nothing when no such take is kept.
    //! Out of a container or a body, at most acItem's count of that very item; a dropped or placed item is one take, given back whole.
    std::optional<Inventory::Entry> Lost(const ItemTakeSource aFrom, const uint64_t aId, const Inventory::Entry& acItem, const Clock::time_point aNow) noexcept
    {
        Forget(aNow);
        const bool cWhole = aFrom != ItemTakeSource::kInventory;
        const Inventory::Entry cItem = Taken(acItem);
        int32_t count = cItem.Count;
        std::optional<Inventory::Entry> back;
        for (Take& take : m_takes)
        {
            if (take.From != aFrom || take.Id != aId || (!cWhole && !take.Item.CanBeMerged(cItem)))
                continue;
            const int32_t cBack = cWhole ? take.Item.Count : std::min(count, take.Item.Count);
            if (cBack <= 0)
                continue;
            if (!back)
            {
                back = take.Item;
                back->Count = 0;
            }
            back->Count += cBack;
            take.Item.Count -= cBack;
            count -= cBack;
            if (cWhole || count <= 0)
                break;
        }
        std::erase_if(m_takes, [](const Take& acTake) { return acTake.Item.Count <= 0; });
        return back;
    }

    void Clear() noexcept { m_takes.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_takes.size(); }

private:
    //! A taken item is in the player's pack, worn by nobody: armour off a body still carries the body's worn flag.
    static Inventory::Entry Taken(Inventory::Entry aItem) noexcept
    {
        aItem.ExtraWorn = false;
        aItem.ExtraWornLeft = false;
        return aItem;
    }

    void Forget(const Clock::time_point aNow) noexcept
    {
        std::erase_if(m_takes, [aNow](const Take& acTake) { return aNow - acTake.At >= kKeptFor; });
    }

    struct Take
    {
        ItemTakeSource From{};
        uint64_t Id{};
        Inventory::Entry Item{};
        Clock::time_point At{};
    };
    std::vector<Take> m_takes;
};
