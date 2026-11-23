#pragma once

#include <Events/PacketEvent.h>

struct World;
struct RequestNpcHeadTrack;

/**
 * @brief Passes on where NPCs look, from the game that runs them to the other games near them.
 */
struct HeadTrackService
{
    HeadTrackService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~HeadTrackService() noexcept = default;

    TP_NOCOPYMOVE(HeadTrackService);

protected:
    void OnNpcHeadTrack(const PacketEvent<RequestNpcHeadTrack>& acMessage) noexcept;

private:
    World& m_world;

    entt::scoped_connection m_npcHeadTrackConnection;
};
