#pragma once

#include <Events/PacketEvent.h>

#include <Components.h>
#include <ReferenceStateLedger.h>
#include <Structs/GameId.h>
#include <TrapSync.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct World;
struct Player;
struct UpdateEvent;
struct ActivateRequest;
struct RequestReferenceState;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct ShiftGridCellRequest;
struct PlayerLeaveEvent;
struct RequestSaveLoaded;

/**
 * @brief Passes on the world reference states a player's game changed (ReferenceStates.h), and keeps them for the players
 * who come near later.
 *
 * A report is taken from a player who stands in the reference's cell and either used it just now or stands near it. The
 * states that come back when a cell resets are kept with the game hours since a player last had them loaded, which the
 * receiving game weighs against its own respawn time.
 */
class ReferenceStateService
{
public:
    ReferenceStateService(World& aWorld, entt::dispatcher& aDispatcher);

    TP_NOCOPYMOVE(ReferenceStateService);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>& acMessage) noexcept;
    void OnReferenceState(const PacketEvent<RequestReferenceState>& acMessage) noexcept;
    void OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) noexcept;
    void OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept;
    void OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    //! The player's game loaded a save: it is sent the kept states of its place again (LoadCatchUpLimit).
    void OnSaveLoaded(const PacketEvent<RequestSaveLoaded>& acMessage) noexcept;

    void SendCatchUp(Player* apPlayer, const CellIdComponent& acViewer) noexcept;
    //! The kept states some player other than apExcept has loaded count as seen now.
    void NoteSeen(const Player* apExcept) noexcept;
    //! The players whose games have the reference loaded as apSender reports it, apSender first.
    std::vector<Player*> PlayersNear(Player* apSender, const CellIdComponent& acWhere) noexcept;

    World& m_world;
    ReferenceStateLedger<CellIdComponent> m_ledger;
    std::unordered_map<uint32_t, TrapSync::RateLimit> m_rates;
    LoadCatchUpLimit m_loadCatchUps;
    float m_sinceSeen{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_stateConnection;
    entt::scoped_connection m_interiorConnection;
    entt::scoped_connection m_exteriorConnection;
    entt::scoped_connection m_gridConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_saveLoadedConnection;
};
