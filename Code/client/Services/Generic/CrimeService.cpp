#include <TiltedOnlinePCH.h>

#include <Services/CrimeService.h>
#include <Services/CharacterService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Messages/RequestCrimeReport.h>
#include <Messages/NotifyCrimeReport.h>

#include <Games/ActorExtension.h>
#include <Forms/TESFaction.h>
#include <Forms/TESNPC.h>
#include <PlayerCharacter.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>

#include <algorithm>
#include <limits>
#include <mutex>

using namespace std::chrono_literals;
using CrimeReports::Kind;
using CrimeReports::Reaction;

namespace
{
struct Crime
{
    Kind What{};
    uint32_t VictimId{};
    uint32_t RefId{};
    uint32_t ObjectId{};
    int32_t Num{};
    int32_t Total{};
    uint32_t OwnerId{};
    Vector<uint32_t> Witnesses;
};

// The game's crime alarms can run off the main thread; the crimes wait here for the next update.
std::mutex s_lock;
Vector<Crime> s_crimes;

// A form id as the server knows it; empty for a temporary form, which no other game has.
GameId ServerIdOf(World& aWorld, const uint32_t aFormId) noexcept
{
    GameId id{};
    if (!aFormId || !aWorld.GetModSystem().GetServerModId(aFormId, id) || id.ModId == std::numeric_limits<uint32_t>::max())
        return {};
    return id;
}

TESForm* FormOf(World& aWorld, const GameId& acId) noexcept
{
    if (acId == GameId{})
        return nullptr;
    const uint32_t cFormId = aWorld.GetModSystem().GetGameId(acId);
    return cFormId ? TESForm::GetById(cFormId) : nullptr;
}

bool Alive(Actor* apActor) noexcept
{
    return apActor && apActor->parentCell && apActor->HasLoadedProcess() && !apActor->IsDisabled() && !apActor->IsDead();
}

const char* NameOf(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::kTheft: return "theft";
    case Kind::kPickpocket: return "pickpocketing";
    case Kind::kAssault: return "assault";
    case Kind::kMurder: return "murder";
    default: return "crime";
    }
}
} // namespace

CrimeService::CrimeService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CrimeService::OnUpdate>(this);
    m_notifyConnection = aDispatcher.sink<NotifyCrimeReport>().connect<&CrimeService::OnNotifyCrimeReport>(this);
}

void CrimeService::OnLocalCrime(
    const Kind aKind, const uint32_t aVictimId, const uint32_t aRefId, const uint32_t aObjectId, const int32_t aNum, const int32_t aTotal, const uint32_t aOwnerId,
    const uint32_t* apWitnesses, const size_t aWitnessCount) noexcept
{
    std::scoped_lock lock(s_lock);
    if (s_crimes.size() < 64)
        s_crimes.push_back({aKind, aVictimId, aRefId, aObjectId, aNum, aTotal, aOwnerId, Vector<uint32_t>(apWitnesses, apWitnesses + aWitnessCount)});
}

