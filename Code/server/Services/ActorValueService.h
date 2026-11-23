#pragma once

#include <Events/PacketEvent.h>

struct World;
struct UpdateEvent;
struct TransportService;
struct RequestActorValueChanges;
struct RequestActorMaxValueChanges;
struct RequestHealthChangeBroadcast;
struct RequestDeathStateChange;

/**
 * @brief Broadcasts changes in (max) actor values and updates them server side.
 */
struct ActorValueService
{
    ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ActorValueService() noexcept = default;

    TP_NOCOPYMOVE(ActorValueService);

    // Gameplay:bEnableDragonSouls, sent to every client with the server's settings.
    static bool IsDragonSoulsOn() noexcept;

private:
    World& m_world;
    //! Placed dragons whose soul was given, by reference: a dragon's entity made again (left and announced anew) gives none again.
    mutable TiltedPhoques::Set<uint64_t> m_dragonSoulsGiven;

    void OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept;
    void OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept;
    void OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept;
    void OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept;

    entt::scoped_connection m_updateHealthConnection;
    entt::scoped_connection m_updateMaxValueConnection;
    entt::scoped_connection m_updateDeltaHealthConnection;
    entt::scoped_connection m_deathStateConnection;
};
