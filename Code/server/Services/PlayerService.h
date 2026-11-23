#pragma once

#include <Events/PacketEvent.h>
#include <ViewerResends.h>

struct World;
struct PlayerLeaveEvent;
struct ShiftGridCellRequest;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct PlayerRespawnRequest;
struct PlayerLevelRequest;
struct RequestNpcPlugins;

/**
 * @brief Handles player specific actions that might change the information needed by other clients about that player.
 */
struct PlayerService
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);

    void Rename(Player& aPlayer, const String& acName) const noexcept;

protected:
    void HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept;
    void HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept;
    void HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept;
    void OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept;
    void OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept;
    void OnNpcPlugins(const PacketEvent<RequestNpcPlugins>& acMessage) const noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) const noexcept;

private:
    void RemoveWhatLeftRange(Player* apPlayer, const CellIdComponent& acPrevious) const noexcept;

    World& m_world;
    //! The characters each player was last sent at a shift of its loaded cells, by player id then server id (ViewerResends).
    mutable TiltedPhoques::Map<uint32_t, TiltedPhoques::Map<uint32_t, ViewerResends::Record>> m_spawnsSent;

    entt::scoped_connection m_gridCellShiftConnection;
    entt::scoped_connection m_exteriorCellEnterConnection;
    entt::scoped_connection m_interiorCellEnterConnection;
    entt::scoped_connection m_playerRespawnConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_npcPluginsConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
