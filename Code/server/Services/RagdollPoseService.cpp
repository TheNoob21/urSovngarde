#include <Services/RagdollPoseService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterRemoveEvent.h>

#include <Messages/RequestRagdollPose.h>
#include <Messages/NotifyRagdollPose.h>

#include <ClientTrust.h>

namespace
{
uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

RagdollPoseService::RagdollPoseService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_ragdollPoseConnection = aDispatcher.sink<PacketEvent<RequestRagdollPose>>().connect<&RagdollPoseService::OnRagdollPose>(this);
    m_characterRemoveConnection = aDispatcher.sink<CharacterRemoveEvent>().connect<&RagdollPoseService::OnCharacterRemove>(this);
}

void RagdollPoseService::SendRestBodies(Player* apPlayer, const entt::entity aEntity) const noexcept
{
    const uint32_t cId = World::ToInteger(aEntity);
    const RagdollPose* pBodies = m_restBodies.Resting(cId);
    if (!pBodies)
        return;
    const auto* pCharacter = m_world.try_get<CharacterComponent>(aEntity);
    const auto* pOwner = m_world.try_get<OwnerComponent>(aEntity);
    if (!pCharacter || !pCharacter->IsDead() || !pOwner || pOwner->GetOwner() == apPlayer)
        return;

    NotifyRagdollPose notify{};
    notify.ServerId = cId;
    notify.OwnershipEpoch = pOwner->OwnershipEpoch;
    notify.Pose = *pBodies;
    // Reliable, unlike the stream: it is sent once.
    apPlayer->Send(notify);
    spdlog::debug("Sent player {:X} the bodies dead creature {:X} rests in", apPlayer->GetId(), cId);
}

void RagdollPoseService::ForgetRestBodies(const entt::entity aEntity) noexcept
{
    m_restBodies.Forget(World::ToInteger(aEntity));
}

void RagdollPoseService::OnCharacterRemove(const CharacterRemoveEvent& acEvent) noexcept
{
    m_restBodies.Forget(acEvent.ServerId);
}

// Unreliable, like movement: a lost one is not sent again, the next replaces it.
void RagdollPoseService::OnRagdollPose(const PacketEvent<RequestRagdollPose>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const bool cExists = acMessage.pPlayer && view.contains(cEntity);

    if (m_relayed.size() > 1024)
        m_relayed.clear();
    const uint64_t cNow = NowMs();
    const auto relayed = m_relayed.find(message.ServerId);
    const RagdollPoseRelay::Relayed cLast = relayed != m_relayed.end() ? relayed->second : RagdollPoseRelay::Relayed{};
    if (!RagdollPoseRelay::ServerTakes(
            cExists, cExists && view.get<OwnerComponent>(cEntity).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch),
            cExists && view.get<CharacterComponent>(cEntity).IsPlayer(), !message.Pose.Bodies.empty(), cLast, cNow))
        return;
    m_relayed[message.ServerId] = RagdollPoseRelay::AfterRelay(cLast, cNow);

    // A dead creature's bodies at rest are kept for the players sent its body later.
    const bool cUsableRoot = ClientTrust::IsUsableTransform(message.Pose.Root[0], message.Pose.Root[1], message.Pose.Root[2], 0.f, 0.f, 0.f);
    switch (CorpseRestPose::OnReport(view.get<CharacterComponent>(cEntity).IsDead(), true, cUsableRoot))
    {
    case CorpseRestPose::Report::kTake: m_restBodies.Take(message.ServerId, message.Pose); break;
    case CorpseRestPose::Report::kForget: m_restBodies.Forget(message.ServerId); break;
    case CorpseRestPose::Report::kIgnore: break;
    }

    NotifyRagdollPose notify{};
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = view.get<OwnerComponent>(cEntity).OwnershipEpoch;
    notify.Pose = message.Pose;
    const auto& cCell = view.get<CellIdComponent>(cEntity);
    const bool cIsDragon = view.get<CharacterComponent>(cEntity).IsDragon();
    for (Player* pPlayer : m_world.GetPlayerManager())
        if (pPlayer != acMessage.pPlayer && cCell.IsInRange(pPlayer->GetCellComponent(), cIsDragon))
            pPlayer->Send(notify, TiltedPhoques::kUnreliable);
}
