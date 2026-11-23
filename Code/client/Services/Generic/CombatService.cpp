#include <Services/CombatService.h>
#include <Services/GameExtension.h>
#include <Helpers/PerfScope.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Events/HitEvent.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <Messages/RequestCombatTarget.h>
#include <Messages/NotifyCombatTarget.h>

#include <Systems/ModSystem.h>
#include <World.h>
#include <Utils.h>

#include <Projectiles/Projectile.h>
#include <Forms/TESObjectWEAP.h>
#include <Forms/TESAmmo.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Services/PvpProjectiles.h>
#include <Services/PartyService.h>
#include <Copies/EpochMiss.h>

namespace
{
// Words per message, under what a game takes in one (CombatTargetWords::kMax).
constexpr size_t kFightWordsPerMessage = 400;

uint64_t NowMs() noexcept
{
    return GetTickCount64();
}

// One line per NPC every 10 s: a flame's hits come four times a second.
void SayFriendlyHitIgnored(Actor* apActor, const uint32_t aAttackerPlayerId) noexcept
{
    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_saidAt;
    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = s_saidAt.begin(); it != s_saidAt.end();)
        it = cNow - it->second > std::chrono::seconds(60) ? s_saidAt.erase(it) : std::next(it);
    auto& saidAt = s_saidAt[apActor->formID];
    if (saidAt.time_since_epoch().count() && cNow - saidAt < std::chrono::seconds(10))
        return;
    saidAt = cNow;
    spdlog::info("Combat: actor {:X} lets player {}'s hit go, as the game does for a friendly hit ({})", apActor->formID, aAttackerPlayerId,
                 apActor->IsPlayerTeammate() ? "a follower" : "it ignores friendly hits");
}
} // namespace

CombatService::CombatService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CombatService::OnUpdate>(this);
    m_hitConnection = aDispatcher.sink<HitEvent>().connect<&CombatService::OnHitEvent>(this);
    m_localComponentRemoved = m_world.on_destroy<LocalComponent>().connect<&CombatService::OnLocalComponentRemoved>(this);
    m_projectileLaunchedConnection = aDispatcher.sink<ProjectileLaunchedEvent>().connect<&CombatService::OnProjectileLaunchedEvent>(this);
    m_projectileLaunchConnection = aDispatcher.sink<NotifyProjectileLaunch>().connect<&CombatService::OnNotifyProjectileLaunch>(this);
    m_combatTargetConnection = aDispatcher.sink<NotifyCombatTarget>().connect<&CombatService::OnNotifyCombatTarget>(this);
}

bool CombatService::CanFight(Actor* apActor) noexcept
{
    const ActorExtension* pExtension = apActor ? apActor->GetExtension() : nullptr;
    return pExtension && pExtension->IsLocal() && !pExtension->IsPlayer() && !apActor->IsDead() && !apActor->actorState.IsBleedingOut();
}

bool CombatService::SetOnTarget(Actor* apActor, Actor* apTarget) const noexcept
{
    if (!CanFight(apActor) || !apTarget || apTarget == apActor || apTarget->IsDead() || apTarget->IsDisabled() || !apTarget->GetNiNode())
        return false;
    apActor->StartCombatEx(apTarget);
    return true;
}

/**
 * Find whom an NPC this game runs fights, by server id.
 *
 * @param apActor The NPC.
 * @return Its target's server id; 0 for nobody, or for a target without one.
 */
uint32_t CombatService::FightOf(Actor* apActor) const noexcept
{
    if (!CanFight(apActor))
        return 0;
    const Actor* pTarget = apActor->GetCombatTarget();
    if (!pTarget)
        return 0;
    return Utils::FindServerIdByFormId(m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>), pTarget->formID).value_or(0);
}

