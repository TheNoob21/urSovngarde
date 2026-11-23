#pragma once

#include <NpcHeadTrack.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyNpcHeadTrack;
struct Actor;

/**
 * @brief Where NPCs look, on every screen: the head turned to the player walking past, to the person it talks to, or to the
 * player it walks up to greet.
 *
 * The game that runs an NPC (its owner) has its AI write that look into the NPC's animation graph: the head-track flags and the
 * target point. Copies run no AI, so their heads stayed still. This game reads those graph values from each of its own NPCs
 * every 100 ms (a few NPCs each frame) and tells the other games when they change; their copies get the same values written into their graphs every
 * frame, eased so the head turns smoothly between words. Only the copy's graph is written: no package, no AI state.
 */
struct HeadTrackService
{
    HeadTrackService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~HeadTrackService() noexcept = default;

    TP_NOCOPYMOVE(HeadTrackService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyNpcHeadTrack(const NotifyNpcHeadTrack& acMessage) noexcept;

private:
    void StepOwnLooks(uint64_t aNowMs) noexcept;
    void SendOwnLooks(uint64_t aNowMs) noexcept;
    void UpdateCopyLooks(uint64_t aNowMs, float aDeltaMs) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_npcHeadTrackConnection;

    NpcHeadTrack::Outbox m_lookOutbox;
    //! This game's NPCs, read a few each frame; the words go out when the last one has been read.
    NpcHeadTrack::ReadRound m_lookRound;
    Vector<entt::entity> m_lookQueue;
};
