#pragma once

#include <DroppedItemIdentity.h>
#include <Messages/NotifyDroppedItem.h>
#include <algorithm>
#include <vector>

namespace DroppedItemReplies
{
struct AwaitingScene
{
    DroppedItemIdentity::Pending Drop{};
    uint32_t ServerId{};
    NotifyDroppedItem Message{};
};

inline std::vector<NotifyDroppedItem> Replays(const std::vector<AwaitingScene>& acAwaiting)
{
    std::vector<NotifyDroppedItem> messages;
    messages.reserve(acAwaiting.size());
    for (const auto& waiting : acAwaiting)
    {
        auto message = waiting.Message;
        message.RequestId = 0;
        message.Replay = true;
        messages.push_back(std::move(message));
    }
    return messages;
}

inline bool Names(const DroppedItemIdentity::Pending& acPending, uint32_t aRefFormId, uint32_t aHandle, const Inventory::Entry& acItem)
{
    return acPending.RefFormId == aRefFormId && acPending.RefHandle == aHandle && DroppedItemIdentity::SameStack(acPending.Item, acItem);
}

//! Whether the reference now under a dropped item's mapped id is still that item, so that its pick-up in another game may remove
//! it here. The game reuses a deleted item's id for another object, an actor among them: never an actor, and the base form and the
//! handle (whose age changes when the id is reused) must be the ones mapped, where they are known.
inline bool StillTheItem(uint32_t aMappedBase, uint32_t aMappedHandle, uint32_t aLiveBase, uint32_t aLiveHandle, bool aIsActor)
{
    return !aIsActor && (!aMappedBase || aMappedBase == aLiveBase) && (!aMappedHandle || aMappedHandle == aLiveHandle);
}

inline bool MappedMatches(uint32_t aHandle, uint32_t aLiveHandle, const Inventory::Entry& acMessage, const Inventory::Entry& acFloor, bool aLegacyItemData)
{
    return aHandle && aHandle == aLiveHandle && DroppedItemIdentity::SameKnownStack(acMessage, acFloor, aLegacyItemData);
}

inline bool Reserved(const std::vector<DroppedItemIdentity::Pending>& acPending, const std::vector<AwaitingScene>& acAwaiting, uint32_t aRefFormId, uint32_t aServerId = 0)
{
    return std::any_of(acPending.begin(), acPending.end(), [aRefFormId](const auto& acDrop) { return acDrop.RefFormId == aRefFormId; }) ||
           std::any_of(acAwaiting.begin(), acAwaiting.end(), [&](const auto& acWaiting) { return acWaiting.Drop.RefFormId == aRefFormId && acWaiting.ServerId != aServerId; });
}

template <class TRemove>
bool PickedUp(
    std::vector<DroppedItemIdentity::Pending>& aPending, std::vector<AwaitingScene>& aAwaiting, uint32_t aRefFormId, uint32_t aHandle, const Inventory::Entry& acItem,
    TRemove aRemove)
{
    bool found = false;
    for (auto& pending : aPending)
    {
        if (Names(pending, aRefFormId, aHandle, acItem))
        {
            pending.PickedUp = true;
            found = true;
        }
    }
    for (auto it = aAwaiting.begin(); it != aAwaiting.end();)
    {
        if (Names(it->Drop, aRefFormId, aHandle, acItem))
        {
            const auto id = it->ServerId;
            it = aAwaiting.erase(it);
            aRemove(id);
            found = true;
        }
        else
            ++it;
    }
    return found;
}

//! A reload may replace the handle, but never the saved reference, stack and scoped place together.
template <class THandleLive, class TRemove>
bool PickedUpScoped(
    std::vector<DroppedItemIdentity::Pending>& aPending, std::vector<AwaitingScene>& aAwaiting, uint32_t aRefFormId, uint32_t aHandle, const Inventory::Entry& acItem,
    const GameId& acCellId, const GameId& acWorldSpaceId, const glm::vec3& acPosition, float aRadius, THandleLive aHandleLive, TRemove aRemove)
{
    if (PickedUp(aPending, aAwaiting, aRefFormId, aHandle, acItem, aRemove))
        return true;
    const auto matches = [&](const auto& acDrop)
    {
        return aHandle && acDrop.RefFormId == aRefFormId && !aHandleLive(acDrop.RefHandle) &&
               DroppedItemIdentity::WaitingForReplay(acDrop, acItem, true, false, acCellId, acWorldSpaceId, acPosition, aRadius);
    };
    bool found = false;
    for (auto& drop : aPending)
        if (matches(drop))
        {
            drop.PickedUp = true;
            found = true;
        }
    for (auto it = aAwaiting.begin(); it != aAwaiting.end();)
    {
        if (matches(it->Drop))
        {
            const auto id = it->ServerId;
            it = aAwaiting.erase(it);
            aRemove(id);
            found = true;
        }
        else
            ++it;
    }
    return found;
}

enum class Outcome
{
    Handled,
    Unloaded,
    Place
};

struct Result
{
    Outcome Action{};
    bool Consumed{};
};

//! Consume ownership replies before checking whether scene placement is possible.
template <class TAdopt, class TRemove, class TIsLoaded>
Result Receive(
    std::vector<DroppedItemIdentity::Pending>& aPending, std::vector<AwaitingScene>& aAwaiting, NotifyDroppedItem& aMessage, TAdopt aAdopt, TRemove aRemove, TIsLoaded aIsLoaded)
{
    bool consumed = false;
    const auto it = std::find_if(
        aPending.begin(), aPending.end(),
        [&](const auto& acPending) { return DroppedItemIdentity::IsReply(acPending.RequestId, acPending.Item, aMessage.RequestId, aMessage.Item); });
    if (it != aPending.end())
    {
        const auto pending = *it;
        aPending.erase(it);
        consumed = true;
        if (pending.PickedUp)
        {
            aRemove(aMessage.Id);
            return {Outcome::Handled, true};
        }
        aAwaiting.push_back({pending, aMessage.Id, aMessage});
        aMessage.Replay = true;
    }
    else if (const auto waiting = std::find_if(aAwaiting.begin(), aAwaiting.end(), [&](const auto& acWaiting) { return acWaiting.ServerId == aMessage.Id; });
             waiting != aAwaiting.end())
        waiting->Message = aMessage;
    if (!aIsLoaded())
        return {Outcome::Unloaded, consumed};

    if (const auto waiting = std::find_if(aAwaiting.begin(), aAwaiting.end(), [&](const auto& acWaiting) { return acWaiting.ServerId == aMessage.Id; }); waiting != aAwaiting.end())
    {
        const auto pending = waiting->Drop;
        // Adoption may erase the awaiting entry, so keep a value copy across the callback.
        if (aAdopt(pending))
            return {Outcome::Handled, consumed};
        aMessage.Replay = true;
    }
    return {Outcome::Place, consumed};
}
} // namespace DroppedItemReplies