void CombatService::FightBack(Actor* apActor, const uint32_t aAttackerPlayerId, const float aDeltaHealth) const noexcept
{
    // An NPC's combat runs on its owner, so a hit from another player must put that player into it.
    // Otherwise an NPC hostile to one player ignores the other, however hard they hit it.
    if (!apActor || !aAttackerPlayerId)
        return;

    // Damage only. The client adds the delta, so a hit is negative; a heal or a buff must never start a fight.
    if (aDeltaHealth >= 0.f)
        return;

    FightBack(apActor, aAttackerPlayerId);
}

void CombatService::FightBack(Actor* apActor, const uint32_t aAttackerPlayerId) const noexcept
{
    if (!apActor || !aAttackerPlayerId || !CanFight(apActor))
        return;

    // The game's own rule for a friendly hit, which never runs here: the hit arrives as a health message.
    // A party is one side, so a member's hit is a friend's for an NPC that is no enemy of this game's player.
    // A follower, and an NPC told to ignore friendly hits (a quest's companion, a siege's own soldiers), let it go.
    // The flag alone is not enough: a siege sets it on both sides, and the game applies it to friends only.
    const auto& cMembers = m_world.GetPartyService().GetPartyMembers();
    const bool cPartyMember = std::find(cMembers.begin(), cMembers.end(), aAttackerPlayerId) != cMembers.end();
    if (cPartyMember && (apActor->IsPlayerTeammate() || (apActor->GetIgnoreFriendlyHit() && !apActor->IsHostileToActor(PlayerCharacter::Get()))))
    {
        SayFriendlyHitIgnored(apActor, aAttackerPlayerId);
        return;
    }

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    const auto it = Utils::FindById<PlayerComponent>(view, aAttackerPlayerId);
    if (!it)
        return;

    Actor* pAttacker = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pAttacker || pAttacker == apActor)
        return;

    // A flurry of arrows would otherwise restart combat every frame.
    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_startedAt;
    const auto now = std::chrono::steady_clock::now();
    for (auto entry = s_startedAt.begin(); entry != s_startedAt.end();)
        entry = now - entry->second > std::chrono::seconds(30) ? s_startedAt.erase(entry) : std::next(entry);

    auto& startedAt = s_startedAt[apActor->formID];
    if (startedAt.time_since_epoch().count() && now - startedAt < std::chrono::seconds(3))
        return;
    startedAt = now;

    if (!SetOnTarget(apActor, pAttacker))
        return;
    spdlog::debug("Combat: actor {:X} now also fights player {} (copy {:X}), which attacked it", apActor->formID, aAttackerPlayerId, pAttacker->formID);
}

// A character gained while its last owner's game had it fighting fights the same one here.
// Left to itself, this game's AI starts it afresh: it puts its weapon away or goes back to its routine.
// The target is found by server id: the local player, another player's character or an NPC.
void CombatService::CarryOnFight(Actor* apActor, const uint32_t aServerId, const uint32_t aTargetServerId) const noexcept
{
    if (!aTargetServerId || !CanFight(apActor))
        return;

    const auto cTargetEntity = Utils::FindEntityByServerId(aTargetServerId);
    const auto* pTargetForm = cTargetEntity ? m_world.try_get<FormIdComponent>(*cTargetEntity) : nullptr;
    Actor* pTarget = pTargetForm ? Cast<Actor>(TESForm::GetById(pTargetForm->Id)) : nullptr;
    if (!SetOnTarget(apActor, pTarget))
    {
        spdlog::info("Combat: actor {:X} (server id {:X}) was fighting server character {:X} at its last owner, which is not here to fight; left to this game's AI",
                     apActor->formID, aServerId, aTargetServerId);
        return;
    }

    spdlog::info("Combat: actor {:X} (server id {:X}) fights {:X} (server id {:X}) here, as at its last owner; now in combat: {}", apActor->formID, aServerId, pTarget->formID,
                 aTargetServerId, apActor->IsInCombat());
}

