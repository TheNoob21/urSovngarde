#include <Services/ReferenceStateService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/NotifyReferenceStates.h>
#include <Messages/RequestReferenceState.h>
#include <Messages/RequestSaveLoaded.h>
#include <Messages/ShiftGridCellRequest.h>

#include <chrono>

namespace
{
// A player is known by connection, never by name: two players may share one, and one who comes back may load an older save.
std::string PlayerKeyOf(const Player* apPlayer) noexcept
{
    return "#" + std::to_string(apPlayer->GetId());
}

GameId GameIdOf(uint64_t aRef) noexcept
{
    return GameId(static_cast<uint32_t>(aRef >> 32), static_cast<uint32_t>(aRef));
}

// The places the players' games have loaded are refreshed this often, so one a player stays in keeps counting as seen.
constexpr float kSeenInterval = 10.f;
} // namespace

static_assert(ReferenceStateLedger<CellIdComponent>::kMaxCatchUp == NotifyReferenceStates::kMax);

ReferenceStateService::ReferenceStateService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&ReferenceStateService::OnUpdate>(this))
    , m_activateConnection(aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ReferenceStateService::OnActivate>(this))
    , m_stateConnection(aDispatcher.sink<PacketEvent<RequestReferenceState>>().connect<&ReferenceStateService::OnReferenceState>(this))
    , m_interiorConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&ReferenceStateService::OnEnterInteriorCell>(this))
    , m_exteriorConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&ReferenceStateService::OnEnterExteriorCell>(this))
    , m_gridConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&ReferenceStateService::OnShiftGridCell>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&ReferenceStateService::OnPlayerLeave>(this))
    , m_saveLoadedConnection(aDispatcher.sink<PacketEvent<RequestSaveLoaded>>().connect<&ReferenceStateService::OnSaveLoaded>(this))
{
}

void ReferenceStateService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_sinceSeen += acEvent.Delta;
    if (m_sinceSeen < kSeenInterval)
        return;
    m_sinceSeen = 0.f;
    if (m_ledger.Size() > 0)
        NoteSeen(nullptr);
}

// A player's own use of a reference is that player's interaction with it: picking a plant, mining a vein, using a trap.
void ReferenceStateService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    const auto cCharacter = acMessage.pPlayer->GetCharacter();
    if (!cCharacter || World::ToInteger(*cCharacter) != cMessage.ActivatorId || cMessage.Id == GameId{})
        return;

    m_ledger.NoteUse(PlayerKeyOf(acMessage.pPlayer), cMessage.Id.LogFormat(), std::chrono::steady_clock::now());
}

