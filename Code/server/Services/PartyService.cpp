#include <Services/PartyService.h>
#include <Components.h>
#include <GameServer.h>
#include <Services/QuestService.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/NotifyPartyPlugin.h>
#include <Messages/RequestPartyPlugin.h>
#include <Messages/NotifyPartyValue.h>
#include <Messages/RequestPartyValue.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyReferenceEnable.h>
#include <Messages/NotifyQuestCatchUp.h>
#include <Messages/NotifyQuestAliases.h>
#include <Services/QuestAliasService.h>
#include <Messages/NotifyWordLearned.h>
#include <Messages/NotifyMapDiscovery.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyChangeLeaderRequest.h>
#include <Messages/PartyKickRequest.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/RequestPartyFastTravel.h>
#include <Messages/RequestPartyFollow.h>
#include <Messages/NotifyPartyFastTravel.h>
#include <Messages/NotifyTeleport.h>

#include <PartyRules.h>
#include <Setting.h>

#include <cmath>

namespace
{
Console::Setting bAutoPartyCreate{"Gameplay:bAutoPartyCreate", "Create a party for the first player to join, so players are grouped without a party menu (VR has none)", true};
Console::Setting bEnablePartyFastTravel{
    "Gameplay:bEnablePartyFastTravel",
    "(Experimental) When the party leader fast-travels, the members away from the leader may follow. The leader's game tells the server of each fast travel either way; while this "
    "is off the server drops it",
    true};

// How long a member may take the leader's fast travel offer: 30 s to open the Wait menu, then as long as it stays open, within reason.
constexpr auto cFollowOfferFor = std::chrono::minutes(5);
}

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateEvent(aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&PartyService::OnPlayerJoin>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PartyService::OnPlayerLeave>(this))
    , m_partyInviteConnection(aDispatcher.sink<PacketEvent<PartyInviteRequest>>().connect<&PartyService::OnPartyInvite>(this))
    , m_partyAcceptInviteConnection(aDispatcher.sink<PacketEvent<PartyAcceptInviteRequest>>().connect<&PartyService::OnPartyAcceptInvite>(this))
    , m_partyLeaveConnection(aDispatcher.sink<PacketEvent<PartyLeaveRequest>>().connect<&PartyService::OnPartyLeave>(this))
    , m_partyCreateConnection(aDispatcher.sink<PacketEvent<PartyCreateRequest>>().connect<&PartyService::OnPartyCreate>(this))
    , m_partyChangeLeaderConnection(aDispatcher.sink<PacketEvent<PartyChangeLeaderRequest>>().connect<&PartyService::OnPartyChangeLeader>(this))
    , m_partyKickConnection(aDispatcher.sink<PacketEvent<PartyKickRequest>>().connect<&PartyService::OnPartyKick>(this))
    , m_partyPluginConnection(aDispatcher.sink<PacketEvent<RequestPartyPlugin>>().connect<&PartyService::OnPartyPlugin>(this))
    , m_partyValueConnection(aDispatcher.sink<PacketEvent<RequestPartyValue>>().connect<&PartyService::OnPartyValue>(this))
    , m_partyFastTravelConnection(aDispatcher.sink<PacketEvent<RequestPartyFastTravel>>().connect<&PartyService::OnPartyFastTravel>(this))
    , m_partyFollowConnection(aDispatcher.sink<PacketEvent<RequestPartyFollow>>().connect<&PartyService::OnPartyFollow>(this))
{
}

const PartyService::Party* PartyService::GetById(uint32_t aId) const noexcept
{
    auto itor = m_parties.find(aId);
    if (itor != std::end(m_parties))
        return &itor->second;

    return nullptr;
}

bool PartyService::IsPlayerInParty(Player* const apPlayer) const noexcept
{
    return apPlayer->GetParty().JoinedPartyId.has_value();
}

bool PartyService::IsPlayerLeader(const Player* const apPlayer) const noexcept
{
    const auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        if (const auto* const pParty = GetById(*inviterPartyComponent.JoinedPartyId))
            return pParty->LeaderPlayerId == apPlayer->GetId();
    }

    return false;
}

PartyService::Party* PartyService::GetPlayerParty(Player* const apPlayer) noexcept
{
    auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        return &m_parties[*inviterPartyComponent.JoinedPartyId];
    }

    return nullptr;
}

