#include "ArtObjects.h"

bool ArtObjects::Play(TESObjectREFR* apRef, BGSArtObject* apArt, NiAVObject* apNode, const float aSeconds) noexcept
{
    // TESObjectREFR::ApplyArtObject; it ends in the call that starts the effect and returns that call's bool.
    using TApplyArtObject = bool(TESObjectREFR*, BGSArtObject*, float, TESObjectREFR*, bool, bool, NiAVObject*, bool);
    POINTER_SKYRIMSE(TApplyArtObject, s_applyArtObject, 22769, 22289);

    if (!apRef || !apArt || !s_applyArtObject.Get())
        return false;
    return s_applyArtObject(apRef, apArt, aSeconds, nullptr, false, false, apNode, false);
}
