#pragma once

#include <Events/PacketEvent.h>
#include <CrimeReports.h>
#include <HitReactions.h>

struct World;
struct ProjectileLaunchRequest;
struct RequestHitReaction;
struct RequestDetectionLevels;
struct RequestCrimeReport;
struct RequestCombatTarget;

struct CombatService
{
    CombatService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CombatService() noexcept = default;

    TP_NOCOPYMOVE(CombatService);

protected:
    void OnProjectileLaunchRequest(const PacketEvent<ProjectileLaunchRequest>& acMessage) const noexcept;
    void OnHitReactionRequest(const PacketEvent<RequestHitReaction>& acMessage) noexcept;
    void OnDetectionLevels(const PacketEvent<RequestDetectionLevels>& acMessage) noexcept;
    void OnCrimeReport(const PacketEvent<RequestCrimeReport>& acMessage) noexcept;
    void OnCombatTargets(const PacketEvent<RequestCombatTarget>& acMessage) noexcept;

private:
    World& m_world;

    entt::scoped_connection m_projectileLaunchConnection;
    entt::scoped_connection m_hitReactionConnection;
    entt::scoped_connection m_detectionConnection;
    entt::scoped_connection m_crimeReportConnection;
    entt::scoped_connection m_combatTargetConnection;

    //! Reactions taken per actor and sending player (actor id << 32 | player id) in the current second.
    TiltedPhoques::Map<uint64_t, HitReactions::RateCount> m_reactionCounts;
    //! Crime reports taken from each player in the current second.
    TiltedPhoques::Map<uint32_t, CrimeReports::RateCount> m_crimeReportCounts;
};