void PartyService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    const auto cCurrentTick = GameServer::Get()->GetTick();
    if (m_nextInvitationExpire > cCurrentTick)
        return;

    // Only expire once every 10 seconds
    m_nextInvitationExpire = cCurrentTick + 10000;

    auto view = m_world.view<PartyComponent>();
    for (auto entity : view)
    {
        auto& partyComponent = view.get<PartyComponent>(entity);
        auto itor = std::begin(partyComponent.Invitations);
        while (itor != std::end(partyComponent.Invitations))
        {
            if (itor->second < cCurrentTick)
            {
                itor = partyComponent.Invitations.erase(itor);
            }
            else
            {
                ++itor;
            }
        }
    }
}

void PartyService::OnPartyCreate(const PacketEvent<PartyCreateRequest>& acPacket) noexcept
{
    spdlog::debug("[PartyService]: Received request to create party");

    CreateParty(acPacket.pPlayer);
}

void PartyService::CreateParty(Player* const player) noexcept
{
    auto& inviterPartyComponent = player->GetParty();

    if (!inviterPartyComponent.JoinedPartyId) // Ensure not in party
    {
        uint32_t partyId = m_nextId++;
        Party& party = m_parties[partyId];
        party.Members.push_back(player);
        party.LeaderPlayerId = player->GetId();
        inviterPartyComponent.JoinedPartyId = partyId;

        spdlog::debug("[PartyService]: Created party for {}", player->GetId());
        SendPartyJoinedEvent(party, player);

        if (m_parties.size() == 1 && GameServer::Get()->AllowsAutoPartyJoin())
        {
            for (Player* otherPlayer : m_world.GetPlayerManager())
            {
                if (otherPlayer->GetId() != player->GetId())
                {
                    party.Members.push_back(otherPlayer);
                    otherPlayer->GetParty().JoinedPartyId = partyId;

                    SendPartyJoinedEvent(party, otherPlayer);
                }
            }

            BroadcastPartyInfo(partyId);
        }
    }
}

void PartyService::OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;
    Player* const player = acPacket.pPlayer;
    Player* const pNewLeader = m_world.GetPlayerManager().GetById(message.PartyMemberPlayerId);

    spdlog::debug("[PartyService]: Received request to change party leader to {}", message.PartyMemberPlayerId);

    if (!pNewLeader)
    {
        spdlog::error("[PartyService]: Player {} does not exist. Cannot change party leader", message.PartyMemberPlayerId);
        return;
    }

    auto& inviterPartyComponent = player->GetParty();
    if (inviterPartyComponent.JoinedPartyId) // Ensure not in party
    {
        Party& party = m_parties[*inviterPartyComponent.JoinedPartyId];
        if (party.LeaderPlayerId == player->GetId())
        {
            for (auto& pPlayer : party.Members)
            {
                if (pPlayer->GetId() == pNewLeader->GetId())
                {
                    party.LeaderPlayerId = pPlayer->GetId();
                    party.FollowOffers.clear();
                    spdlog::debug("[PartyService]: Changed party leader to {}, updating party members.", party.LeaderPlayerId);
                    BroadcastPartyInfo(*inviterPartyComponent.JoinedPartyId);
                    break;
                }
            }
        }
    }
}

void PartyService::OnPartyPlugin(const PacketEvent<RequestPartyPlugin>& acPacket) noexcept
{
    const String& cPluginFile = acPacket.Packet.PluginFile;
    const auto cSameFile = [&cPluginFile](const String& acFile)
    {
        return acFile.size() == cPluginFile.size() &&
               std::equal(acFile.begin(), acFile.end(), cPluginFile.begin(), [](const char aLeft, const char aRight) { return std::tolower(static_cast<uint8_t>(aLeft)) == std::tolower(static_cast<uint8_t>(aRight)); });
    };

    NotifyPartyPlugin notify{};
    notify.PluginFile = cPluginFile;
    // A player's list is known from their login, before they can be in a party.
    if (const Party* pParty = GetPlayerParty(acPacket.pPlayer))
    {
        for (const Player* pMember : pParty->Members)
        {
            const auto& cMods = pMember->GetMods();
            if (std::none_of(cMods.begin(), cMods.end(), cSameFile))
            {
                notify.PlayerWithout = pMember->GetUsername();
                break;
            }
        }
    }
    acPacket.pPlayer->Send(notify);
}

