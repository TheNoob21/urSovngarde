#pragma once

#include <Messages/NotifyPlayerWhereabouts.h>

struct World;
struct UpdateEvent;

/**
 * @brief Tells every player where every player is (NotifyPlayerWhereabouts): four times a second while someone moves, every two seconds otherwise.
 * Clients already know who is in their party.
 */
struct WhereaboutsService
{
    WhereaboutsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~WhereaboutsService() noexcept = default;

    TP_NOCOPYMOVE(WhereaboutsService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

private:
    World& m_world;
    std::chrono::steady_clock::time_point m_nextLook{};
    std::chrono::steady_clock::time_point m_lastSent{};
    NotifyPlayerWhereabouts m_last{};

    entt::scoped_connection m_updateConnection;
};
