#pragma once

#include <Events/PacketEvent.h>
#include <ContainerLedger.h>

#include <TrapSync.h>

struct World;
struct PlayerLeaveCellEvent;
struct PlayerLeaveEvent;
struct ActivateRequest;
struct LockChangeRequest;
struct AssignObjectsRequest;
struct ScriptAnimationRequest;
struct RequestReferenceEnable;
struct RequestObjectInventoryChanges;
struct RequestTrapFire;

/**
 * @brief Manages (interactive) objects and relays interactions with said objects.
 */
class ObjectService
{
public:
    ObjectService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>&) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>&) const noexcept;
    void OnLockChange(const PacketEvent<LockChangeRequest>&) const noexcept;
    void OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>&) noexcept;
    void OnReferenceEnableRequest(const PacketEvent<RequestReferenceEnable>&) noexcept;
    void OnObjectInventoryChanges(const PacketEvent<RequestObjectInventoryChanges>&) noexcept;
    void OnTrapFire(const PacketEvent<RequestTrapFire>&) noexcept;

    World& m_world;
    // What the containers held when the server forgot them, for the next game in.
    ContainerLedger m_containerLedger;

    entt::scoped_connection m_leaveCellConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_referenceEnableConnection;
    entt::scoped_connection m_objectInventoryConnection;
    entt::scoped_connection m_trapFireConnection;

    // Traps relayed per player, a handful a second at most (TrapSync::RateLimit).
    Map<uint32_t, TrapSync::RateLimit> m_trapFireRates;
    // Switches taken from members other than the leader, per player, as many messages a second at most.
    Map<uint32_t, TrapSync::RateLimit> m_memberSwitchRates;
};
