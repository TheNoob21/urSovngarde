#pragma once

#include <cstdint>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

struct NotifyStuckArrow;
struct NotifyDismember;
struct UpdateEvent;
struct Actor;
struct TESObjectREFR;
struct TESForm;

//! Arrows stuck in an actor and limbs cut off it in the other games, shown here too.
//! Arrows stuck in a dead body go as its ammunition of their kind is taken, in every game.
namespace StuckArrows
{
//! An arrow another game stuck in an actor: kept for the actor, at most three, the oldest dropped (game thread).
void OnNotifyStuckArrow(const NotifyStuckArrow& acMessage) noexcept;

//! Limbs another game cut off an actor: added to what is kept for it (game thread).
void OnNotifyDismember(const NotifyDismember& acMessage) noexcept;

//! The limbs the server keeps for an actor, from its spawn or assignment data (game thread).
void OnSpawnLimbs(uint32_t aServerId, uint16_t aLimbs) noexcept;

//! The actor was brought back by its owner: the limbs and arrows told for it wait no more (game thread).
void OnRevived(uint32_t aServerId) noexcept;

//! This game's copy of the actor is gone, or the server let go of its id: everything kept for it is forgotten (game thread).
//! A copy made again is told its limbs again by its spawn data.
void OnRetired(uint32_t aServerId, uint32_t aFormId = 0) noexcept;

//! The limbs kept for an actor by its server id; 0 for none.
uint16_t LimbsOf(uint32_t aServerId) noexcept;

//! How many arrows are kept for an actor by its server id.
uint32_t PendingArrowCount(uint32_t aServerId) noexcept;

//! An arrow of this player's stuck in an actor this game runs: sent to the server for the other games.
//! The position is in the space of the bone, before the bone's scale (StuckArrowPlacement::ToBoneLocal).
void SendStuckArrow(
    uint32_t aServerId, uint32_t aAmmoModId, uint32_t aAmmoBaseId, const char* apBone, const glm::vec3& acPosition, const glm::quat& acRotation, float aScale, uint32_t aSequence,
    bool aRemoved = false) noexcept;

//! Limbs cut off an actor this game runs: sent to the server for the other games.
void SendDismember(uint32_t aServerId, uint16_t aLimbs, uint8_t aLastHitLimb) noexcept;

//! The game cut the head off an actor (Actor::Decapitate), on whatever thread called it: sent from the next frame when this game
//! runs the actor and this code did not cut it.
void OnDecapitated(Actor* apActor) noexcept;

// Native lifetime boundary, before Resurrect can reenter scripts. Reads no entity registry.
void OnNativeRevival(uint32_t aFormId) noexcept;

// Held around the actual native RemoveItem call, including replicated incremental changes.
// Bulk inventory replacement suppresses these intermediate removals.
struct AmmoRemoval
{
    AmmoRemoval(TESObjectREFR* apOwner, TESForm* apAmmo) noexcept;
    ~AmmoRemoval();
    Actor* Owner{};
    TESForm* Ammo{};
    uintptr_t Root{};
    uint32_t Projectile{};
    int64_t Before{};
    uint64_t Session{};
    uint64_t Life{};
};
struct InventoryReplacement
{
    InventoryReplacement() noexcept;
    ~InventoryReplacement();
};

// Native scene work is drained only at the actual main loop boundary, outside loading menus.
void OnMainLoop() noexcept;

//! Each frame: what other games did to actors put on this game's copies of them (game thread).
void OnUpdate(const UpdateEvent& acEvent) noexcept;

//! Everything forgotten, at a disconnect.
void Reset() noexcept;
} // namespace StuckArrows