void ReferenceStateService::OnReferenceState(const PacketEvent<RequestReferenceState>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    Player* pSender = acMessage.pPlayer;
    const auto cCharacter = pSender->GetCharacter();
    if (!cCharacter || cMessage.State.Id == GameId{} || cMessage.CellId == GameId{})
        return;

    const uint64_t cRef = cMessage.State.Id.LogFormat();
    const auto cKind = static_cast<ReferenceStates::Kind>(cMessage.State.Kind);
    const CellIdComponent cWhere{cMessage.CellId, cMessage.WorldSpaceId, cMessage.CenterCoords};
    const std::string cSender = PlayerKeyOf(pSender);

    // Where the server has the sender's own character, to weigh against where the reference is.
    std::optional<ReferenceStates::Point> sender;
    if (const auto* pMovement = m_world.valid(*cCharacter) ? m_world.try_get<MovementComponent>(*cCharacter) : nullptr)
        sender = ReferenceStates::Point{pMovement->Position.x, pMovement->Position.y, pMovement->Position.z};
    const ReferenceStates::Point cClaimed{cMessage.Position.x, cMessage.Position.y, cMessage.Position.z};

    // A count lasts a second, so the players' counts are dropped once there are many. Every report counts, taken or not.
    if (m_rates.size() > 64)
        m_rates.clear();
    if (!m_rates[pSender->GetId()].Allow(std::chrono::steady_clock::now()))
    {
        spdlog::debug(
            "ReferenceStates: {:X}:{:X} from player {} not taken, more than {} a second", cMessage.State.Id.ModId, cMessage.State.Id.BaseId, pSender->GetId(),
            TrapSync::RateLimit::kPerSecond);
        return;
    }

    const bool cInCell = pSender->GetStandingCell().IsInRange(cWhere, false);
    const auto cRefusal = m_ledger.Judge(cSender, cRef, cMessage.State.Kind, cMessage.State.Value, cInCell, sender, cClaimed, std::chrono::steady_clock::now());
    if (cRefusal != ReferenceStates::Refusal::None)
    {
        spdlog::debug(
            "ReferenceStates: {:X}:{:X} kind {} value {} from player {} not taken, refusal {}", cMessage.State.Id.ModId, cMessage.State.Id.BaseId,
            static_cast<int>(cMessage.State.Kind), static_cast<int>(cMessage.State.Value), pSender->GetId(), static_cast<int>(cRefusal));
        return;
    }

    const std::vector<Player*> cNear = PlayersNear(pSender, cWhere);
    std::vector<std::string> doneBy;
    for (const Player* pPlayer : cNear)
        doneBy.push_back(PlayerKeyOf(pPlayer));

    using Result = ReferenceStateLedger<CellIdComponent>::Result;
    const Result cResult = m_ledger.Record(cRef, cKind, cMessage.State.Value, cWhere, cClaimed, std::move(doneBy), m_world.GetCalendarService().GetGameHours(), cNear.size() > 1);
    if (cResult == Result::Behind)
    {
        NotifyReferenceStates notify{};
        ReferenceState& state = notify.States.emplace_back();
        state.Id = cMessage.State.Id;
        state.Kind = cMessage.State.Kind;
        state.Value = m_ledger.ValueOf(cRef, cKind).value_or(cMessage.State.Value);
        pSender->Send(notify);
        return;
    }
    if (cResult != Result::Changed)
    {
        spdlog::debug(
            "ReferenceStates: {:X}:{:X} kind {} value {} from player {}: {}", cMessage.State.Id.ModId, cMessage.State.Id.BaseId, static_cast<int>(cMessage.State.Kind),
            static_cast<int>(cMessage.State.Value), pSender->GetId(), cResult == Result::Ended ? "back as new, no longer kept" : "kept already, or loaded by another player");
        return;
    }

    NotifyReferenceStates notify{};
    ReferenceState& state = notify.States.emplace_back();
    state.Id = cMessage.State.Id;
    state.Kind = cMessage.State.Kind;
    state.Value = cMessage.State.Value;
    for (Player* pPlayer : cNear)
    {
        if (pPlayer != pSender)
            pPlayer->Send(notify);
    }

    spdlog::debug(
        "ReferenceStates: {:X}:{:X} kind {} value {} from player {} passed on to {} player(s); {} kept", cMessage.State.Id.ModId, cMessage.State.Id.BaseId,
        static_cast<int>(cMessage.State.Kind), static_cast<int>(cMessage.State.Value), pSender->GetId(), cNear.size() - 1, m_ledger.Size());
}

// Those ObjectService::OnActivate passes a use on to: the sender, those in the reference's cell, and outdoors those with its
// grid loaded.
std::vector<Player*> ReferenceStateService::PlayersNear(Player* apSender, const CellIdComponent& acWhere) noexcept
{
    std::vector<Player*> players{apSender};
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != apSender && acWhere.IsInRange(pPlayer->GetCellComponent(), false))
            players.push_back(pPlayer);
    }
    return players;
}

void ReferenceStateService::NoteSeen(const Player* apExcept) noexcept
{
    std::vector<CellIdComponent> loaded;
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != apExcept)
            loaded.push_back(pPlayer->GetCellComponent());
    }
    if (loaded.empty())
        return;

    const auto cLoadedThere = [&loaded](const CellIdComponent& acWhere)
    {
        return std::any_of(loaded.begin(), loaded.end(), [&acWhere](const CellIdComponent& acCell) { return acWhere.IsInRange(acCell, false); });
    };
    m_ledger.Seen(cLoadedThere, m_world.GetCalendarService().GetGameHours());
}

