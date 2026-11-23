#pragma once

#include <cstdint>

struct Actor;
struct NotifyDragonSoul;

// bEnableDragonSouls off on the server (which tells every client) leaves the game's own absorb.
// A dragon's soul for every player of the party, once per dragon (DragonSoul.h, urSovngardeDragonSoulEffect.psc).
// Every dragon this game holds is kept out of the game's own absorb; the server's word that one died gives this player its soul:
// absorbed if this player comes near while it is loaded here, as alone, or given without the absorb.
namespace DragonSouls
{
// A dragon this game holds, whoever runs it: kept out of the game's own absorb, before it can die (game thread).
void Hold(Actor* apDragon) noexcept;

// Holds the dragons that arrived since, and gives the souls waiting for this player to come near (game thread, with the update).
void Update() noexcept;

// The server's word that a dragon died (game thread).
void OnNotify(const NotifyDragonSoul& acMessage) noexcept;

// The next soul for the effect script (script thread): a dragon's form id to absorb, -1 for a soul without the absorb, 0 for none.
int32_t Take() noexcept;

// Cleanup is not switch-gated: even after disabling, any tracked faction changes must be restored.
// Kept out of the game's own absorb only while connected: a save never has it (the save guard, Actor.cpp), nor the game after a disconnect.
// Not lifted from a body whose soul this player had, so a solo game cannot absorb it a second time.
bool LiftForSave(Actor* apActor) noexcept;
void RestoreAfterSave(Actor* apActor) noexcept;

// The dragons this game put out of the game's own absorb are back in it, then everything is forgotten (game thread, at a disconnect).
void Reset() noexcept;
} // namespace DragonSouls