void PartyService::OnPartyValue(const PacketEvent<RequestPartyValue>& acPacket) noexcept
{
    const auto& cMessage = acPacket.Packet;
    Party* pParty = GetPlayerParty(acPacket.pPlayer);
    if (!pParty || pParty->LeaderPlayerId != acPacket.pPlayer->GetId())
        return;
    if (cMessage.Key.empty() || cMessage.Key.size() > RequestPartyValue::kLongestKey || !std::isfinite(cMessage.Value))
        return;
    if (!pParty->PluginValues.contains(cMessage.Key) && pParty->PluginValues.size() >= RequestPartyValue::kMostKeys)
    {
        spdlog::warn("PartyValue: '{}' refused, the party already holds {} keys", cMessage.Key.c_str(), pParty->PluginValues.size());
        return;
    }
    if (pParty->PluginValues.try_emplace(cMessage.Key, cMessage.Value).second)
        spdlog::info("PartyValue: leader {} set '{}' to {}", acPacket.pPlayer->GetId(), cMessage.Key.c_str(), cMessage.Value);
    else
        pParty->PluginValues[cMessage.Key] = cMessage.Value;

    NotifyPartyValue notify{};
    notify.Key = cMessage.Key;
    notify.Value = cMessage.Value;
    for (Player* pMember : pParty->Members)
        if (pMember != acPacket.pPlayer)
            pMember->Send(notify);
}

void PartyService::OnPartyFastTravel(const PacketEvent<RequestPartyFastTravel>& acPacket) noexcept
{
    Player* pLeader = acPacket.pPlayer;
    Party* pParty = GetPlayerParty(pLeader);
    if (!bEnablePartyFastTravel || !pParty || pParty->LeaderPlayerId != pLeader->GetId())
        return;

    const auto& cLeaderCell = pLeader->GetCellComponent();
    NotifyPartyFastTravel notify{};
    notify.LeaderId = pLeader->GetId();
    notify.CellId = cLeaderCell.Cell;
    notify.WorldSpaceId = cLeaderCell.WorldSpaceId;

    const auto cNow = std::chrono::steady_clock::now();
    for (Player* pMember : pParty->Members)
    {
        // A member who arrived with the leader, or stands near where it arrived, has nothing to follow.
        if (pMember == pLeader || pMember->GetCellComponent().IsInRange(cLeaderCell, false))
            continue;

        pParty->FollowOffers[pMember->GetId()] = cNow;
        pMember->Send(notify);
        spdlog::debug("[PartyService]: leader {} fast-travelled to cell {:X}; player {} may follow", pLeader->GetId(), cLeaderCell.Cell.LogFormat(), pMember->GetId());
    }
}

void PartyService::OnPartyFollow(const PacketEvent<RequestPartyFollow>& acPacket) noexcept
{
    Player* pMember = acPacket.pPlayer;
    Party* pParty = GetPlayerParty(pMember);
    if (!bEnablePartyFastTravel || !pParty)
        return;

    const auto cOffer = pParty->FollowOffers.find(pMember->GetId());
    if (cOffer == pParty->FollowOffers.end())
        return;
    const bool cInTime = std::chrono::steady_clock::now() - cOffer->second < cFollowOfferFor;
    pParty->FollowOffers.erase(cOffer);
    if (!cInTime)
        return;

    Player* pLeader = m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId);
    if (!pLeader || pLeader == pMember || !pLeader->GetCharacter())
        return;
    const auto* pMovement = m_world.try_get<MovementComponent>(*pLeader->GetCharacter());
    if (!pMovement)
        return;

    // Where the leader is now, not where it arrived: it may have walked on.
    NotifyTeleport teleport{};
    teleport.CellId = pLeader->GetCellComponent().Cell;
    teleport.WorldSpaceId = pLeader->GetCellComponent().WorldSpaceId;
    teleport.Position = pMovement->Position;
    pMember->Send(teleport);
    spdlog::debug("[PartyService]: player {} follows leader {} to cell {:X}", pMember->GetId(), pLeader->GetId(), teleport.CellId.LogFormat());
}