void CrimeService::OnUpdate(const UpdateEvent&) noexcept
{
    Vector<Crime> crimes;
    {
        std::scoped_lock lock(s_lock);
        crimes.swap(s_crimes);
    }
    if (crimes.empty() || !m_transport.IsOnline() || !m_world.GetPartyService().IsInParty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    for (const Crime& cCrime : crimes)
    {
        RequestCrimeReport request{};
        CrimeReport& report = request.Report;
        report.Kind = static_cast<uint8_t>(cCrime.What);
        report.Ref = ServerIdOf(m_world, cCrime.RefId);
        report.Object = ServerIdOf(m_world, cCrime.ObjectId);
        report.Owner = ServerIdOf(m_world, cCrime.OwnerId);
        report.Count = std::min(static_cast<uint32_t>(std::max(cCrime.Num, 0)), static_cast<uint32_t>(CrimeReportLimits::kMaxCount));
        report.Value = std::min(static_cast<uint32_t>(std::max(cCrime.Total, 0)), static_cast<uint32_t>(CrimeReportLimits::kMaxValue));

        if (cCrime.VictimId)
        {
            if (const auto cEntity = Utils::FindByFormId(m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>), cCrime.VictimId))
                report.VictimId = Utils::GetServerId(*cEntity).value_or(0);
        }

        // Witnesses by their server ids: NPC copies only, as they are now. The guards among them are asked for once it is sent.
        Vector<std::pair<uint32_t, entt::entity>> guards;
        for (const uint32_t cFormId : cCrime.Witnesses)
        {
            const auto cEntity = Utils::FindByFormId(m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>), cFormId);
            if (!cEntity || m_world.all_of<PlayerComponent>(*cEntity))
                continue;
            if (const auto* pRemote = m_world.try_get<RemoteComponent>(*cEntity))
            {
                report.Witnesses.push_back(pRemote->Id);
                Actor* pWitness = Cast<Actor>(TESForm::GetById(cFormId));
                if (CrimeReports::OffenderAsksForWitness(pWitness && pWitness->IsGuard(), Alive(pWitness)))
                    guards.emplace_back(cFormId, *cEntity);
            }
        }

        const bool cTheft = cCrime.What == Kind::kTheft;
        if (report.Witnesses.empty() || (cTheft && (report.Object == GameId{} || report.Owner == GameId{})) || (!cTheft && !report.VictimId))
            continue;

        // One report a second against the same owner (a theft) or victim (anything else).
        if (m_reportedAt.size() > 256)
            m_reportedAt.clear();
        const uint64_t cKey = static_cast<uint64_t>(report.Kind) << 32 | (cTheft ? cCrime.OwnerId : cCrime.VictimId);
        auto& reportedAt = m_reportedAt[cKey];
        if (reportedAt.time_since_epoch().count() && cNow - reportedAt < 1s)
            continue;
        reportedAt = cNow;

        m_transport.Send(request);
        spdlog::debug("Crime: this player's {} seen by {} NPC(s) another game runs; sent to it", NameOf(cCrime.What), report.Witnesses.size());

        // Run here, a guard comes to arrest this player for the bounty this game holds, and the other game sees it come.
        for (const auto& [cFormId, cEntity] : guards)
        {
            const auto* pRemote = m_world.try_get<RemoteComponent>(cEntity);
            if (pRemote && m_world.GetCharacterService().RequestOwnership(cFormId, pRemote->Id, cEntity, false, false, true))
                spdlog::debug("Crime: guard {:X} (server id {:X}) saw this player's {}; asked for, to come for the player here", cFormId, pRemote->Id, NameOf(cCrime.What));
        }
    }
}

