#include <Services/PartyService.h>
#include <Services/PvpRules.h>
#include <Helpers/PerfScope.h>

#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyPartyPlugin.h>
#include <Messages/NotifyPartyValue.h>
#include <PluginCalls.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyChangeLeaderRequest.h>
#include <Messages/PartyKickRequest.h>
#include <Messages/RequestPartyFastTravel.h>
#include <Messages/RequestPartyFollow.h>
#include <Messages/NotifyPartyFastTravel.h>
#include <Interface/UI.h>
#include <Utils.h>

#include <OverlayApp.hpp>

#include <Forms/TESGlobal.h>

#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Games/References.h>
#include <Games/TES.h>

// A party member counts for the balance while their character is in this game within 3500 units (50 m).
constexpr float cNearbyUnits = 3500.f;

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransportService) noexcept
    : m_world(aWorld)
    , m_transport(aTransportService)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&PartyService::OnDisconnected>(this);

    m_playerListConnection = aDispatcher.sink<NotifyPlayerList>().connect<&PartyService::OnPlayerList>(this);
    m_partyInfoConnection = aDispatcher.sink<NotifyPartyInfo>().connect<&PartyService::OnPartyInfo>(this);
    m_partyInviteConnection = aDispatcher.sink<NotifyPartyInvite>().connect<&PartyService::OnPartyInvite>(this);
    m_partyJoinedConnection = aDispatcher.sink<NotifyPartyJoined>().connect<&PartyService::OnPartyJoined>(this);
    m_partyLeftConnection = aDispatcher.sink<NotifyPartyLeft>().connect<&PartyService::OnPartyLeft>(this);
    m_partyPluginConnection = aDispatcher.sink<NotifyPartyPlugin>().connect<&PartyService::OnPartyPlugin>(this);
    m_partyValueConnection = aDispatcher.sink<NotifyPartyValue>().connect<&PartyService::OnPartyValue>(this);
    m_partyFastTravelConnection = aDispatcher.sink<NotifyPartyFastTravel>().connect<&PartyService::OnPartyFastTravel>(this);
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void PartyService::CreateParty() const noexcept
{
    PartyCreateRequest request;
    m_transport.Send(request);
}

void PartyService::LeaveParty() const noexcept
{
    PartyLeaveRequest request;
    m_transport.Send(request);
}

void PartyService::CreateInvite(const uint32_t aPlayerId) const noexcept
{
    PartyInviteRequest request;
    request.PlayerId = aPlayerId;
    m_transport.Send(request);
}

void PartyService::AcceptInvite(const uint32_t aInviterId) const noexcept
{
    if (!m_invitations.contains(aInviterId))
        return;

    PartyAcceptInviteRequest request;
    request.InviterId = aInviterId;
    m_transport.Send(request);
}

void PartyService::KickPartyMember(const uint32_t aPlayerId) const noexcept
{
    PartyKickRequest kickMessage;
    kickMessage.PartyMemberPlayerId = aPlayerId;
    m_transport.Send(kickMessage);
}

void PartyService::ChangePartyLeader(const uint32_t aPlayerId) const noexcept
{
    PartyChangeLeaderRequest changeMessage;
    changeMessage.PartyMemberPlayerId = aPlayerId;
    m_transport.Send(changeMessage);
}

void PartyService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("PartyService::OnUpdate");
    RunFastTravelFollow();

    // Each frame, so another player's copy is marked before it can hit or be hit.
    PvpRules::MarkOtherGameCopies(m_world);

    const auto cCurrentTick = m_transport.GetClock().GetCurrentTick();
    if (m_nextUpdate > cCurrentTick)
        return;

    // Update once every second
    m_nextUpdate = cCurrentTick + 1000;

    auto itor = std::begin(m_invitations);
    while (itor != std::end(m_invitations))
    {
        if (itor->second < cCurrentTick)
            itor = m_invitations.erase(itor);
        else
            ++itor;
    }

    CountPlayersNearby();
}

void PartyService::CountPlayersNearby() noexcept
{
    uint32_t players = 1;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (m_inParty && pPlayer)
    {
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (const auto cEntity : view)
        {
            if (std::find(m_partyMembers.begin(), m_partyMembers.end(), view.get<PlayerComponent>(cEntity).Id) == m_partyMembers.end())
                continue;
            Actor* pCopy = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
            if (pCopy && pCopy != pPlayer && pCopy->GetNiNode() && glm::distance(glm::vec3(pCopy->position), glm::vec3(pPlayer->position)) <= cNearbyUnits)
                ++players;
        }
    }
    SetPlayersNearby(players);
}

