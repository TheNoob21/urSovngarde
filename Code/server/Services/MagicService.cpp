#include <Services/MagicService.h>

#include <ClientTrust.h>
#include <ClientTrustFacts.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SpellCastRequest.h>
#include <Messages/InterruptCastRequest.h>
#include <Messages/AddTargetRequest.h>

#include <Messages/NotifySpellCast.h>
#include <Messages/NotifyInterruptCast.h>
#include <Messages/NotifyAddTarget.h>
#include <Messages/NotifyRemoveSpell.h>

namespace
{
// Whether the server relays a report about this caster or target; a refusal is logged at debug.
bool Takes(const ClientTrust::Refusal aRefusal, const char* apWhat, const uint32_t aActorId, const Player* apSender) noexcept
{
    if (aRefusal == ClientTrust::Refusal::kNone || !GameServer::Get()->ValidatesClientReports())
        return true;
    spdlog::debug("Refused {} on {:X} from player {}: {}", apWhat, aActorId, apSender ? apSender->GetId() : 0, ClientTrust::RefusalName(aRefusal));
    return false;
}
} // namespace

MagicService::MagicService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_spellCastConnection = aDispatcher.sink<PacketEvent<SpellCastRequest>>().connect<&MagicService::OnSpellCastRequest>(this);
    m_interruptCastConnection = aDispatcher.sink<PacketEvent<InterruptCastRequest>>().connect<&MagicService::OnInterruptCastRequest>(this);
    m_addTargetConnection = aDispatcher.sink<PacketEvent<AddTargetRequest>>().connect<&MagicService::OnAddTargetRequest>(this);
    m_removeSpellConnection = aDispatcher.sink<PacketEvent<RemoveSpellRequest>>().connect<&MagicService::OnRemoveSpellRequest>(this);
}

void MagicService::OnSpellCastRequest(const PacketEvent<SpellCastRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const auto cCaster = ClientTrust::FactsOf(m_world, message.CasterId, acMessage.pPlayer);
    if (!Takes(ClientTrust::JudgeOwnedActor(cCaster.Known, cCaster.SenderOwns), "a spell cast", message.CasterId, acMessage.pPlayer))
        return;

    NotifySpellCast notify;
    notify.CasterId = message.CasterId;
    notify.SpellFormId = message.SpellFormId;
    notify.CastingSource = message.CastingSource;
    notify.IsDualCasting = message.IsDualCasting;
    notify.DesiredTarget = message.DesiredTarget;

    const auto entity = static_cast<entt::entity>(message.CasterId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnInterruptCastRequest(const PacketEvent<InterruptCastRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const auto cCaster = ClientTrust::FactsOf(m_world, message.CasterId, acMessage.pPlayer);
    if (!Takes(ClientTrust::JudgeOwnedActor(cCaster.Known, cCaster.SenderOwns), "a cast interruption", message.CasterId, acMessage.pPlayer))
        return;

    NotifyInterruptCast notify;
    notify.CasterId = message.CasterId;
    notify.CastingSource = message.CastingSource;

    const auto entity = static_cast<entt::entity>(message.CasterId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnAddTargetRequest(const PacketEvent<AddTargetRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    // From the game that runs the caster, or the one that runs the target.
    const auto cTarget = ClientTrust::FactsOf(m_world, message.TargetId, acMessage.pPlayer);
    const auto cCaster = ClientTrust::FactsOf(m_world, message.CasterId, acMessage.pPlayer);
    if (!Takes(
            ClientTrust::JudgeEffect(cTarget.Known, cTarget.SenderOwns, cCaster.SenderOwns && message.CasterId, message.Magnitude), "a spell effect", message.TargetId,
            acMessage.pPlayer))
        return;

    NotifyAddTarget notify;
    notify.TargetId = message.TargetId;
    notify.CasterId = message.CasterId;
    notify.SpellId = message.SpellId;
    notify.EffectId = message.EffectId;
    notify.Magnitude = message.Magnitude;
    notify.IsDualCasting = message.IsDualCasting;
    notify.CasterPerkFactor = message.CasterPerkFactor;
    notify.ApplyStaminaPerkBonus = message.ApplyStaminaPerkBonus;

    const auto entity = static_cast<entt::entity>(message.TargetId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnRemoveSpellRequest(const PacketEvent<RemoveSpellRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    const auto cTarget = ClientTrust::FactsOf(m_world, message.TargetId, acMessage.pPlayer);
    if (!Takes(ClientTrust::JudgeOwnedActor(cTarget.Known, cTarget.SenderOwns), "a spell removal", message.TargetId, acMessage.pPlayer))
        return;

    NotifyRemoveSpell notify;
    notify.TargetId = message.TargetId;
    notify.SpellId = message.SpellId;

    //spdlog::info(__FUNCTION__ ": TargetId: {}, Spell baseId: {}", notify.TargetId, notify.SpellId.BaseId);

    const auto entity = static_cast<entt::entity>(message.TargetId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
