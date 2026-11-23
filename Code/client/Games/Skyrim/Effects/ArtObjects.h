#pragma once

struct TESObjectREFR;
struct BGSArtObject;
struct NiAVObject;

//! Art objects the game plays on a reference, as it plays a hand's casting art.
namespace ArtObjects
{
//! Plays apArt on apRef at apNode for aSeconds, after which the game ends it. True when the game started it.
//! The game gives back only whether it started the effect, never the effect, so nothing of it is held.
bool Play(TESObjectREFR* apRef, BGSArtObject* apArt, NiAVObject* apNode, float aSeconds) noexcept;
} // namespace ArtObjects
