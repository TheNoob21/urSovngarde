#include <Services/MagicStateService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/RequestMagicState.h>
#include <Messages/NotifyMagicState.h>

#include <MagicStates.h>

MagicStateService::MagicStateService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_magicStateConnection = aDispatcher.sink<PacketEvent<RequestMagicState>>().connect<&MagicStateService::OnMagicState>(this);
}

// Only a player's own character, from that player; the state is cleaned before it is kept or passed on.
void MagicStateService::OnMagicState(const PacketEvent<RequestMagicState>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    const entt::entity cEntity = static_cast<entt::entity>(acMessage.Packet.CharacterId);
    if (!pSender || !m_world.valid(cEntity))
        return;

    const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
    const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
    if (!pCharacter || !pCharacter->IsPlayer() || !pOwner || pOwner->GetOwner() != pSender)
    {
        spdlog::debug("MagicState: player {:X} spoke for character {:X}, which is not theirs; refused", pSender->GetId(), acMessage.Packet.CharacterId);
        return;
    }

    auto& kept = m_world.get_or_emplace<MagicStateComponent>(cEntity);
    kept.State = MagicStates::Sanitized(acMessage.Packet.State);
    kept.SaidAt = std::chrono::steady_clock::now();

    NotifyMagicState notify;
    notify.CharacterId = acMessage.Packet.CharacterId;
    notify.State = kept.State;
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, pSender))
        spdlog::debug("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicStateService::SendTo(World& aWorld, Player* apPlayer, const entt::entity aEntity) noexcept
{
    const auto* pKept = aWorld.try_get<MagicStateComponent>(aEntity);
    const auto* pOwner = aWorld.try_get<OwnerComponent>(aEntity);
    if (!apPlayer || !pKept || !pOwner || pOwner->GetOwner() == apPlayer)
        return;

    const float cAge = std::chrono::duration<float>(std::chrono::steady_clock::now() - pKept->SaidAt).count();
    NotifyMagicState notify;
    notify.CharacterId = World::ToInteger(aEntity);
    notify.State = MagicStates::Aged(pKept->State, cAge);
    if (notify.State.Empty())
        return;
    apPlayer->Send(notify);
}
