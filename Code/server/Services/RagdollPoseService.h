#pragma once

#include <Events/PacketEvent.h>

#include <CorpseRestPose.h>
#include <RagdollPoseRelay.h>

struct World;
struct Player;
struct RequestRagdollPose;
struct CharacterRemoveEvent;

/**
 * @brief Passes where the bodies of a moved creature's ragdoll are, from the game that runs it to the players near it (RagdollPoseRelay).
 */
struct RagdollPoseService
{
    RagdollPoseService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~RagdollPoseService() noexcept = default;

    TP_NOCOPYMOVE(RagdollPoseService);

    //! Sends a player where a dead creature's bodies came to rest at its owner's, for its copy to lie so (CorpseRestPose).
    //! Nothing when none are kept.
    void SendRestBodies(Player* apPlayer, entt::entity aEntity) const noexcept;
    //! The creature lives again: the bodies it last rested in are dropped (CorpseRestPose).
    void ForgetRestBodies(entt::entity aEntity) noexcept;

protected:
    void OnRagdollPose(const PacketEvent<RequestRagdollPose>& acMessage) noexcept;
    void OnCharacterRemove(const CharacterRemoveEvent& acEvent) noexcept;

private:
    World& m_world;

    // The last poses passed on, by the actor's entity id.
    TiltedPhoques::Map<uint32_t, RagdollPoseRelay::Relayed> m_relayed;
    // Where each dead creature's bodies came to rest at its owner's, by entity id, for the players sent the body later.
    CorpseRestPose::Store<RagdollPose> m_restBodies;

    entt::scoped_connection m_ragdollPoseConnection;
    entt::scoped_connection m_characterRemoveConnection;
};
