#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/LockData.h>
#include <Game/Player.h>

struct ObjectComponent
{
    ObjectComponent(Player* apLastSender)
        : pLastSender(apLastSender)
    {
    }

    Player* pLastSender;
    LockData CurrentLockData{};
    //! A game that sent it found it a merchant's chest (ObjectData::IsVendor); the ContainerLedger does not keep it.
    bool IsVendor{};
    //! A game sent it whole (RequestObjectInventoryChanges), which only a merchant's chest is; the ContainerLedger does not keep it.
    bool SentWhole{};
    //! It came with something in it, so it is a container: a door never does.
    bool HadContent{};
};