void PartyService::SetPlayersNearby(const uint32_t aPlayers) noexcept
{
    if (aPlayers == m_playersNearby)
        return;

    m_playersNearby = aPlayers;
    const float cHealth = GameplayFormulas::PartyEnemyHealth(aPlayers);
    spdlog::info("Balance: now {} of the party within 50 m, this one included; enemies take {:.0f}% of this player's damage (as with {:.0f}% health) and deal {:.0f}% to this player",
                 aPlayers, 100.f / cHealth, 100.f * cHealth, 100.f * GameplayFormulas::PartyEnemyDamage(aPlayers));
}

void PartyService::OnPartyPlugin(const NotifyPartyPlugin& acMessage) noexcept
{
    StorePartyPluginAnswer(acMessage.PluginFile, acMessage.PlayerWithout);
}

void PartyService::OnPartyValue(const NotifyPartyValue& acMessage) noexcept
{
    StorePartyValue(acMessage.Key, acMessage.Value);
}

void PartyService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    ForgetPartyPluginAnswers();
    // After the party is gone, so a plugin's set made meanwhile finds this game no longer leading.
    DestroyParty();
    ForgetPartyValues();
    m_invitations.clear();
    m_nextUpdate = 0;
    // Another server numbers its players afresh.
    m_flatPlayers.clear();
}

void PartyService::OnPlayerList(const NotifyPlayerList& acPlayerList) noexcept
{
    m_players = acPlayerList.Players;
    m_flatPlayers = acPlayerList.FlatPlayers;
}

void PartyService::OnPartyInfo(const NotifyPartyInfo& acPartyInfo) noexcept
{
    if (m_inParty)
    {
        spdlog::debug("[PartyService]: Got party info update");
        ForgetPartyPluginAnswers();
        m_isLeader = acPartyInfo.IsLeader;
        m_leaderPlayerId = acPartyInfo.LeaderPlayerId;
        m_partyMembers = acPartyInfo.PlayerIds;

        UpdateWorldEncounters();

        if (!m_world.GetOverlayService().GetOverlayApp())
            return;

        auto pArguments = CefListValue::Create();

        auto pPlayerIds = CefListValue::Create();
        for (int i = 0; i < m_partyMembers.size(); i++)
            pPlayerIds->SetInt(i, m_partyMembers[i]);

        pArguments->SetList(0, pPlayerIds);
        pArguments->SetInt(1, acPartyInfo.LeaderPlayerId);

        m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyInfo", pArguments);
    }
}

void PartyService::UpdateWorldEncounters() noexcept
{
    // While connected, only the party leader rolls encounters; a connected solo player does not.
    if (TESGlobal* pWorldEncountersEnabled = Cast<TESGlobal>(TESForm::GetById(0xB8EC1)))
    {
        const bool cEnable = m_inParty && m_isLeader;
        const float cWanted = cEnable ? 1.f : 0.f;
        if (pWorldEncountersEnabled->f != cWanted)
            spdlog::debug("[PartyService]: world encounters {} in this game (party leader: {})", cEnable ? "on" : "off", cEnable);
        pWorldEncountersEnabled->f = cWanted;
    }
}

BSTEventResult PartyService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    // The save can restore a stale value. Check current state on the main thread, including a disconnect since load.
    m_world.GetRunner().Queue(
        [this]()
        {
            if (m_transport.IsConnected())
                UpdateWorldEncounters();
        });
    return BSTEventResult::kOk;
}

void PartyService::OnPartyInvite(const NotifyPartyInvite& acPartyInvite) noexcept
{
    spdlog::debug("[PartyService]: Got party invite from {}", acPartyInvite.InviterId);

    m_invitations[acPartyInvite.InviterId] = acPartyInvite.ExpiryTick;

    if (!m_world.GetOverlayService().GetOverlayApp())
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acPartyInvite.InviterId);
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyInviteReceived", pArguments);
}

void PartyService::OnPartyJoined(const NotifyPartyJoined& acPartyJoined) noexcept
{
    spdlog::debug("[PartyService]: Joined party. LeaderId: {}, IsLeader: {}", acPartyJoined.LeaderPlayerId, acPartyJoined.IsLeader);

    ForgetPartyPluginAnswers();
    // The new party's values follow this message.
    ForgetPartyValues();
    m_inParty = true;
    m_isLeader = acPartyJoined.IsLeader;
    m_leaderPlayerId = acPartyJoined.LeaderPlayerId;
    m_partyMembers = acPartyJoined.PlayerIds;

    m_world.GetDispatcher().trigger(PartyJoinedEvent(m_isLeader));
}

void PartyService::OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept
{
    spdlog::debug("[PartyService]: Left party");

    ForgetPartyPluginAnswers();
    DestroyParty();
    ForgetPartyValues();

    m_world.GetDispatcher().trigger(PartyLeftEvent());
}

