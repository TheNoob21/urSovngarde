#pragma once

#include "NiLayout.h"

struct NiMatrix3
{
    float data[3][3];
};
static_assert(sizeof(NiMatrix3) == 0x24);

struct NiTransform
{
    NiMatrix3 rotate;
    NiPoint3 translate;
    float scale;
};

//! An object's world transform, which NiAVObject keeps at NiLayout::kWorldOffset.
inline const NiTransform& WorldTransformOf(const void* apObject) noexcept
{
    return *reinterpret_cast<const NiTransform*>(static_cast<const uint8_t*>(apObject) + NiLayout::kWorldOffset);
}