// A copy taken over runs its own AI from now on: a script look-at left on it would keep its head on the old target.
void CombatService::ForgetCopyFight(const entt::entity aEntity, Actor* apActor, const char* acpWhy) noexcept
{
    const auto* pFight = m_world.try_get<CopyFightComponent>(aEntity);
    if (!pFight)
        return;

    if (pFight->LookingAtServerId && apActor)
    {
        apActor->ClearLookAt();
        spdlog::debug("Combat: copy {:X} no longer tracks server character {:X}: {}", apActor->formID, pFight->LookingAtServerId, acpWhy);
    }
    m_world.remove<CopyFightComponent>(aEntity);
}

void CombatService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("CombatService::OnUpdate");

    RunTargetUpdates(static_cast<float>(acEvent.Delta));

    const uint64_t cNow = NowMs();
    if (cNow < m_nextFightRead)
        return;
    m_nextFightRead = cNow + CombatTargets::kReadEveryMs;

    UpdateCopyFights(cNow);
    // Only once the server has accepted this player: it kicks a connection that sends anything before.
    if (m_transport.IsOnline())
        ReadOwnFights(cNow);
}

// The owner's side: whom each NPC this game runs fights, sent when it changes and at each heartbeat.
void CombatService::ReadOwnFights(const uint64_t aNowMs) noexcept
{
    const auto cView = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<PlayerComponent>);
    for (const entt::entity cEntity : cView)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(cView.get<FormIdComponent>(cEntity).Id));
        if (!pActor || pActor == PlayerCharacter::Get())
            continue;

        const LocalComponent& cLocal = cView.get<LocalComponent>(cEntity);
        m_fightOutbox.Put(cLocal.Id, FightOf(pActor), cLocal.OwnershipEpoch);
    }

    const std::vector<CombatTargets::Word> cWords = m_fightOutbox.Take(aNowMs);
    for (size_t first = 0; first < cWords.size(); first += kFightWordsPerMessage)
    {
        RequestCombatTarget request{};
        const size_t cLast = std::min(cWords.size(), first + kFightWordsPerMessage);
        request.Words.reserve(cLast - first);
        for (size_t i = first; i < cLast; ++i)
            request.Words.push_back({cWords[i].NpcId, cWords[i].TargetId, cWords[i].OwnershipEpoch});
        m_transport.Send(request);
    }
}

// The copy's side: each copy follows its owner's target until the owner says the fight ended, or says nothing for 5 s.
void CombatService::OnNotifyCombatTarget(const NotifyCombatTarget& acMessage) noexcept
{
    const uint64_t cNow = NowMs();
    for (const CombatTargetWord& cWord : acMessage.Words)
    {
        const std::optional<entt::entity> cEntity = Utils::FindEntityByServerId(cWord.NpcId);
        const auto* pRemote = cEntity ? m_world.try_get<RemoteComponent>(*cEntity) : nullptr;
        const CombatTargets::CopyVerdict cVerdict = CombatTargets::CopyTakes(cEntity.has_value(), pRemote != nullptr, pRemote ? pRemote->OwnershipEpoch : 0, cWord.OwnershipEpoch);
        if (cVerdict == CombatTargets::CopyVerdict::kEpochMiss)
            ReportEpochMiss("a combat target", cWord.NpcId, cWord.OwnershipEpoch, pRemote->OwnershipEpoch);
        if (cVerdict != CombatTargets::CopyVerdict::kTake || m_world.all_of<PlayerComponent>(*cEntity))
            continue;

        const auto* pFormId = m_world.try_get<FormIdComponent>(*cEntity);
        Actor* pCopy = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        if (!pCopy)
            continue;

        if (!cWord.TargetId)
        {
            ForgetCopyFight(*cEntity, pCopy, "its owner's fight ended");
            continue;
        }

        auto& fight = m_world.get_or_emplace<CopyFightComponent>(*cEntity);
        fight.TargetServerId = cWord.TargetId;
        fight.AtMs = cNow;
        if (!ApplyCopyFight(pCopy, fight))
            ForgetCopyFight(*cEntity, pCopy, "its target is dead, disabled or not drawn here");
    }
}

