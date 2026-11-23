#pragma once

#include <CombatTargets.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct NotifyCombatTarget;
struct CopyFightComponent;
struct HitEvent;
struct Actor;

/**
 * @brief Responsible for projectiles, combat agro, etc.
 *
 * Whom an NPC fights is its owner's: this game reads whom its own NPCs fight and tells the other games every 250 ms when it
 * changes. Their copies, which run no AI, then look at that target, and a copy with a combat controller of its own is
 * pointed at it too. A copy is never put into combat or out of it here.
 */
struct CombatService
{
    CombatService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~CombatService() noexcept = default;

    TP_NOCOPYMOVE(CombatService);

    //! A living NPC this game runs, which is not bleeding out: only the owner runs an NPC's AI, and players are never made to fight.
    static bool CanFight(Actor* apActor) noexcept;
    //! Sets an NPC that can fight on a target alive and drawn here; one already fighting is turned to it, never stopped first.
    bool SetOnTarget(Actor* apActor, Actor* apTarget) const noexcept;
    /**
     * Find whom an NPC this game runs fights, by server id.
     *
     * @param apActor The NPC.
     * @return Its target's server id; 0 for nobody, or for a target without one.
     */
    uint32_t FightOf(Actor* apActor) const noexcept;
    //! An NPC hit by another player fights that player too.
    void FightBack(Actor* apActor, uint32_t aAttackerPlayerId, float aDeltaHealth) const noexcept;
    //! An NPC attacked by another player, with or without damage, fights that player too.
    void FightBack(Actor* apActor, uint32_t aAttackerPlayerId) const noexcept;
    //! A character gained while its last owner's game had it fighting fights the same one here.
    void CarryOnFight(Actor* apActor, uint32_t aServerId, uint32_t aTargetServerId) const noexcept;
    //! A copy this game takes over stops following its last owner's target: its own AI runs it now.
    void ForgetCopyFight(entt::entity aEntity, Actor* apActor, const char* acpWhy) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept;
    void OnProjectileLaunchedEvent(const ProjectileLaunchedEvent& acEvent) const noexcept;
    void OnNotifyProjectileLaunch(const NotifyProjectileLaunch& acMessage) const noexcept;
    void OnHitEvent(const HitEvent& acEvent) const noexcept;
    void OnNotifyCombatTarget(const NotifyCombatTarget& acMessage) noexcept;

    void RunTargetUpdates(const float acDelta) const noexcept;

private:
    void ReadOwnFights(uint64_t aNowMs) noexcept;
    void UpdateCopyFights(uint64_t aNowMs) noexcept;
    //! Follows the owner's target on a copy; false when the word is to be dropped.
    bool ApplyCopyFight(Actor* apCopy, CopyFightComponent& aFight) const noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_localComponentRemoved;
    entt::scoped_connection m_projectileLaunchedConnection;
    entt::scoped_connection m_projectileLaunchConnection;
    entt::scoped_connection m_hitConnection;
    entt::scoped_connection m_combatTargetConnection;

    CombatTargets::Outbox m_fightOutbox;
    uint64_t m_nextFightRead{};
};