// The game that runs a witness. Everything is looked up again here; nothing is kept for a later frame.
void CrimeService::OnNotifyCrimeReport(const NotifyCrimeReport& acMessage) noexcept
{
    const CrimeReport& cReport = acMessage.Report;
    if (!m_transport.IsOnline() || !m_world.GetPartyService().IsInParty() || !CrimeReports::Taken(cReport.Kind))
        return;
    const Kind cKind = static_cast<Kind>(cReport.Kind);

    // The offender's copy here.
    Actor* pOffender = nullptr;
    const auto cPlayerView = m_world.view<FormIdComponent, RemoteComponent, PlayerComponent>();
    for (const entt::entity cEntity : cPlayerView)
    {
        if (cPlayerView.get<PlayerComponent>(cEntity).Id == acMessage.OffenderPlayerId)
            pOffender = Cast<Actor>(TESForm::GetById(cPlayerView.get<FormIdComponent>(cEntity).Id));
    }
    const ActorExtension* pOffenderExtension = pOffender ? pOffender->GetExtension() : nullptr;
    const bool cOffenderFound = Alive(pOffender) && pOffenderExtension && pOffenderExtension->IsRemotePlayer();

    // This game's own NPCs among the witnesses that still see the copy now.
    Vector<Actor*> witnesses;
    if (cOffenderFound)
    {
        for (const uint32_t cServerId : cReport.Witnesses)
        {
            const auto cEntity = Utils::FindEntityByServerId(cServerId);
            const auto* pFormId = cEntity ? m_world.try_get<FormIdComponent>(*cEntity) : nullptr;
            if (!pFormId || !m_world.all_of<LocalComponent>(*cEntity) || m_world.all_of<PlayerComponent>(*cEntity))
                continue;
            Actor* pWitness = Cast<Actor>(TESForm::GetById(pFormId->Id));
            if (Alive(pWitness) && pWitness->RequestDetectionLevel(pOffender) > 0)
                witnesses.push_back(pWitness);
        }
    }

    const bool cTheft = cKind == Kind::kTheft;
    TESForm* pObject = cTheft ? FormOf(m_world, cReport.Object) : nullptr;
    TESForm* pOwner = cTheft ? FormOf(m_world, cReport.Owner) : nullptr;
    const bool cOwnerFound = pOwner && (Cast<TESNPC>(pOwner) || Cast<TESFaction>(pOwner));

    if (!CrimeReports::ReceiverRuns(
            true, cOffenderFound, pOffender == PlayerCharacter::Get(), cOffenderFound && pOffender->IsPlayerTeammate(), !witnesses.empty(), !cTheft || pObject != nullptr,
            !cTheft || cOwnerFound))
    {
        spdlog::debug(
            "Crime: player {}'s {} not raised here: their copy {}, {} witness(es) of ours seeing it, item {}, owner {}", acMessage.OffenderPlayerId, NameOf(cKind),
            cOffenderFound ? "found" : "not found", witnesses.size(), cTheft ? (pObject ? "known" : "unknown") : "-", cTheft ? (cOwnerFound ? "known" : "unknown") : "-");
        return;
    }

    // A theft needs no reference here: the call-out is a line the witness says, and a shelf or table item is usually gone here already
    // (its pick-up was replayed). The check that once refused those was for the native theft alarm's replay, which a copy cannot use.

    switch (CrimeReports::ReactionOf(cKind))
    {
    case Reaction::kReplayAssault:
    {
        const auto cEntity = Utils::FindEntityByServerId(cReport.VictimId);
        const auto* pFormId = cEntity ? m_world.try_get<FormIdComponent>(*cEntity) : nullptr;
        Actor* pVictim = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        if (!pVictim || !pVictim->parentCell || pVictim == PlayerCharacter::Get())
        {
            spdlog::debug("Crime: player {}'s {} not raised here: its victim is not here", acMessage.OffenderPlayerId, NameOf(cKind));
            return;
        }
        if (!pVictim->ReplayAssault(pOffender))
        {
            spdlog::debug("Crime: player {}'s {} not raised here: native assault alarm unavailable", acMessage.OffenderPlayerId, NameOf(cKind));
            return;
        }
        break;
    }
    case Reaction::kStartCombat:
    {
        const auto cNow = std::chrono::steady_clock::now();
        if (m_turnedAt.size() > 256)
            m_turnedAt.clear();
        for (Actor* pWitness : witnesses)
        {
            if (!CrimeReports::WitnessMayFight(pWitness->IsGuard()))
                continue;
            auto& turnedAt = m_turnedAt[pWitness->formID];
            if (turnedAt.time_since_epoch().count() && cNow - turnedAt < 3s)
                continue;
            turnedAt = cNow;
            pWitness->StartCombatEx(pOffender);
        }
        break;
    }
    case Reaction::kCallOut:
    {
        const auto cNow = std::chrono::steady_clock::now();
        if (m_turnedAt.size() > 256)
            m_turnedAt.clear();
        for (Actor* pWitness : witnesses)
        {
            if (!CrimeReports::WitnessMayCallOut(pWitness->IsGuard()))
                continue;
            auto& turnedAt = m_turnedAt[pWitness->formID];
            if (turnedAt.time_since_epoch().count() && cNow - turnedAt < 3s)
                continue;
            turnedAt = cNow;
            if (!pWitness->CallOutTheft())
                spdlog::debug("Crime: player {}'s theft not called out by {:X}: the game's theft dialogue is not here", acMessage.OffenderPlayerId, pWitness->formID);
        }
        break;
    }
    default: return;
    }

    spdlog::debug(
        "Crime: player {}'s {} seen by {} NPC(s) of ours: they reacted to their copy {:X}", acMessage.OffenderPlayerId, NameOf(cKind), witnesses.size(), pOffender->formID);
}