// Each round: a word not heard again for 5 s goes, and a target that died or went since is let go of.
void CombatService::UpdateCopyFights(const uint64_t aNowMs) noexcept
{
    struct Done
    {
        entt::entity Entity;
        Actor* pActor;
        const char* pWhy;
    };
    Vector<Done> done;

    const auto cView = m_world.view<CopyFightComponent>();
    for (const entt::entity cEntity : cView)
    {
        auto& fight = cView.get<CopyFightComponent>(cEntity);
        const auto* pFormId = m_world.try_get<FormIdComponent>(cEntity);
        Actor* pCopy = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        if (!pCopy || !m_world.all_of<RemoteComponent>(cEntity))
            done.push_back({cEntity, pCopy, "it is no longer a copy here"});
        else if (CombatTargets::CopyExpired(fight.AtMs, aNowMs))
            done.push_back({cEntity, pCopy, "no word from its owner for 5 s"});
        else if (!ApplyCopyFight(pCopy, fight))
            done.push_back({cEntity, pCopy, "its target is dead, disabled or not drawn here"});
    }

    // The actors are used before this frame ends, and never kept.
    for (const Done& cDone : done)
        ForgetCopyFight(cDone.Entity, cDone.pActor, cDone.pWhy);
}

// The target is found again by server id each time: the local player, another player's character or an NPC.
bool CombatService::ApplyCopyFight(Actor* apCopy, CopyFightComponent& aFight) const noexcept
{
    const auto cTargetEntity = Utils::FindEntityByServerId(aFight.TargetServerId);
    const auto* pTargetForm = cTargetEntity ? m_world.try_get<FormIdComponent>(*cTargetEntity) : nullptr;
    Actor* pTarget = pTargetForm ? Cast<Actor>(TESForm::GetById(pTargetForm->Id)) : nullptr;
    const bool cAlive = pTarget && pTarget != apCopy && !pTarget->IsDead() && !pTarget->IsDisabled() && pTarget->GetNiNode();

    // Keyed on the controller, not the game: a copy whose AI runs has one (VR today), one whose AI is off has none.
    const CombatTargets::CopyStep cStep = CombatTargets::CopyAction(apCopy->pCombatController != nullptr, pTarget != nullptr, cAlive);
    if (cStep == CombatTargets::CopyStep::kClear)
        return false;

    if (cStep == CombatTargets::CopyStep::kKeep)
    {
        // Kept for the next word: the owner's heartbeat says it again once the target is here.
        if (aFight.LookingAtServerId)
        {
            apCopy->ClearLookAt();
            aFight.LookingAtServerId = 0;
        }
        if (aFight.MissingServerId != aFight.TargetServerId)
        {
            aFight.MissingServerId = aFight.TargetServerId;
            spdlog::debug("Combat: copy {:X} kept, target server character {:X} not here; it looks at nobody until it is", apCopy->formID, aFight.TargetServerId);
        }
        return true;
    }
    aFight.MissingServerId = 0;

    // Re-pointed only, never started or stopped: the copy fights whom its owner fights.
    if (cStep == CombatTargets::CopyStep::kLookAtAndPoint && apCopy->GetCombatTarget() != pTarget)
    {
        apCopy->SetCombatTargetEx(pTarget);
        if (aFight.PointedServerId != aFight.TargetServerId)
        {
            aFight.PointedServerId = aFight.TargetServerId;
            spdlog::debug("Combat: copy {:X} has a combat controller here; pointed at {:X}, as at its owner", apCopy->formID, pTarget->formID);
        }
    }

    if (aFight.LookingAtServerId != aFight.TargetServerId)
    {
        apCopy->SetLookAt(pTarget);
        aFight.LookingAtServerId = aFight.TargetServerId;
        if (pTarget == PlayerCharacter::Get())
            spdlog::debug("Combat: copy {:X} tracks this game's player, as at its owner", apCopy->formID);
        else
            spdlog::debug("Combat: copy {:X} tracks {:X}, as at its owner", apCopy->formID, pTarget->formID);
    }
    return true;
}

