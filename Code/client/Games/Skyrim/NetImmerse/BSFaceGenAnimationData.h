#pragma once

#include <cstddef>
#include <cstdint>

#include <Games/Skyrim/BSSpinLock.h>

//! One set of face morph weights (expressions, modifiers or phonemes) of a face.
struct BSFaceGenKeyframeMultiple
{
    void* pVtbl;
    uint32_t type;
    float unk0C;
    float* pValues;
    uint32_t count;
    //! Cleared so the face takes the new weights at its next update.
    bool isUpdated;
};

static_assert(sizeof(BSFaceGenKeyframeMultiple) == 0x20);

//! The face's animation data: the morph weights its head is posed with.
struct BSFaceGenAnimationData
{
    //! Sets one phoneme's weight, 0 to 1, as the console's mfg phoneme does.
    void SetPhoneme(const uint32_t aIndex, const float aWeight) noexcept
    {
        BSScopedSpinLock guard(lock);
        if (!phonemes.pValues || aIndex >= phonemes.count)
            return;
        phonemes.pValues[aIndex] = aWeight;
        phonemes.isUpdated = false;
    }

    uint8_t unk0[0x80];
    BSFaceGenKeyframeMultiple phonemes;
    uint8_t unkA0[0x180];
    BSSpinLock lock;
};

static_assert(offsetof(BSFaceGenAnimationData, phonemes) == 0x80);
static_assert(offsetof(BSFaceGenAnimationData, lock) == 0x220);
