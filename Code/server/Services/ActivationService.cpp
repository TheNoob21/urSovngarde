#include <Services/ActivationService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/PlayerLeaveEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/NotifyActivationCatchUp.h>
#include <Messages/RequestActivationState.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Structs/DoorState.h>

#include <chrono>

namespace
{
// A player is known by name, so a reconnect is the same game; one with no name by its connection.
std::string GetPlayerKey(const Player* apPlayer) noexcept
{
    const String& cName = apPlayer->GetUsername();
    return cName.empty() ? "#" + std::to_string(apPlayer->GetId()) : std::string(cName.c_str());
}

// Indoors a player is in one cell: a use or an end state in another is not that player's.
bool IsInOwnCell(const Player* apPlayer, const GameId& acCell) noexcept
{
    const CellIdComponent cStanding = apPlayer->GetStandingCell();
    return !cStanding.IsInInteriorCell() || cStanding.Cell == acCell;
}

} // namespace

static_assert(ActivationLedger<CellIdComponent>::kMaxStateCatchUp == NotifyActivationCatchUp::kMaxStates);
static_assert(ActivationCatchUp::kMaxPoseValues == ActivationState::kMaxPoseValues);

ActivationService::ActivationService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_activateConnection(aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ActivationService::OnActivate>(this))
    , m_stateConnection(aDispatcher.sink<PacketEvent<RequestActivationState>>().connect<&ActivationService::OnActivationState>(this))
    , m_interiorConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&ActivationService::OnEnterInteriorCell>(this))
    , m_exteriorConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&ActivationService::OnEnterExteriorCell>(this))
    , m_gridConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&ActivationService::OnShiftGridCell>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&ActivationService::OnPlayerLeave>(this))
{
}

bool ActivationService::UsedRecently(const Player* apPlayer) const noexcept
{
    return apPlayer && m_ledger.UsedAnyRecently(GetPlayerKey(apPlayer), std::chrono::steady_clock::now());
}

void ActivationService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;

    // A door's state is kept by ObjectService, and set, not replayed.
    if (DoorState::AfterActivation(cMessage.PreActivationOpenState) != DoorState::kNone || cMessage.Id == GameId{} || cMessage.CellId == GameId{})
        return;

    // Only a player's own use: a mechanism is a player's to use, and NPCs' uses of furniture would crowd the ledger.
    const auto cCharacter = acMessage.pPlayer->GetCharacter();
    if (!cCharacter || World::ToInteger(*cCharacter) != cMessage.ActivatorId)
        return;

    if (!IsInOwnCell(acMessage.pPlayer, cMessage.CellId))
        return;

    m_ledger.NoteUse(GetPlayerKey(acMessage.pPlayer), cMessage.Id.LogFormat(), std::chrono::steady_clock::now());
    auto [doneBy, where] = PlayersNear(acMessage.pPlayer, cMessage.CellId);
    m_ledger.Record(cMessage.Id.LogFormat(), where, std::move(doneBy));
}

void ActivationService::OnActivationState(const PacketEvent<RequestActivationState>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    if (cMessage.State.Id == GameId{} || cMessage.CellId == GameId{} || !acMessage.pPlayer->GetCharacter())
        return;

    // Only for a mechanism this player used just now, in the cell this player is in.
    if (!IsInOwnCell(acMessage.pPlayer, cMessage.CellId) ||
        !m_ledger.UsedRecently(GetPlayerKey(acMessage.pPlayer), cMessage.State.Id.LogFormat(), std::chrono::steady_clock::now()))
        return;

    auto [doneBy, where] = PlayersNear(acMessage.pPlayer, cMessage.CellId);
    ActivationCatchUp::Pose pose(cMessage.State.Pose.begin(), cMessage.State.Pose.end());
    if (!m_ledger.RecordState(cMessage.State.Id.LogFormat(), where, std::move(pose), std::move(doneBy)))
        spdlog::debug("ActivationCatchUp: end state from player {} dropped, {} pose values", acMessage.pPlayer->GetId(), cMessage.State.Pose.size());
}

// Those ObjectService::OnActivate passes a use on to: the sender, those in its cell, and outdoors those with its grid loaded.
std::pair<std::vector<std::string>, CellIdComponent> ActivationService::PlayersNear(Player* apSender, const GameId& acCell) noexcept
{
    const CellIdComponent cSenderCell = apSender->GetStandingCell();
    std::vector<std::string> doneBy{GetPlayerKey(apSender)};
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == apSender)
            continue;

        const CellIdComponent& cCell = pPlayer->GetCellComponent();
        const bool cSameCell = cCell.Cell == acCell;
        const bool cLoadedThere = !cSenderCell.IsInInteriorCell() && !cCell.IsInInteriorCell() && cSenderCell.IsInRange(cCell, false);
        if (cSameCell || cLoadedThere)
            doneBy.push_back(GetPlayerKey(pPlayer));
    }

    const CellIdComponent cWhere = cSenderCell.IsInInteriorCell() ? CellIdComponent{acCell} : cSenderCell;
    return {std::move(doneBy), cWhere};
}

// Reads the packet, not the player's cell component, which another service updates from the same packet in no set order.
void ActivationService::OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) noexcept
{
    SendCatchUp(acMessage.pPlayer, CellIdComponent{acMessage.Packet.CellId});
}

void ActivationService::OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    SendCatchUp(acMessage.pPlayer, CellIdComponent{cMessage.CellId, cMessage.WorldSpaceId, cMessage.CurrentCoords});
}

void ActivationService::OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    SendCatchUp(acMessage.pPlayer, CellIdComponent{cMessage.PlayerCell, cMessage.WorldSpaceId, cMessage.CenterCoords});
}

void ActivationService::SendCatchUp(Player* apPlayer, const CellIdComponent& acViewer) noexcept
{
    if (m_ledger.Size() == 0)
        return;

    const auto cInRange = [&acViewer](const CellIdComponent& acWhere)
    {
        return acWhere.IsInRange(acViewer, false);
    };
    const std::string cPlayer = GetPlayerKey(apPlayer);
    // States first: a use of a reference with a state is then passed over.
    const auto cStates = m_ledger.CatchUpStates(cPlayer, cInRange);
    const auto cRefs = m_ledger.CatchUp(cPlayer, cInRange);
    if (cRefs.empty() && cStates.empty())
        return;

    const auto cToGameId = [](uint64_t aRef)
    {
        return GameId(static_cast<uint32_t>(aRef >> 32), static_cast<uint32_t>(aRef));
    };
    NotifyActivationCatchUp notify{};
    for (const uint64_t cRef : cRefs)
        notify.Activations.push_back(cToGameId(cRef));
    for (const auto& cState : cStates)
    {
        ActivationState& state = notify.States.emplace_back();
        state.Id = cToGameId(cState.Ref);
        state.Pose.assign(cState.Pose.begin(), cState.Pose.end());
    }
    apPlayer->Send(notify);

    spdlog::debug(
        "ActivationCatchUp: {} use(s) and {} end state(s) from before player {} came near sent to them; {} kept", cRefs.size(), cStates.size(), apPlayer->GetId(), m_ledger.Size());
}

void ActivationService::OnPlayerLeave(const PlayerLeaveEvent&) noexcept
{
    // The last player out ends the session: the next one starts from the players' saves. The leaver is still counted here.
    if (m_world.GetPlayerManager().Count() <= 1)
        m_ledger.Clear();
}
