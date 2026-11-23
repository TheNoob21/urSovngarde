#pragma once

#include <Events/PacketEvent.h>

#include <ActivationLedger.h>
#include <Components.h>
#include <Structs/GameId.h>

#include <string>
#include <utility>
#include <vector>

struct World;
struct Player;
struct ActivateRequest;
struct RequestActivationState;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct ShiftGridCellRequest;
struct PlayerLeaveEvent;

/**
 * @brief Keeps the session's activations for a player who was not near them, and sends them as that player comes near.
 *
 * ObjectService passes each activation on live; a game that has the reference's cell loaded later still has it as before.
 * Doors are left to ObjectService, which keeps their state. The receiving game replays only the mechanisms (ActivationCatchUp.h).
 * A lever or a pillar whose user reported its end state goes as that state, which the receiving game sets.
 */
class ActivationService
{
public:
    ActivationService(World& aWorld, entt::dispatcher& aDispatcher);

    TP_NOCOPYMOVE(ActivationService);

    //! Whether the player used something of their own cell lately: its script's switches may be theirs to share (SharedHomes).
    [[nodiscard]] bool UsedRecently(const Player* apPlayer) const noexcept;

private:
    void OnActivate(const PacketEvent<ActivateRequest>& acMessage) noexcept;
    void OnActivationState(const PacketEvent<RequestActivationState>& acMessage) noexcept;
    void OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) noexcept;
    void OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept;
    void OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    void SendCatchUp(Player* apPlayer, const CellIdComponent& acViewer) noexcept;
    //! The players whose games have a reference in aCell loaded as apSender uses it, apSender first; and where it is.
    std::pair<std::vector<std::string>, CellIdComponent> PlayersNear(Player* apSender, const GameId& acCell) noexcept;

    World& m_world;
    ActivationLedger<CellIdComponent> m_ledger;

    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_stateConnection;
    entt::scoped_connection m_interiorConnection;
    entt::scoped_connection m_exteriorConnection;
    entt::scoped_connection m_gridConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
