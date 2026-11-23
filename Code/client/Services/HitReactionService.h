#pragma once

#include <HitReactions.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyHitReaction;
struct Actor;

/**
 * @brief Another player's hit staggers or recoils the NPC it hit, on every screen.
 *
 * The hitter's game says what its hit did to an NPC copy: the stagger or recoil the game wanted to play on it,
 * or, when the game wanted none, what a power attack or bash does. The NPC's owner plays it as the game's own action,
 * and the replay of that action brings it to every copy.
 */
struct HitReactionService
{
    HitReactionService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~HitReactionService() noexcept = default;

    TP_NOCOPYMOVE(HitReactionService);

    //! A copy wanted to play a reaction here, which only its owner's actions may (HookPerformAction). Any thread.
    static void OnCopyReaction(uint32_t aFormId, HitReactions::Kind aKind) noexcept;
    //! A stagger started on an NPC copy's graph, held back for its owner (AnimationGraphEvents). Any thread.
    static void OnCopyRawStagger(uint32_t aFormId) noexcept;
    //! This game's player hit an NPC copy (the game's hit event). Any thread.
    static void OnCopyHit(uint32_t aFormId, uint8_t aFlags) noexcept;

    uint32_t PresentReaction(uint32_t aId, uint32_t aEpoch, uint32_t aKind, float aMagnitude, float aDirection, uint32_t aObserver);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyHitReaction(const NotifyHitReaction& acMessage) noexcept;

private:
    void Send(Actor* apActor, uint32_t aServerId, HitReactions::Kind aKind, float aMagnitude, float aDirection, const char* acpWhy) noexcept;
    bool Play(Actor* apActor, HitReactions::Kind aKind, float aMagnitude, float aDirection) noexcept;

    World& m_world;
    TransportService& m_transport;

    //! When this game last sent a reaction for a copy, by form id.
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_sentAt;
    //! When the owner last played one on its actor, by form id.
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_playedAt;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_notifyConnection;
};