// OverlayService.cpp: the place a cell is in, as the party menu names it.
String GetCellName(const GameId& acWorldSpaceId, const GameId& acCellId) noexcept;

void PartyService::OnPartyFastTravel(const NotifyPartyFastTravel& acMessage) noexcept
{
    if (!m_inParty || m_isLeader || acMessage.LeaderId != m_leaderPlayerId)
        return;

    const auto cLeader = m_players.find(acMessage.LeaderId);
    const String cLeaderName = cLeader != m_players.end() ? cLeader->second : String("The party leader");
    String cPlace = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    if (cPlace == "UNKNOWN")
        cPlace.clear();

    m_followOfferUntil = std::chrono::steady_clock::now() + 30s;
    m_followLeaderId = acMessage.LeaderId;
    m_followAccepted = false;
    spdlog::debug("[PartyService]: the leader fast-travelled to '{}'; the Sleep/Wait menu follows for 30 s", cPlace.c_str());
    Utils::ShowModHudMessage(
        cLeaderName + " fast-travelled" + (cPlace.empty() ? String("") : String(" to ") + cPlace) +
        ". Open the Wait menu within 30 s to follow (on foot; followers and a horse stay behind).");
}

void PartyService::ForgetFollowOffer() noexcept
{
    m_followOfferUntil = {};
    m_followLeaderId = 0;
    m_followAccepted = false;
    m_closeFollowMenu.store(false);
}

void PartyService::RunFastTravelFollow() noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextFollowCheck)
        return;
    m_nextFollowCheck = cNow + 250ms;

    const UI* pUI = UI::Get();
    if (!pUI)
        return;
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_sleepWaitMenu("Sleep/Wait Menu");
    const bool cLoading = pUI->GetMenuOpen(s_loadingMenu);

    // The leader: a loading screen queued with the map open is a fast travel. Told once the load is over, from where it arrived.
    const uint32_t cFastTravels = UI::FastTravelsQueued();
    if (m_fastTravelsSeen && *m_fastTravelsSeen != cFastTravels && m_inParty && m_isLeader)
        m_fastTravelSendAt = cNow + 2s;
    m_fastTravelsSeen = cFastTravels;
    if (m_fastTravelSendAt != std::chrono::steady_clock::time_point{})
    {
        if (cLoading)
            m_fastTravelSendAt = cNow + 2s;
        else if (cNow >= m_fastTravelSendAt)
        {
            m_fastTravelSendAt = {};
            if (m_inParty && m_isLeader && m_transport.IsConnected())
                m_transport.Send(RequestPartyFastTravel{});
        }
    }

    // Only a menu opened after the offer came answers it: one already open (a sleep under way) does not.
    const bool cWaitMenuOpen = pUI->GetMenuOpen(s_sleepWaitMenu);
    const bool cWaitMenuOpened = cWaitMenuOpen && !m_waitMenuWasOpen;
    m_waitMenuWasOpen = cWaitMenuOpen;

    if (!HasFollowOffer())
        return;
    if (!m_inParty || m_isLeader || m_leaderPlayerId != m_followLeaderId)
    {
        ForgetFollowOffer();
        return;
    }

    if (!m_followAccepted)
    {
        if (cNow >= m_followOfferUntil)
        {
            spdlog::debug("[PartyService]: the leader's fast travel was not followed");
            ForgetFollowOffer();
            return;
        }
        if (cWaitMenuOpened)
        {
            m_followAccepted = true;
            m_followOfferUntil = {};
            Utils::ShowModHudMessage("press OK or close the menu to follow");
        }
        return;
    }

    // The menu's OK follows: the menu is closed as the game closes it, and the follow below goes on once it is gone.
    if (cWaitMenuOpen && m_closeFollowMenu.exchange(false) && !UI::CloseMenu(s_sleepWaitMenu))
        Utils::ShowModHudMessage("close the menu to follow");

    // Once the menu is gone, and with the game able to take the move.
    if (cWaitMenuOpen || cLoading)
        return;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const bool cDown = pPlayer && pPlayer->actorState.LifeState() != LifeStates::kAlive;
    ForgetFollowOffer();
    if (!pPlayer || cDown || pPlayer->IsInCombat())
    {
        Utils::ShowModHudMessage("not followed, not while fighting or down");
        return;
    }
    spdlog::debug("[PartyService]: following the leader's fast travel");
    m_transport.Send(RequestPartyFollow{});
}

void PartyService::DestroyParty() noexcept
{
    ForgetFollowOffer();
    m_fastTravelSendAt = {};
    m_inParty = false;
    m_isLeader = false;
    m_leaderPlayerId = -1;
    m_partyMembers.clear();
    SetPlayersNearby(1);
}