void CombatService::OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    m_world.remove<CombatComponent>(aEntity);
}

void CombatService::OnProjectileLaunchedEvent(const ProjectileLaunchedEvent& acEvent) const noexcept
{
    ModSystem& modSystem = m_world.Get().GetModSystem();

    uint32_t shooterFormId = acEvent.ShooterID;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto shooterEntityIt = Utils::FindByFormId(view, shooterFormId);

    if (!shooterEntityIt)
        return;

    LocalComponent& localComponent = view.get<LocalComponent>(*shooterEntityIt);

    ProjectileLaunchRequest request{};

    request.OriginX = acEvent.Origin.x;
    request.OriginY = acEvent.Origin.y;
    request.OriginZ = acEvent.Origin.z;

    if (!modSystem.GetServerModId(acEvent.ProjectileBaseID, request.ProjectileBaseID) || !modSystem.GetServerModId(acEvent.WeaponID, request.WeaponID) ||
        !modSystem.GetServerModId(acEvent.AmmoID, request.AmmoID) || !modSystem.GetServerModId(acEvent.ParentCellID, request.ParentCellID) ||
        !modSystem.GetServerModId(acEvent.SpellID, request.SpellID))
        return;

    if (GameExtension::RejectMagic(request.ProjectileBaseID) || GameExtension::RejectMagic(request.WeaponID) || GameExtension::RejectMagic(request.AmmoID) ||
        GameExtension::RejectMagic(request.ParentCellID) || GameExtension::RejectMagic(request.SpellID))
        return;
    request.ShooterID = localComponent.Id;

    request.ZAngle = acEvent.ZAngle;
    request.XAngle = acEvent.XAngle;
    request.YAngle = acEvent.YAngle;

    request.CastingSource = acEvent.CastingSource;

    request.Area = acEvent.Area;
    request.Power = acEvent.Power;
    request.Scale = acEvent.Scale;

    request.AlwaysHit = acEvent.AlwaysHit;
    request.NoDamageOutsideCombat = acEvent.NoDamageOutsideCombat;
    request.AutoAim = acEvent.AutoAim;
    request.DeferInitialization = acEvent.DeferInitialization;
    request.ForceConeOfFire = acEvent.ForceConeOfFire;

    request.UnkBool1 = acEvent.UnkBool1;
    request.UnkBool2 = acEvent.UnkBool2;

    if (m_transport.Send(request))
        GameExtension::OnProjectile(
            {request.ShooterID,
             localComponent.OwnershipEpoch,
             1,
             acEvent.Handle,
             acEvent.Form,
             request.ProjectileBaseID.LogFormat(),
             request.WeaponID.LogFormat(),
             request.AmmoID.LogFormat(),
             request.SpellID.LogFormat(),
             {request.OriginX, request.OriginY, request.OriginZ},
             {request.XAngle, request.YAngle, request.ZAngle}});
}