void PartyService::OnPartyKick(const PacketEvent<PartyKickRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;
    Player* const player = acPacket.pPlayer;
    Player* const pKick = m_world.GetPlayerManager().GetById(message.PartyMemberPlayerId);

    spdlog::debug("[PartyService]: Received request to change party leader to {}", message.PartyMemberPlayerId);

    if (!pKick)
    {
        spdlog::error("[PartyService]: Player {} does not exist. Cannot kick", message.PartyMemberPlayerId);
        return;
    }

    // Only a member of the leader's own party: with two parties, one leader could kick the other's members.
    const auto& cKickerParty = player->GetParty().JoinedPartyId;
    if (PartyRules::MayKick(cKickerParty, IsPlayerLeader(player), pKick->GetParty().JoinedPartyId))
    {
        spdlog::debug("[PartyService]: Kicking player {} from party", pKick->GetId());
        RemovePlayerFromParty(pKick);
        // The kicked player is still on the server, so everyone's list keeps them, with the game they play.
        BroadcastPlayerList();
    }
}

void PartyService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    BroadcastPlayerList();

    NotifyPlayerJoined notify{};
    notify.PlayerId = acEvent.pPlayer->GetId();
    notify.Username = acEvent.pPlayer->GetUsername();

    notify.WorldSpaceId = acEvent.WorldSpaceId;
    notify.CellId = acEvent.CellId;

    notify.Level = acEvent.pPlayer->GetLevel();

    spdlog::debug("[Party] New notify player {:x} {}", notify.PlayerId, notify.Username.c_str());

    GameServer::Get()->SendToPlayers(notify, acEvent.pPlayer);

    if (!m_parties.empty() && GameServer::Get()->AllowsAutoPartyJoin() && !IsPlayerInParty(acEvent.pPlayer))
    {
        std::vector<PartyRules::Party> parties;
        for (const auto& [cId, cParty] : m_parties)
            parties.push_back({cId, cParty.Members.size()});

        if (const auto cPartyId = PartyRules::PartyToJoin(parties))
        {
            Party& party = m_parties[*cPartyId];
            party.Members.push_back(acEvent.pPlayer);
            acEvent.pPlayer->GetParty().JoinedPartyId = *cPartyId;

            spdlog::debug("[PartyService]: Player {} joins party {} of {}", acEvent.pPlayer->GetId(), *cPartyId, m_parties.size());
            SendPartyJoinedEvent(party, acEvent.pPlayer);
            BroadcastPartyInfo(*cPartyId);
        }
    }

    if (m_parties.empty() && bAutoPartyCreate && !GameServer::Get()->IsPublicServer() && !IsPlayerInParty(acEvent.pPlayer))
    {
        spdlog::info("[PartyService]: No party on the server, creating one for {}", acEvent.pPlayer->GetId());
        CreateParty(acEvent.pPlayer);
    }
}

void PartyService::OnPartyInvite(const PacketEvent<PartyInviteRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;

    // Make sure the player we invite exists
    Player* const pInvitee = m_world.GetPlayerManager().GetById(message.PlayerId);
    Player* const pInviter = acPacket.pPlayer;

    // If both players are available and they are different
    if (pInvitee && pInvitee != pInviter)
    {
        auto& inviterPartyComponent = pInviter->GetParty();
        auto& inviteePartyComponent = pInvitee->GetParty();

        spdlog::debug("[PartyService]: Got party invite from {}", pInviter->GetId());

        if (!inviterPartyComponent.JoinedPartyId)
        {
            spdlog::debug("[PartyService]: Inviter not in party, cancelling invite.");
            return;
        }
        else if (inviteePartyComponent.JoinedPartyId)
        {
            spdlog::debug("[PartyService]: Invitee in party already, cancelling invite.");
            return;
        }

        auto& party = m_parties[*inviterPartyComponent.JoinedPartyId];
        if (party.LeaderPlayerId != pInviter->GetId())
        {
            spdlog::debug("[PartyService]: Inviter not party leader, cancelling invite.");
            return;
        }

        // Expire in 60 seconds
        const auto cExpiryTick = GameServer::Get()->GetTick() + 60000;
        inviteePartyComponent.Invitations[pInviter] = cExpiryTick;

        NotifyPartyInvite notification;
        notification.InviterId = pInviter->GetId();
        notification.ExpiryTick = cExpiryTick;

        spdlog::debug("[PartyService]: Sending party invite to {}", pInvitee->GetId());
        pInvitee->Send(notification);
    }
}

