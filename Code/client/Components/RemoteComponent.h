#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>

#include <limits>

struct RemoteComponent
{
    RemoteComponent(uint32_t aId, uint32_t aRefId, uint32_t aOwnershipEpoch) noexcept
        : Id(aId)
        , CachedRefId(aRefId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    // Set through emplace, replace or patch only: World keeps an index of server ids.
    uint32_t Id;
    uint32_t CachedRefId;
    uint32_t OwnershipEpoch;
    //! The base form of the copy this client made for this character, or 0.
    //! The game reuses a deleted copy's form id; a changed base means CachedRefId now names another actor.
    //! See CharacterService::RunSpawnUpdates.
    uint32_t CachedBaseId{};
    //! The owner's game has said this actor is dead: it came dead, or its death arrived (the death message, or the server's spawn data).
    //! A copy can die here alone; its corpse is then not placed where its owner has the living actor (InterpolationSystem::Update).
    //! A health broadcast that kills the copy does not count: the copy's health can be lower than the owner's.
    bool DeadAtOwner{};
    //! The health its owner's game last reported for it (ActorValueService::OnActorValueChanges), or NaN before any came.
    //! An NPC copy's own health is kept above its owner's 0 and heals here, so this is what says its owner's is at 0.
    float OwnerHealth{std::numeric_limits<float>::quiet_NaN()};
    //! This game ran it, saw it die and handed it on before the server took the death; its new owner is sent the killing blow.
    //! Until that owner's death arrives, a copy that goes is still a body. Read only for that, not for where its corpse lies.
    bool DeadHere{};
    //! The party leader's game did not claim it on arrival, as it was far from this player; it is claimed once it comes near (CharacterService::RunLeaderClaims).
    bool ClaimWhenNear{};
};
