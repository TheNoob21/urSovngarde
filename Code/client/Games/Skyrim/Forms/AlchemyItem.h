#pragma once

#include <cstddef>

#include "MagicItem.h"

#include <Structs/ItemData.h>

struct AlchemyItem : MagicItem
{
    enum Flags : uint32_t
    {
        kCostOverride = 1 << 0,
        kPoison = 1 << 17,
    };

    //! The game's made potion or poison with these effects: the one it already made, or a new one (BGSCreatedObjectManager).
    static AlchemyItem* Create(const CreatedAlchemy& acData) noexcept;

    [[nodiscard]] bool IsPoison() const noexcept { return (data.uiFlags & kPoison) != 0; }
    [[nodiscard]] bool IsCreated() const noexcept { return (formID & 0xFF000000) == 0xFF000000; }

    uint8_t unk90[0x138 - sizeof(MagicItem)];

    struct Data
    {
        int32_t iCostOverride;
        uint32_t uiFlags;
        void* pAddictionItem;
        float fAddictionChance;
        uint32_t pad14;
        void* pConsumptionSound;
    } data;

    uint8_t unk158[0x10];
};

static_assert(offsetof(AlchemyItem, data) == 0x138);
static_assert(sizeof(AlchemyItem) == 0x168);