void PartyService::OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>& acPacket) noexcept
{
    auto& message = acPacket.Packet;

    Player* const pInviter = m_world.GetPlayerManager().GetById(message.InviterId);
    Player* pSelf = acPacket.pPlayer;

    spdlog::debug("[PartyService]: Got party accept request from {}", pSelf->GetId());

    // If both players are available and they are different
    if (pInviter && pInviter != pSelf)
    {
        auto& inviterPartyComponent = pInviter->GetParty();
        auto& selfPartyComponent = pSelf->GetParty();

        // Check if we have this invitation so people don't invite themselves
        if (selfPartyComponent.Invitations.count(pInviter) == 0)
            return;

        spdlog::debug("[PartyService]: Invite found, processing.");
        if (!inviterPartyComponent.JoinedPartyId) // Ensure inviter is in a party otherwise break
        {
            spdlog::debug("[PartyService]: Inviter not in party. Cancelling.");
            return;
        }

        auto partyId = *inviterPartyComponent.JoinedPartyId;
        Party& party = m_parties[partyId];

        if (party.LeaderPlayerId != pInviter->GetId())
        {
            spdlog::debug("[PartyService]: Inviter is not party leader. Cancelling.");
            return;
        }

        if (selfPartyComponent.JoinedPartyId) // Remove from party if in one already. TODO: Decide if player needs to be out of party first
        {
            spdlog::debug("[PartyService]: Invitee already in party, cancelling.");
            // RemovePlayerFromParty(pSelf, false); // skip sending left event, will override with SendPartyJoinedEvent
            return;
        }

        party.Members.push_back(pSelf);
        selfPartyComponent.JoinedPartyId = partyId;

        spdlog::debug("[PartyService]: Added invitee to party, sending events");
        SendPartyJoinedEvent(party, pSelf);
        BroadcastPartyInfo(partyId);
    }
}

void PartyService::OnPartyLeave(const PacketEvent<PartyLeaveRequest>& acPacket) noexcept
{
    RemovePlayerFromParty(acPacket.pPlayer);
}

void PartyService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    RemovePlayerFromParty(acEvent.pPlayer);
    BroadcastPlayerList(acEvent.pPlayer);
}

void PartyService::RemovePlayerFromParty(Player* apPlayer) noexcept
{
    auto* pPartyComponent = &apPlayer->GetParty();
    spdlog::debug("[PartyService]: Removing player from party.");

    if (pPartyComponent->JoinedPartyId)
    {
        auto id = *pPartyComponent->JoinedPartyId;

        Party& party = m_parties[id];
        auto& members = party.Members;

        members.erase(std::find(std::begin(members), std::end(members), apPlayer));
        party.FollowOffers.erase(apPlayer->GetId());

        if (members.empty())
        {
            m_parties.erase(id);
            m_world.GetQuestService().ForgetPartyWords(id);
            if (auto* pAliases = m_world.ctx().find<QuestAliasService>())
                pAliases->ForgetParty(id);
        }
        else
        {
            if (party.LeaderPlayerId == apPlayer->GetId())
            {
                party.LeaderPlayerId = members.at(0)->GetId(); // Reassign party leader
                party.FollowOffers.clear();                    // a new leader's fast travels only
                spdlog::debug("[PartyService]: Leader left, reassigned party leader to {}", party.LeaderPlayerId);
            }
            spdlog::debug("[PartyService]: Updating other party players of removal.");
            BroadcastPartyInfo(id);
        }

        pPartyComponent->JoinedPartyId.reset();

        spdlog::debug("[PartyService]: Sending party left event to player.");
        NotifyPartyLeft leftMessage;
        apPlayer->Send(leftMessage);
    }
}

void PartyService::BroadcastPlayerList(Player* apPlayer) const noexcept
{
    auto pIgnoredPlayer = apPlayer;
    for (auto pSelf : m_world.GetPlayerManager())
    {
        if (pIgnoredPlayer == pSelf)
            continue;

        NotifyPlayerList playerList;
        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pSelf == pPlayer)
                continue;

            if (pIgnoredPlayer == pPlayer)
                continue;

            playerList.Players[pPlayer->GetId()] = pPlayer->GetUsername();
            if (pPlayer->IsFlat())
                playerList.FlatPlayers.push_back(pPlayer->GetId());
        }

        pSelf->Send(playerList);
    }
}

void PartyService::BroadcastPartyInfo(uint32_t aPartyId) const noexcept
{
    auto itor = m_parties.find(aPartyId);
    if (itor == std::end(m_parties))
        return;

    auto& party = itor->second;
    auto& members = party.Members;

    NotifyPartyInfo message;
    message.LeaderPlayerId = party.LeaderPlayerId;

    for (auto pPlayer : members)
    {
        message.PlayerIds.push_back(pPlayer->GetId());
    }

    for (auto pPlayer : members)
    {
        message.IsLeader = pPlayer->GetId() == party.LeaderPlayerId;
        pPlayer->Send(message);
    }
}

