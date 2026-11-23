#pragma once

#include "ExtraData.h"

#include <NetImmerse/NiPointer.h>

#include <cstddef>

struct NiAVObject;
struct TESForm;

//! The arrows stuck in a reference, as the engine lists them; their 3D hangs under the node each struck.
struct ExtraAttachedArrows3D : BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::AttachedArrows3D;

    struct Item
    {
        NiPointer<NiAVObject> arrow3D;
        uint64_t timeStamp;
        // BGSProjectile
        TESForm* source;
    };

    GameArray<Item> data;
    uint16_t nextStorage;
    uint16_t oldestStorage;
};

static_assert(sizeof(ExtraAttachedArrows3D::Item) == 0x18);
static_assert(offsetof(ExtraAttachedArrows3D, data) == 0x10);
static_assert(sizeof(ExtraAttachedArrows3D) == 0x30);