void CombatService::OnNotifyProjectileLaunch(const NotifyProjectileLaunch& acMessage) const noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    if (GameExtension::RejectMagic(acMessage.ProjectileBaseID) || GameExtension::RejectMagic(acMessage.WeaponID) || GameExtension::RejectMagic(acMessage.AmmoID) ||
        GameExtension::RejectMagic(acMessage.ParentCellID) || GameExtension::RejectMagic(acMessage.SpellID))
        return;
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = Utils::FindById<RemoteComponent>(remoteView, acMessage.ShooterID);

    if (!remoteIt)
    {
        spdlog::warn("Shooter with remote id {:X} not found.", acMessage.ShooterID);
        return;
    }

    FormIdComponent formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    // Not while this game is paused (the journal, the console).
    // A projectile launched into a paused world hangs where it was made, and all of them land at once on unpause.
    if (const UI* pUI = UI::Get(); pUI && pUI->numPausesGame > 0)
    {
        static std::chrono::steady_clock::time_point s_nextSaid{};
        if (std::chrono::steady_clock::now() >= s_nextSaid)
        {
            s_nextSaid = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            spdlog::info("Remote projectile from {:X} not launched: this game is paused, and it would land only on unpause", formIdComponent.Id);
        }
        return;
    }

    Projectile::LaunchData launchData{};

    // Projectile::Launch relies on pParentCell being valid.
    // TODO: it's possible that more of these values must have a value, should probably check for that in the game code.
    const uint32_t cParentCellId = modSystem.GetGameId(acMessage.ParentCellID);
    launchData.pParentCell = Cast<TESObjectCELL>(TESForm::GetById(cParentCellId));

    if (!launchData.pParentCell)
    {
        spdlog::warn("Cannot launch projectile, invalid parent cell: {:X}", cParentCellId);
        return;
    }

    launchData.pShooter = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!launchData.pShooter)
    {
        spdlog::debug("Remote projectile skipped: shooter {:X} is unavailable", formIdComponent.Id);
        return;
    }

    launchData.Origin.x = acMessage.OriginX;
    launchData.Origin.y = acMessage.OriginY;
    launchData.Origin.z = acMessage.OriginZ;

    const uint32_t cProjectileBaseId = modSystem.GetGameId(acMessage.ProjectileBaseID);
    launchData.pProjectileBase = TESForm::GetById(cProjectileBaseId);

    // A form a mod made at run time has an id only its own game knows (mod id FFFFFFFF), and a mod this game lacks resolves to nothing.
    // The game refuses a launch without a projectile.
    if (!launchData.pProjectileBase || (GameExtension::ChecksMagic() && launchData.pProjectileBase->formType != FormType::Projectile))
    {
        static std::chrono::steady_clock::time_point s_nextSaid{};
        if (std::chrono::steady_clock::now() >= s_nextSaid)
        {
            s_nextSaid = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            spdlog::info("Remote projectile from {:X} not launched: its projectile {:X}:{:X} of spell {:X}:{:X} does not exist here", formIdComponent.Id,
                         acMessage.ProjectileBaseID.ModId, acMessage.ProjectileBaseID.BaseId, acMessage.SpellID.ModId, acMessage.SpellID.BaseId);
        }
        return;
    }

    const uint32_t cFromWeaponId = modSystem.GetGameId(acMessage.WeaponID);
    launchData.pFromWeapon = Cast<TESObjectWEAP>(TESForm::GetById(cFromWeaponId));

    const uint32_t cFromAmmoId = modSystem.GetGameId(acMessage.AmmoID);
    launchData.pFromAmmo = Cast<TESAmmo>(TESForm::GetById(cFromAmmoId));

    launchData.fZAngle = acMessage.ZAngle;
    launchData.fXAngle = acMessage.XAngle;
    launchData.fYAngle = acMessage.YAngle;

    const uint32_t cSpellId = modSystem.GetGameId(acMessage.SpellID);
    launchData.pSpell = Cast<MagicItem>(TESForm::GetById(cSpellId));

    if (GameExtension::ChecksMagic() &&
        ((acMessage.WeaponID && !launchData.pFromWeapon) || (acMessage.AmmoID && !launchData.pFromAmmo) || (acMessage.SpellID && !launchData.pSpell)))
        return;
    launchData.eCastingSource = (MagicSystem::CastingSource)acMessage.CastingSource;

    launchData.iArea = acMessage.Area;
    launchData.fPower = acMessage.Power;
    launchData.fScale = acMessage.Scale;

    launchData.bAlwaysHit = acMessage.AlwaysHit;
    launchData.bNoDamageOutsideCombat = acMessage.NoDamageOutsideCombat;
    launchData.bAutoAim = acMessage.AutoAim;

    launchData.bForceConeOfFire = acMessage.ForceConeOfFire;

    // always use origin, or it'll recalculate it and it desyncs
    launchData.bUseOrigin = true;

    launchData.bUnkBool1 = acMessage.UnkBool1;
    launchData.bUnkBool2 = acMessage.UnkBool2;

    BSPointerHandle<Projectile> result;

    Projectile::Launch(&result, launchData);
    GameExtension::OnProjectile(
        {acMessage.ShooterID,
         remoteView.get<RemoteComponent>(*remoteIt).OwnershipEpoch,
         0,
         result.handle.iBits,
         0,
         acMessage.ProjectileBaseID.LogFormat(),
         acMessage.WeaponID.LogFormat(),
         acMessage.AmmoID.LogFormat(),
         acMessage.SpellID.LogFormat(),
         {acMessage.OriginX, acMessage.OriginY, acMessage.OriginZ},
         {acMessage.XAngle, acMessage.YAngle, acMessage.ZAngle}});

    // Another player's arrow: its hit or miss here decides the hit their game reports on this player (the defender's own copy).
    // A spell's hit is their game's alone: it is what the caster's game saw that counts, so a sword blow after it is not the spell.
    if (result.handle.iBits != 0 && acMessage.SpellID.BaseId == 0 && m_world.all_of<PlayerComponent>(*remoteIt))
        PvpProjectiles::NoteLaunched(formIdComponent.Id);

    // Pairs with the sender's "Projectile launch" line: what this side actually launched for a remote shooter.
    spdlog::info("Remote projectile launched: shooter {:X}, base {:X}, weapon {:X}, ammo {:X}, spell {:X}, handle {}", formIdComponent.Id, cProjectileBaseId, cFromWeaponId, cFromAmmoId,
                 cSpellId, result.handle.iBits != 0);
}

