#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>

#include <Setting.h>
#include <WorldCommandRules.h>

extern Console::Setting<bool> bAnnounceServer;

namespace
{
bool IsAdmin(const Player* apPlayer) noexcept
{
    for (const auto cSession : GameServer::Get()->GetAdminSessions())
        if (const Player* pAdmin = PlayerManager::Get()->GetByConnectionId(cSession); pAdmin && pAdmin == apPlayer)
            return true;
    return false;
}
}

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
}

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    NotifySetTimeResult response{};

    // Authority comes from the authenticated connection, never the packet's claimed PlayerId.
    if (!acMessage.pPlayer)
        return;
    const auto* pPartyService = &m_world.GetPartyService();
    if (WorldCommandRules::CanSetTime(IsAdmin(acMessage.pPlayer), pPartyService->IsPlayerLeader(acMessage.pPlayer), CalendarService::IsOpenToStrangers()))
    {
        const auto cHours = static_cast<int>(acMessage.Packet.Hours);
        const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

        m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale());

        response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
        acMessage.pPlayer->Send(response);

        return;
    }

    response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
    acMessage.pPlayer->Send(response);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};

    // On a public server, only admins and the target's party may teleport to a player.
    // A refused answer carries no cell, and the game reports a failed teleport.
    if (pTargetPlayer && CalendarService::IsOpenToStrangers() && !IsAdmin(acMessage.pPlayer))
    {
        auto& partyService = m_world.GetPartyService();
        const auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
        const bool cSameParty = pParty && std::find(pParty->Members.begin(), pParty->Members.end(), pTargetPlayer) != pParty->Members.end();
        if (!cSameParty)
        {
            spdlog::info("Teleport of player {} to '{}' refused: a public server, and neither an admin nor in that player's party", acMessage.pPlayer->GetId(),
                         acMessage.Packet.TargetPlayer.c_str());
            pTargetPlayer = nullptr;
        }
    }

    if (pTargetPlayer)
    {
        auto character = pTargetPlayer->GetCharacter();
        if (character)
        {
            const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
            if (pMovementComponent)
            {
                const auto& cellComponent = pTargetPlayer->GetCellComponent();
                response.CellId = cellComponent.Cell;
                response.Position = pMovementComponent->Position;
                response.WorldSpaceId = cellComponent.WorldSpaceId;
            }
        }
    }

    acMessage.pPlayer->Send(response);
}