void PartyService::SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept
{
    NotifyPartyJoined joinedMessage;
    joinedMessage.LeaderPlayerId = aParty.LeaderPlayerId;
    joinedMessage.IsLeader = aParty.LeaderPlayerId == aPlayer->GetId();
    for (auto pPlayer : aParty.Members)
    {
        joinedMessage.PlayerIds.push_back(pPlayer->GetId());
    }
    spdlog::debug("[PartyService]: Sending party join event to player");
    aPlayer->Send(joinedMessage);

    // A member who joins gets the references the party's scripts have switched so far.
    // Sent in parts, each well under what one message may hold (ReferenceEnableStates::kMax).
    if (!joinedMessage.IsLeader && !aParty.ReferenceEnabled.Empty())
    {
        constexpr size_t cPart = 4096;
        NotifyReferenceEnable states{};
        aParty.ReferenceEnabled.ForEach(
            [&](const GameId& acId, const bool aEnabled)
            {
                states.Changes.push_back({acId, aEnabled});
                if (states.Changes.size() == cPart)
                {
                    aPlayer->Send(states);
                    states.Changes.clear();
                }
            });
        if (!states.Changes.empty())
            aPlayer->Send(states);
        spdlog::info("ReferenceEnable: {} reference(s) the party switched sent to joining player {}", aParty.ReferenceEnabled.Size(), aPlayer->GetId());
    }

    // And the numbers the leader's plugins set for the party.
    if (!joinedMessage.IsLeader && !aParty.PluginValues.empty())
    {
        for (const auto& [cKey, cValue] : aParty.PluginValues)
        {
            NotifyPartyValue value{};
            value.Key = cKey;
            value.Value = cValue;
            aPlayer->Send(value);
        }
        spdlog::info("PartyValue: {} value(s) sent to joining player {}", aParty.PluginValues.size(), aPlayer->GetId());
    }

    // And the words of power the party was taught, which this player's game teaches as it does one taught live.
    if (!joinedMessage.IsLeader && aPlayer->GetParty().JoinedPartyId)
    {
        const auto cWords = m_world.GetQuestService().GetPartyWords().Of(*aPlayer->GetParty().JoinedPartyId);
        if (!cWords.empty())
        {
            NotifyWordLearned words{};
            for (const uint64_t cWord : cWords)
                words.Words.push_back(GameId(static_cast<uint32_t>(cWord >> 32), static_cast<uint32_t>(cWord)));
            aPlayer->Send(words);
            spdlog::debug("WordLearned: {} word(s) the party was taught sent to joining player {}", cWords.size(), aPlayer->GetId());
        }
    }

    // And the map markers its members discovered.
    if (!joinedMessage.IsLeader && aParty.Discoveries.Size() != 0)
    {
        NotifyMapDiscovery discoveries{};
        for (const auto& [cKey, cCanTravel] : aParty.Discoveries.All())
            discoveries.Discoveries.push_back({GameId(static_cast<uint32_t>(cKey >> 32), static_cast<uint32_t>(cKey)), cCanTravel});
        aPlayer->Send(discoveries);
        spdlog::debug("MapDiscovery: {} marker(s) the party discovered sent to joining player {}", discoveries.Discoveries.size(), aPlayer->GetId());
    }

    // And the quest starts, stages and stops the party was sent before, which this player missed.
    // Only while the switch is on: a list kept before it was turned off is not sent.
    if (QuestHistory::MaySendCatchUp(joinedMessage.IsLeader, QuestService::IsCatchUpOn(), !aParty.Quests.GetSteps().empty()))
    {
        NotifyQuestCatchUp catchUp{};
        catchUp.Steps = aParty.Quests.GetSteps();
        aPlayer->Send(catchUp);
        spdlog::debug("QuestCatchUp: {} quest step(s) sent to joining player {}", catchUp.Steps.size(), aPlayer->GetId());
    }

    if (aParty.Waypoint)
        aPlayer->Send(*aParty.Waypoint);

    if (QuestAliasService::IsEnabled())
    {
        for (const auto& [_, aliases] : aParty.QuestAliasFills.GetQuests())
        {
            NotifyQuestAliases notify{};
            notify.Aliases = aliases;
            aPlayer->Send(notify);
        }
    }
}