void CombatService::OnHitEvent(const HitEvent& acEvent) const noexcept
{
#if 0
    if (!m_transport.IsConnected())
        return;

    // The targeting system does not apply to players, and only local actors should be considered.
    auto* pHittee = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));
    if (!pHittee || pHittee->GetExtension()->IsPlayer() || pHittee->GetExtension()->IsRemote())
        return;

    // NPCs should not affect targeting.
    auto* pHitter = Cast<Actor>(TESForm::GetById(acEvent.HitterId));
    if (!pHitter || !pHitter->GetExtension()->IsPlayer())
        return;

    // If the target is not in combat, don't start combat, let the game take care of that first.
    if (!pHittee->IsInCombat())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<ObjectComponent>);

    const auto hitteeIt = Utils::FindByFormId(view, acEvent.HitteeId);

    if (!hitteeIt)
    {
        spdlog::warn(__FUNCTION__ ": hittee form id component not found, form id: {:X}", acEvent.HitterId);
        return;
    }

    if (m_world.any_of<CombatComponent>(*hitteeIt))
        return;

    m_world.emplace_or_replace<CombatComponent>(*hitteeIt, acEvent.HitterId);

    pHittee->SetCombatTargetEx(pHitter);
#endif
}

void CombatService::RunTargetUpdates(const float acDelta) const noexcept
{
#if 0
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 200ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    const auto view = m_world.view<FormIdComponent, CombatComponent>();

    Vector<entt::entity> toRemove{};

    for (const auto entity : view)
    {
        auto& combatComponent = view.get<CombatComponent>(entity);
        combatComponent.Timer = combatComponent.Timer - acDelta;

        if (combatComponent.Timer <= 0.f)
        {
            toRemove.push_back(entity);
            continue;
        }

        auto* pTarget = Cast<Actor>(TESForm::GetById(combatComponent.TargetFormId));
        if (!pTarget)
        {
            spdlog::warn(__FUNCTION__ ": combat target not found, form id {:X}", combatComponent.TargetFormId);
            toRemove.push_back(entity);
            continue;
        }

        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
        {
            spdlog::error(__FUNCTION__ ": actor not found, form id {:X}", formIdComponent.Id);
            toRemove.push_back(entity);
            continue;
        }
    }

    for (const auto entity : toRemove)
        m_world.remove<CombatComponent>(entity);
#endif
}
