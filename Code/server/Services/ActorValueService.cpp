#include <Components.h>
#include <EpochDrop.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Services/ActorValueService.h>
#include <Services/RagdollPoseService.h>
#include <World.h>
#include <GameServer.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <KillCredit.h>
#include <DragonSoul.h>
#include <Messages/NotifyDragonSoul.h>

#include <ClientTrust.h>
#include <Setting.h>

namespace
{
Console::Setting bEnableDragonSouls{"Gameplay:bEnableDragonSouls", "Gives every player of a party the soul of a dragon one of them killed, once per dragon", true};
}

bool ActorValueService::IsDragonSoulsOn() noexcept
{
    return bEnableDragonSouls;
}

namespace
{
// Tells a game that reported a hit with PvP off what the server's settings are, once in a while.
void CorrectPvp(Player* apPlayer) noexcept
{
    static TiltedPhoques::Map<uint32_t, ClientTrust::CorrectionWindow> s_windows;
    if (!apPlayer)
        return;
    if (s_windows.size() > 256)
        s_windows.clear();
    if (s_windows[apPlayer->GetId()].Due(std::chrono::steady_clock::now()))
        GameServer::Get()->SendSettingsTo(apPlayer);
}

// The values a game may report: the rest are left out.
template <class T> void KeepSaneValues(T& aValues) noexcept
{
    for (auto it = aValues.begin(); it != aValues.end();)
    {
        if (ClientTrust::IsSaneValue(it->second))
            ++it;
        else
            it = aValues.erase(it);
    }
}
} // namespace

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateHealthConnection = aDispatcher.sink<PacketEvent<RequestActorValueChanges>>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_updateMaxValueConnection = aDispatcher.sink<PacketEvent<RequestActorMaxValueChanges>>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_updateDeltaHealthConnection = aDispatcher.sink<PacketEvent<RequestHealthChangeBroadcast>>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_deathStateConnection = aDispatcher.sink<PacketEvent<RequestDeathStateChange>>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it == actorValuesView.end())
        return;
    if (!actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
    {
        ReportEpochDrop("an actor value change", message.Id, message.OwnershipEpoch, actorValuesView.get<OwnerComponent>(*it).OwnershipEpoch);
        return;
    }

    auto values = message.Values;
    if (GameServer::Get()->ValidatesClientReports())
        KeepSaneValues(values);

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : values)
    {
        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = value;
    }

    NotifyActorValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = acMessage.Packet.Id;
    notify.Values = std::move(values);

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it == actorValuesView.end())
        return;
    if (!actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
    {
        ReportEpochDrop("an actor max value change", message.Id, message.OwnershipEpoch, actorValuesView.get<OwnerComponent>(*it).OwnershipEpoch);
        return;
    }

    auto values = message.Values;
    if (GameServer::Get()->ValidatesClientReports())
        KeepSaneValues(values);

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : values)
    {
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList[id] = value;
    }

    NotifyActorMaxValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    notify.Values = std::move(values);

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    float deltaHealth = message.DeltaHealth;

    if (GameServer::Get()->ValidatesClientReports())
    {
        const entt::entity cTarget = static_cast<entt::entity>(message.Id);
        const auto* pCharacter = m_world.valid(cTarget) ? m_world.try_get<CharacterComponent>(cTarget) : nullptr;
        const auto* pOwner = pCharacter ? m_world.try_get<OwnerComponent>(cTarget) : nullptr;
        const auto* pCell = pCharacter ? m_world.try_get<CellIdComponent>(cTarget) : nullptr;
        ClientTrust::HealthFacts facts;
        facts.ActorKnown = pCharacter != nullptr;
        facts.TargetIsPlayer = pCharacter && pCharacter->IsPlayer();
        facts.SenderOwnsTarget = acMessage.pPlayer && pOwner && pOwner->GetOwner() == acMessage.pPlayer;
        facts.InRange = acMessage.pPlayer && pCell && pCell->IsInRange(acMessage.pPlayer->GetCellComponent(), pCharacter->IsDragon());
        facts.PvpOn = GameServer::Get()->IsPvpEnabled();
        if (const auto cRefusal = ClientTrust::JudgeHealthChange(facts, deltaHealth); cRefusal != ClientTrust::Refusal::kNone)
        {
            spdlog::debug(
                "Refused a health change of {} on {:X} from player {}: {}", message.DeltaHealth, message.Id, acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0,
                ClientTrust::RefusalName(cRefusal));
            if (cRefusal == ClientTrust::Refusal::kPvpOff)
                CorrectPvp(acMessage.pPlayer);
            return;
        }
    }

    // TODO(cosideci): should server side health not be updated?
    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it != actorValuesView.end())
    {
        auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
        // Damage arrives as a negative delta (HealthChangeEvent(formId, -realDamage)), so it is added.
        auto currentHealth = actorValuesComponent.CurrentActorValues.ActorValuesList[24];
        actorValuesComponent.CurrentActorValues.ActorValuesList[24] = currentHealth + deltaHealth;
    }

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = deltaHealth;
    // Only the server knows who sent this, and the actor's owner needs it to start combat with the right player.
    notify.AttackerPlayerId = acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0;
    notify.Tick = message.Tick;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    // TEMPORARY (PvP): logs one player's hit on another player's character and who it was sent to.
    const auto* pCharacter = m_world.try_get<CharacterComponent>(cEntity);
    // Skips a player's own game reporting its own health (an NPC's blow, a fall).
    const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity);
    const bool cOwnHealth = pOwner && pOwner->GetOwner() == acMessage.pPlayer;
    if (deltaHealth <= -1.f && pCharacter && pCharacter->IsPlayer() && !cOwnHealth)
    {
        std::string sentTo;
        if (const auto* pCell = m_world.try_get<CellIdComponent>(cEntity))
            for (Player* pOther : m_world.GetPlayerManager())
                if (pOther != acMessage.pPlayer && pCell->IsInRange(pOther->GetCellComponent(), false))
                    sentTo += fmt::format("{}{}", sentTo.empty() ? "" : ", ", pOther->GetId());
        spdlog::info(
            "PvP: player {} hit player character {:X} for {:.0f}; sent to player(s) [{}]", acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0, message.Id, -deltaHealth, sentTo);
    }
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();

    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it == characterView.end())
        return;
    if (!characterView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
    {
        ReportEpochDrop("a death state change", message.Id, message.OwnershipEpoch, characterView.get<OwnerComponent>(*it).OwnershipEpoch);
        return;
    }

    auto& characterComponent = characterView.get<CharacterComponent>(*it);
    characterComponent.SetDead(message.IsDead);
    // Its owner's game reports a life only when it saw the character come back from a death, so that is its revive.
    // A game joining later then follows the owner's life instead of keeping a body from its save (and killing it at the owner).
    characterComponent.SetRevivedSinceDeath(!message.IsDead);
    if (!message.IsDead)
    {
        spdlog::info("Death sync: character {:X} was revived in its owner's game (player {}); a game joining later follows that", message.Id,
                     acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0);
        // Its body no longer lies anywhere.
        m_world.GetCharacterService().ForgetRestPose(*it);
        m_world.ctx().at<RagdollPoseService>().ForgetRestBodies(*it);
    }
    spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);

    NotifyDeathStateChange notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;
    // Only the death itself is kept: the killer is credited once, by the game that gets this now.
    const uint32_t cSender = acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0;
    notify.KillerPlayerId = message.IsDead ? KillCredit::Relayed(message.KillerPlayerId, cSender, m_world.GetPlayerManager().GetById(message.KillerPlayerId) != nullptr) : 0;
    if (notify.KillerPlayerId)
        spdlog::debug("Death sync: character {:X} killed in player {}'s game by player {}'s blow", message.Id, cSender, notify.KillerPlayerId);

    // Sent to everyone, not only those in range: a death is sent once, and a missed one leaves a living body.
    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);

    // With bEnableDragonSouls off no soul is given; the death is still relayed above.
    if (!bEnableDragonSouls)
        return;

    // A dragon's soul, once per dragon, to every player of its owner's party, the owner too, wherever they are.
    // The entity stays the same across hand-offs, so a death reported again by another game gives nothing more.
    // Placed dragons are also known by their reference, as their entity is made again when no game holds them for a while.
    const auto* pFormId = m_world.try_get<FormIdComponent>(*it);
    const bool cPlaced = pFormId && pFormId->Id.ModId != std::numeric_limits<uint32_t>::max() && (pFormId->Id.ModId || pFormId->Id.BaseId);
    const uint64_t cRefKey = cPlaced ? pFormId->Id.LogFormat() : 0;
    const bool cGivenBefore = characterComponent.IsSoulGranted() || (cPlaced && m_dragonSoulsGiven.contains(cRefKey));
    if (DragonSoul::ShouldGrant(message.IsDead, characterComponent.IsDragon(), cGivenBefore) && acMessage.pPlayer)
    {
        characterComponent.SetSoulGranted(true);
        if (cPlaced)
            m_dragonSoulsGiven.insert(cRefKey);
        NotifyDragonSoul soul{};
        soul.ServerId = message.Id;
        uint32_t players = 0;
        if (const auto* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer))
        {
            for (Player* pMember : pParty->Members)
                if (pMember)
                {
                    pMember->Send(soul);
                    ++players;
                }
        }
        else
        {
            acMessage.pPlayer->Send(soul);
            players = 1;
        }
        spdlog::debug("Dragon soul: dragon {:X} died in player {}'s game; its soul given to {} player(s)", message.Id, cSender, players);
    }
}
