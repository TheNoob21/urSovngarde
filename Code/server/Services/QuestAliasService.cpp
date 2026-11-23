#include <Services/QuestAliasService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PlayerLeaveEvent.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestAliases.h>
#include <Messages/NotifyQuestAliases.h>

#include <Setting.h>

namespace
{
Console::Setting bEnableQuestAliasSync{
    "Gameplay:bEnableQuestAliasSync", "(Experimental) Radiant quests (bounties, guild jobs) name the same target for the whole party: the first to start one picks", true};
}

QuestAliasService::QuestAliasService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestAliasService::OnQuestUpdate>(this);
    m_questAliasesConnection = aDispatcher.sink<PacketEvent<RequestQuestAliases>>().connect<&QuestAliasService::OnQuestAliases>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&QuestAliasService::OnPlayerLeave>(this);
}

bool QuestAliasService::IsEnabled() noexcept
{
    return bEnableQuestAliasSync;
}

void QuestAliasService::OnQuestUpdate(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    const auto& cPartyId = pPlayer->GetParty().JoinedPartyId;
    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);
    if (!bEnableQuestAliasSync || !cPartyId || !pParty)
        return;

    const auto& cMessage = acMessage.Packet;
    if (cMessage.Status == RequestQuestUpdate::Started)
    {
        pParty->QuestAliasFills.Start(cMessage.Id);
        m_deciders.OnStarted(*cPartyId, cMessage.Id, pPlayer->GetId());
    }
    else if (cMessage.Status == RequestQuestUpdate::Stopped)
    {
        m_deciders.OnStopped(*cPartyId, cMessage.Id);
        pParty->QuestAliasFills.Stop(cMessage.Id);
    }
}

void QuestAliasService::OnQuestAliases(const PacketEvent<RequestQuestAliases>& acMessage) noexcept
{
    if (!bEnableQuestAliasSync)
        return;

    Player* pPlayer = acMessage.pPlayer;
    const auto& cPartyId = pPlayer->GetParty().JoinedPartyId;
    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);
    if (!cPartyId || !pParty)
        return;

    const auto cIsMember = [pParty](uint32_t aPlayerId)
    {
        return std::any_of(pParty->Members.begin(), pParty->Members.end(), [aPlayerId](const Player* apMember) { return apMember->GetId() == aPlayerId; });
    };

    const QuestAliases& cAliases = acMessage.Packet.Aliases;
    if (!cAliases.Quest || cAliases.Fills.empty() || pParty->QuestAliasFills.IsStopped(cAliases.Quest))
        return;
    if (!m_deciders.Accept(*cPartyId, cAliases.Quest, pPlayer->GetId(), pParty->LeaderPlayerId == pPlayer->GetId(), cIsMember))
    {
        spdlog::debug("QuestAliasSync: dropped fills of quest {:X} from player {}, another player picks them", cAliases.Quest.LogFormat(), pPlayer->GetId());
        return;
    }

    pParty->QuestAliasFills.Remember(cAliases);
    NotifyQuestAliases notify{};
    notify.Aliases = cAliases;
    GameServer::Get()->SendToParty(notify, pPlayer->GetParty(), acMessage.GetSender());
}

void QuestAliasService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_deciders.ForgetPlayer(acEvent.pPlayer->GetId());
}