// Reads the packet, not the player's cell component, which another service updates from the same packet in no set order.
void ReferenceStateService::OnEnterInteriorCell(const PacketEvent<EnterInteriorCellRequest>& acMessage) noexcept
{
    SendCatchUp(acMessage.pPlayer, CellIdComponent{acMessage.Packet.CellId});
}

void ReferenceStateService::OnEnterExteriorCell(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    SendCatchUp(acMessage.pPlayer, CellIdComponent{cMessage.CellId, cMessage.WorldSpaceId, cMessage.CurrentCoords});
}

void ReferenceStateService::OnShiftGridCell(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept
{
    const auto& cMessage = acMessage.Packet;
    SendCatchUp(acMessage.pPlayer, CellIdComponent{cMessage.PlayerCell, cMessage.WorldSpaceId, cMessage.CenterCoords});
}

void ReferenceStateService::SendCatchUp(Player* apPlayer, const CellIdComponent& acViewer) noexcept
{
    if (m_ledger.Size() == 0)
        return;

    // A place another player has loaded is seen now; this player's game was not there.
    NoteSeen(apPlayer);
    const auto cStates = m_ledger.CatchUp(
        PlayerKeyOf(apPlayer), [&acViewer](const CellIdComponent& acWhere) { return acWhere.IsInRange(acViewer, false); }, m_world.GetCalendarService().GetGameHours());
    if (cStates.empty())
        return;

    NotifyReferenceStates notify{};
    for (const auto& cState : cStates)
    {
        ReferenceState& state = notify.States.emplace_back();
        state.Id = GameIdOf(cState.Ref);
        state.Kind = static_cast<uint8_t>(cState.What);
        state.Value = cState.Value;
        state.IdleHours = cState.IdleHours;
    }
    apPlayer->Send(notify);

    spdlog::debug("ReferenceStates: {} kept state(s) from before player {} came near sent to them; {} kept", cStates.size(), apPlayer->GetId(), m_ledger.Size());
}

void ReferenceStateService::OnSaveLoaded(const PacketEvent<RequestSaveLoaded>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    switch (m_loadCatchUps.Ask(pPlayer->GetId(), std::chrono::steady_clock::now()))
    {
    case LoadCatchUpLimit::Answer::kTaken: break;
    case LoadCatchUpLimit::Answer::kDropped:
        spdlog::info(
            "ReferenceStates: player {} said again within {} s that it loaded a save; not taken until then", pPlayer->GetId(),
            std::chrono::duration_cast<std::chrono::seconds>(LoadCatchUpLimit::kEvery).count());
        return;
    case LoadCatchUpLimit::Answer::kDroppedQuietly: return;
    }

    // The loaded save has its own plants, veins and traps: what this player's game was sent before is forgotten, and its place is
    // sent again. Only this player's own marks change; nobody else's state.
    m_ledger.ForgetToldTo(PlayerKeyOf(pPlayer));
    spdlog::debug("ReferenceStates: player {} loaded a save; the kept states of their place go to them again", pPlayer->GetId());
    SendCatchUp(pPlayer, pPlayer->GetCellComponent());
}

void ReferenceStateService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_rates.erase(acEvent.pPlayer ? acEvent.pPlayer->GetId() : 0);
    m_loadCatchUps.Forget(acEvent.pPlayer ? acEvent.pPlayer->GetId() : 0);
    if (acEvent.pPlayer)
        m_ledger.Forget(PlayerKeyOf(acEvent.pPlayer));
    // The last player out ends the session: the next one starts from the players' saves. The leaver is still counted here.
    if (m_world.GetPlayerManager().Count() <= 1)
        m_ledger.Clear();
}
