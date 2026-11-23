#pragma once

#include <Events/PacketEvent.h>

#include <Structs/MagicState.h>

#include <chrono>

struct World;
struct Player;
struct RequestMagicState;

//! On a player's character: what its owner last said it casts and wears as magic, and when (MagicStateService).
struct MagicStateComponent
{
    MagicState State{};
    std::chrono::steady_clock::time_point SaidAt{};
};

/**
 * @brief Keeps what each player casts and wears as magic, and sends it with their character to players who see them later.
 */
struct MagicStateService
{
    MagicStateService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~MagicStateService() noexcept = default;

    TP_NOCOPYMOVE(MagicStateService);

    //! Sent with the character (CharacterService::SendSpawn): the kept state, aged by the time since it was said.
    static void SendTo(World& aWorld, Player* apPlayer, entt::entity aEntity) noexcept;

protected:
    void OnMagicState(const PacketEvent<RequestMagicState>& acMessage) noexcept;

private:
    World& m_world;

    entt::scoped_connection m_magicStateConnection;
};
