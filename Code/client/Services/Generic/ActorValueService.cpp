#include <TiltedOnlinePCH.h>
#include <Helpers/Throttle.h>
#include <Helpers/PerfScope.h>
#include <cmath>

#include <Services/ActorValueService.h>
#include <Services/CombatService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESNPC.h>
#include <Games/References.h>
#include <Components.h>
#include <Systems/InterpolationSystem.h>
#include <Copies/EpochMiss.h>

#include <Events/UpdateEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>
#include <Events/ClashEvent.h>
#include <PlayerCharacter.h>
#include <UI/Banner/DeathCause.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Services/PvpProjectiles.h>
#include <Services/BlockService.h>
#include <Services/PvpRules.h>

#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/RequestDeathStateChange.h>
#include <KillCredit.h>
#include <StuckArrows.h>

#include <misc/ActorValueOwner.h>

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<ClashEvent>().connect<&ActorValueService::OnClash>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

// The least health a remote player's copy is ever given (see StandingValues in CharacterService).
// A copy at 1 health goes down on the next hit and stops being drawn.
constexpr float kRemotePlayerHealthFloor = 25.f;

// The least health an NPC copy is given: only its owner's death word kills it.
constexpr float kNpcCopyHealthFloor = 1.f;

static bool IsNpcCopy(Actor* apActor) noexcept
{
    const ActorExtension* pExtension = apActor ? apActor->GetExtension() : nullptr;
    return pExtension && pExtension->IsRemote() && !pExtension->IsPlayer();
}

// Once per copy, so that a fight leaves a few lines.
static void SayNpcCopyKeptUp(Actor* apActor, float aHealth) noexcept
{
    static TiltedPhoques::Set<uint32_t> s_said;
    if (s_said.size() > 256)
        s_said.clear();
    if (s_said.insert(apActor->formID).second)
        spdlog::info("Death sync: NPC copy {:X} kept at {:.0f} health instead of {:.0f}; its owner's game decides whether it dies", apActor->formID, kNpcCopyHealthFloor, aHealth);
}

// Another player's spell on this player, summed per second: a beam's hits are small, and come four times a second.
static void SaySpellHits(const uint32_t aPlayerId, const float aDamage, const float aHealth) noexcept
{
    static float s_damage = 0.f;
    static uint32_t s_hits = 0;
    static std::chrono::steady_clock::time_point s_since = std::chrono::steady_clock::now();

    s_damage += aDamage;
    ++s_hits;
    if (const auto cNow = std::chrono::steady_clock::now(); cNow - s_since >= 1s)
    {
        spdlog::info("PvP: spell hits from player {}: {} hit(s), {:.0f} damage in {} ms; health now {:.0f}", aPlayerId, s_hits, s_damage,
                     std::chrono::duration_cast<std::chrono::milliseconds>(cNow - s_since).count(), aHealth);
        s_damage = 0.f;
        s_hits = 0;
        s_since = cNow;
    }
}

// The form id of player aPlayerId's copy in this game, or 0 when it has none.
static uint32_t PlayerCopyFormId(uint32_t aPlayerId) noexcept
{
    auto view = World::Get().view<PlayerComponent, FormIdComponent>();
    for (const auto cEntity : view)
        if (view.get<PlayerComponent>(cEntity).Id == aPlayerId)
            return view.get<FormIdComponent>(cEntity).Id;
    return 0;
}

// Set only while ApplyHealthChange hands another player's blow to the game.
static thread_local uint32_t t_blowAttacker = 0;

uint32_t ActorValueService::BlowAttacker() noexcept
{
    return t_blowAttacker;
}

void ActorValueService::CreateActorValuesComponent(const entt::entity aEntity, Actor* apActor) noexcept
{
    auto& actorValuesComponent = m_world.emplace_or_replace<ActorValuesComponent>(aEntity);

    for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
    {
        float value = apActor->GetActorValue(i);
        actorValuesComponent.CurrentActorValues.ActorValuesList.insert({i, value});
        float maxValue = apActor->GetActorPermanentValue(i);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList.insert({i, maxValue});
    }
}

void ActorValueService::OnLocalComponentAdded(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    const auto& formIdComponent = aRegistry.get<FormIdComponent>(aEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (pActor != NULL)
    {
        auto& localComponent = aRegistry.get<LocalComponent>(aEntity);
        localComponent.IsDead = pActor->IsDead();
        localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        CreateActorValuesComponent(aEntity, pActor);
    }
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    // TODO: this crashes sometimes, no clue why
    m_world.clear<ActorValuesComponent>();
    m_heldHits.clear();
    m_clashes.clear();
    m_healthResends.clear();
    m_healthEases.clear();
    m_deathRepeats.clear();
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    m_deathRepeats.erase(acEvent.FormId);
    m_healthEases.erase(acEvent.FormId);
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    const auto it = Utils::FindById<FormIdComponent>(view, acEvent.FormId);
    if (it)
        m_world.remove<ActorValuesComponent>(*it);
}

namespace
{
// A copy revived on its owner's word can be dead again within 100 ms, with no kill and its health full.
// The word comes only once, so each revival is checked a second later and repeated until it lasts.
// Every copy's revival is watched (Actor::Respawn), whichever way the word reached it.
struct CopyRevival
{
    std::chrono::steady_clock::time_point CheckAt{};
    uint32_t TriesLeft{};
};
TiltedPhoques::Map<uint32_t, CopyRevival> s_copyRevivals;

bool IsAliveAtOwner(const uint32_t aFormId) noexcept
{
    auto view = World::Get().view<FormIdComponent, RemoteComponent>();
    const auto it = Utils::FindByFormId(view, aFormId);
    return it && !view.get<RemoteComponent>(*it).DeadAtOwner;
}

/**
 * Look again at each copy revived in the last moments, and revive it again if it is dead while its owner has it alive.
 * A copy stops being watched once it lives, its tries are used up, its owner has it dead, or a spell raises it (Actor::IsBeingRaised).
 */
void RunCopyRevivals() noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = s_copyRevivals.begin(); it != s_copyRevivals.end();)
    {
        if (cNow < it->second.CheckAt)
        {
            ++it;
            continue;
        }

        Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        if (!pExtension || !pExtension->IsRemote() || !pActor->IsDead() || it->second.TriesLeft == 0 || !IsAliveAtOwner(pActor->formID) || pActor->IsBeingRaised())
        {
            it = s_copyRevivals.erase(it);
            continue;
        }

        pActor->Respawn();
        it.value().CheckAt = cNow + 1s;
        --it.value().TriesLeft;
        spdlog::info("Death sync: remote actor {:X} is dead again after its revival; revival asked again, {} tries left", pActor->formID, it->second.TriesLeft);
        ++it;
    }
}
} // namespace

void ActorValueService::WatchCopyRevival(const uint32_t aFormId) noexcept
{
    constexpr uint32_t cTries = 5;

    s_copyRevivals.try_emplace(aFormId, CopyRevival{std::chrono::steady_clock::now() + 1s, cTries});
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("ActorValueService::OnUpdate");

    RunSmallHealthUpdates();
    RunHeldHits();
    RunHealthEases();
    RunDeathStateUpdates();
    RunActorValuesUpdates();
    RunCopyRevivals();
}

void ActorValueService::BroadcastActorValues() noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Only changes are sent, so a copy that misses one stays wrong until the next change.
    // A player's health, magicka and stamina are therefore re-sent every three seconds.
    static std::chrono::steady_clock::time_point s_nextRefresh{};
    const auto refreshNow = std::chrono::steady_clock::now();
    const bool cRefresh = refreshNow >= s_nextRefresh;
    if (cRefresh)
        s_nextRefresh = refreshNow + 3s;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();
    Set<uint32_t> stillToResend;

    for (auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);

        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        RequestActorValueChanges requestValueChanges;
        requestValueChanges.Id = localComponent.Id;
        requestValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;
        requestMaxValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if (isPlayer && i == ActorValueInfo::kDragonSouls)
                continue;
            
            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (newValue != oldValue)
            {
                requestValueChanges.Values.insert({i, newValue});
                actorValuesComponent.CurrentActorValues.ActorValuesList[i] = newValue;
            }

            float newMaxValue = pActor->GetActorPermanentValue(i);
            float oldMaxValue = actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i];
            if (newMaxValue != oldMaxValue)
            {
                requestMaxValueChanges.Values.insert({i, newMaxValue});
                actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i] = newMaxValue;
            }
        }

        // Players only: NPC copies are corrected in OnActorValueChanges, and there are far more of them.
        if (cRefresh && isPlayer)
        {
            for (const uint32_t key : {uint32_t(ActorValueInfo::kHealth), uint32_t(ActorValueInfo::kMagicka), uint32_t(ActorValueInfo::kStamina)})
            {
                requestValueChanges.Values[key] = pActor->GetActorValue(key);
                requestMaxValueChanges.Values[key] = pActor->GetActorPermanentValue(key);
            }
        }

        // The server took a blow's change, and something here may have given the health back since.
        // Unchanged, it would not be sent, and the server would stay lower: its number is what a hand-off and a joining player take.
        // The copies take it only when it is 25 or more away from theirs, as any value.
        // A small change still waiting to go out would land after it and take the server lower again, so it waits for the next broadcast.
        if (m_healthResends.contains(formIdComponent.Id) && (m_smallHealthChanges.contains(localComponent.Id) || m_smallSpellChanges.contains(localComponent.Id)))
            stillToResend.insert(formIdComponent.Id);
        else if (m_healthResends.contains(formIdComponent.Id) && !requestValueChanges.Values.contains(ActorValueInfo::kHealth))
        {
            const float cHealth = pActor->GetActorValue(ActorValueInfo::kHealth);
            requestValueChanges.Values[ActorValueInfo::kHealth] = cHealth;
            static std::chrono::steady_clock::time_point s_nextSaid{};
            if (refreshNow >= s_nextSaid)
            {
                s_nextSaid = refreshNow + 10s;
                spdlog::info("Health of {:X} sent unchanged at {:.0f}: a blow on it went to the other games as a change", pActor->formID, cHealth);
            }
        }

        if (requestValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestValueChanges);
        }

        if (requestMaxValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestMaxValueChanges);
        }
    }
    m_healthResends = std::move(stillToResend);
}

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto hitteeIt = Utils::FindByFormId(view, acEvent.HitteeId);

    if (!hitteeIt)
    {
        spdlog::warn("Health change event form id component not found, form id: {:X}", acEvent.HitteeId);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*hitteeIt);
    if (!serverIdRes.has_value())
    {
        // Named, once per actor every 10 s with what it missed meanwhile: a fight logged this ten times a second, saying nobody.
        struct Unsent
        {
            std::chrono::steady_clock::time_point Next{};
            uint32_t Count{};
            float Delta{};
        };
        static TiltedPhoques::Map<uint32_t, Unsent> s_unsent;
        if (s_unsent.size() > 256)
            s_unsent.clear();
        auto& unsent = s_unsent[acEvent.HitteeId];
        ++unsent.Count;
        unsent.Delta += acEvent.DeltaHealth;
        if (const auto cNow = std::chrono::steady_clock::now(); cNow >= unsent.Next)
        {
            const auto* pActor = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));
            const auto* pBase = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
            spdlog::warn("Health change of {:X} ('{}', base {:X}) not sent: it has no server id{}; {} change(s), {:.1f} health in all, since the last such line",
                         acEvent.HitteeId, pBase ? pBase->fullName.value.AsAscii() : "?", pBase ? pBase->formID : 0,
                         m_world.all_of<WaitingForAssignmentComponent>(*hitteeIt) ? ", its assignment is still pending" : " and none is pending", unsent.Count, unsent.Delta);
            unsent = Unsent{cNow + std::chrono::seconds(10)};
        }
        return;
    }

    uint32_t serverId = serverIdRes.value();

    if (acEvent.DeltaHealth < 0.f && m_world.all_of<LocalComponent>(*hitteeIt))
        m_healthResends.insert(acEvent.HitteeId);

    if (acEvent.DeltaHealth > -1.0f && acEvent.DeltaHealth < 1.0f)
    {
        auto& smallChanges = acEvent.FromSpell ? m_smallSpellChanges : m_smallHealthChanges;
        if (smallChanges.find(serverId) == smallChanges.end())
            smallChanges[serverId] = acEvent.DeltaHealth;
        else
            smallChanges[serverId] += acEvent.DeltaHealth;
        return;
    }

    RequestHealthChangeBroadcast requestHealthChange;
    requestHealthChange.Id = serverId;
    requestHealthChange.DeltaHealth = acEvent.DeltaHealth;
    requestHealthChange.Tick = acEvent.FromSpell ? 0 : m_transport.GetClock().GetCurrentTick();

    m_transport.Send(requestHealthChange);

    spdlog::debug("Sent out delta health through collection: {:X}:{:f}", serverId, acEvent.DeltaHealth);
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    static Throttle s_sendThrottle(250ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    if (!m_smallHealthChanges.empty())
    {
        for (auto& value : m_smallHealthChanges)
        {
            RequestHealthChangeBroadcast requestHealthChange;
            requestHealthChange.Id = value.first;
            requestHealthChange.DeltaHealth = value.second;
            requestHealthChange.Tick = m_transport.GetClock().GetCurrentTick();

            m_transport.Send(requestHealthChange);

            spdlog::debug("Sent out delta health through timer, {:X}:{:f}", value.first, value.second);
        }

        m_smallHealthChanges.clear();
    }

    for (auto& value : m_smallSpellChanges)
    {
        RequestHealthChangeBroadcast requestHealthChange;
        requestHealthChange.Id = value.first;
        requestHealthChange.DeltaHealth = value.second;

        m_transport.Send(requestHealthChange);
    }

    m_smallSpellChanges.clear();
}

void ActorValueService::RunDeathStateUpdates() noexcept
{
    static Throttle s_sendThrottle(250ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    auto localView = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : localView)
    {
        const auto& formIdComponent = localView.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto& localComponent = localView.get<LocalComponent>(entity);

        bool isDead = pActor->IsDead();
        if (isDead != localComponent.IsDead)
            SendDeathState(localComponent, pActor, isDead, 0);
    }
}

void ActorValueService::SendDeathState(LocalComponent& aLocal, Actor* apActor, bool aIsDead, uint32_t aKillerPlayerId) noexcept
{
    // A new death of an NPC: its body's resting pose goes out at least once, for the others to lay their copy out in.
    if (aIsDead && !aLocal.IsDead && apActor != PlayerCharacter::Get())
        VRBodySync::NoteDeathHere(apActor->formID);
    if (!aIsDead)
        StuckArrows::OnRevived(aLocal.Id);
    aLocal.IsDead = aIsDead;
    // A life after a death here is this game's revive: the server marks it from this report, and an announcement from here carries it (BuildActorData).
    if (ActorExtension* pExtension = apActor->GetExtension())
        pExtension->RevivedSinceDeath = !aIsDead;
    if (aKillerPlayerId)
        spdlog::debug("Death sync: local actor {:X} is now dead by player {}'s blow, telling the server", apActor->formID, aKillerPlayerId);
    else
        spdlog::info("Death sync: local actor {:X} is now {}, telling the server", apActor->formID, aIsDead ? "dead" : "alive");

    RequestDeathStateChange requestChange;
    requestChange.Id = aLocal.Id;
    requestChange.OwnershipEpoch = aLocal.OwnershipEpoch;
    requestChange.IsDead = aIsDead;
    requestChange.KillerPlayerId = aIsDead ? aKillerPlayerId : 0;

    m_transport.Send(requestChange);
    // A blow right after on the body needs no repeat of it (RepeatDeath).
    if (aIsDead)
        m_deathRepeats[apActor->formID] = std::chrono::steady_clock::now();
}

void ActorValueService::RepeatDeath(Actor* apActor, uint32_t aServerId) noexcept
{
    const auto cEntity = Utils::FindEntityByServerId(aServerId);
    auto* pLocal = cEntity ? m_world.try_get<LocalComponent>(*cEntity) : nullptr;
    if (!pLocal)
        return;

    const auto cNow = std::chrono::steady_clock::now();
    if (m_deathRepeats.size() > 64)
    {
        for (auto repeat = m_deathRepeats.begin(); repeat != m_deathRepeats.end();)
            repeat = cNow - repeat->second >= std::chrono::minutes(1) ? m_deathRepeats.erase(repeat) : std::next(repeat);
    }
    auto& last = m_deathRepeats[apActor->formID];
    if (!KillCredit::RepeatDue(last, cNow))
        return;
    last = cNow;

    // Its copy there still stands, waiting for a death word that went missing; a blow on a body credits nobody.
    spdlog::debug("Death sync: another player struck {:X}, already dead here; its death is sent again", apActor->formID);
    SendDeathState(*pLocal, apActor, true, 0);
}

void ActorValueService::RunActorValuesUpdates() noexcept
{
    static Throttle s_sendThrottle(1000ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    BroadcastActorValues();
}

namespace
{
double SteadyMilliseconds() noexcept
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

namespace
{
// The defender's rule: the defender's game decides whether a hit was blocked.
// The other player's copy shows here as late as its playback clock plays it, so a hit at tick t is seen at t plus that delay.
// The hit is held until then, and dropped if this player's hand or held item met that player's weapon around then.
// The delay before the copy's first update arrived:
constexpr uint64_t kDefaultHitSeenAfter = 225;
// Blades meet before the hit lands, and a contact is only taken in the next frames.
// Measured between two headsets at a 225 ms delay, a blocking clash came 120-290 ms before the hit's own tick.
// So the blades crossed up to about 515 ms before the hit on the attacker's side, whatever the delay.
// A clash counts from 525 ms before the hit was seen here up to 100 ms after.
constexpr uint64_t kBlockBefore = 525;
constexpr uint64_t kBlockAfter = 100;
} // namespace

void ActorValueService::OnClash(const ClashEvent& acEvent) noexcept
{
    m_clashes.push_back({acEvent.FormId, acEvent.Tick});
}

void ActorValueService::RunHealthEases() noexcept
{
    if (m_healthEases.empty())
        return;

    const double cNow = SteadyMilliseconds();
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    for (auto it = m_healthEases.begin(); it != m_healthEases.end();)
    {
        // Only the copy the walk was made for: the same character, epoch and actor, still another player's.
        const uint32_t cServerId = it->second.For.ServerId;
        const auto cEntity =
            std::find_if(std::begin(view), std::end(view), [&view, cServerId](entt::entity aEntity) { return view.get<RemoteComponent>(aEntity).Id == cServerId; });
        HealthEase::Binding now{};
        if (cEntity != std::end(view))
            now = {cServerId, view.get<RemoteComponent>(*cEntity).OwnershipEpoch, view.get<FormIdComponent>(*cEntity).Id};
        Actor* pActor = now.FormId == it->first ? Cast<Actor>(TESForm::GetById(it->first)) : nullptr;
        const bool cRemotePlayer = pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer();
        if (!pActor || pActor->IsDead() || !HealthEase::StillFor(it->second, now, cRemotePlayer))
        {
            it = m_healthEases.erase(it);
            continue;
        }
        if (HealthEase::StepDue(it->second, cNow))
        {
            it.value().Last = cNow;
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, HealthEase::ValueAt(it->second, cNow));
        }
        it = HealthEase::IsDone(it->second, cNow) ? m_healthEases.erase(it) : std::next(it);
    }
}

void ActorValueService::RunHeldHits() noexcept
{
    if (m_heldHits.empty() && m_clashes.empty())
        return;

    const uint64_t cNow = m_transport.GetClock().GetCurrentTick();
    // Kept a while: a hit can arrive late.
    m_clashes.erase(std::remove_if(m_clashes.begin(), m_clashes.end(), [cNow](const auto& acClash) { return acClash.second + 30000 < cNow; }), m_clashes.end());

    for (auto it = m_heldHits.begin(); it != m_heldHits.end();)
    {
        // The attacker's copy, and how far behind the attacker it is shown here right now.
        uint32_t copy = 0;
        uint64_t seenAfter = kDefaultHitSeenAfter;
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (const auto entity : view)
            if (view.get<PlayerComponent>(entity).Id == it->AttackerPlayerId)
            {
                copy = view.get<FormIdComponent>(entity).Id;
                if (const auto* pInterpolation = m_world.try_get<InterpolationComponent>(entity))
                    if (const uint64_t cShown = InterpolationSystem::GetPlaybackTick(*pInterpolation); cShown && cShown < cNow)
                        seenAfter = std::clamp<uint64_t>(cNow - cShown, 16, 400);
                break;
            }

        const uint64_t cSeen = it->Tick + seenAfter;
        // A tick from the future (a clock not yet in step) is not waited for.
        if (cNow < cSeen + kBlockAfter && it->Tick < cNow + 5000)
        {
            ++it;
            continue;
        }

        const auto clash = std::find_if(m_clashes.begin(), m_clashes.end(), [copy, cSeen](const auto& acClash)
                                        { return copy && acClash.first == copy && acClash.second + kBlockBefore >= cSeen && acClash.second <= cSeen + kBlockAfter; });
        if (clash != m_clashes.end())
        {
            const int64_t cOffset = static_cast<int64_t>(clash->second) - static_cast<int64_t>(cSeen);
            spdlog::info("Defender's rule: a hit of {:.0f} from player {} is blocked: our weapon met theirs {} ms {} it was seen here", -it->DeltaHealth, it->AttackerPlayerId,
                         std::abs(cOffset), cOffset <= 0 ? "before" : "after");
        }
        else
        {
            spdlog::info("Defender's rule: a hit of {:.0f} from player {} lands, {} ms after it happened; nothing of ours met their weapon", -it->DeltaHealth, it->AttackerPlayerId,
                         static_cast<int64_t>(cNow) - static_cast<int64_t>(it->Tick));
            ApplyHealthChange(*it);
        }
        it = m_heldHits.erase(it);
    }
}

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) noexcept
{
    // Another player's hit on this player counts only between players who may fight.
    if (acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId && !PvpRules::IsAllowedWithPlayer(acMessage.AttackerPlayerId) &&
        Utils::GetByServerId<Actor>(acMessage.Id) == PlayerCharacter::Get())
    {
        static std::chrono::steady_clock::time_point s_nextSaid{};
        if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextSaid)
        {
            s_nextSaid = cNow + std::chrono::seconds(5);
            spdlog::debug(
                "PvP: a hit of {:.0f} from player {} not taken: the two may not fight here (PvP off, or one plays flat Skyrim and the other VR)", -acMessage.DeltaHealth,
                acMessage.AttackerPlayerId);
        }
        return;
    }

    // Another player's hit on this player waits for the defender's rule (RunHeldHits): in VR a blade contact can stop it,
    // on flat the player's block (BlockService), and with no flat block it is not held.
    // A spell's hit has no tick and goes straight on: no weapon can block it, and it is not an arrow's.
#ifdef SKYRIMVR
    constexpr bool cHeld = true;
#else
    const bool cHeld = BlockService::Active();
#endif
    if (cHeld && acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId && acMessage.Tick)
    {
        Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
        if (pActor && pActor == PlayerCharacter::Get())
        {
            // An arrow: that player's arrow is replayed here, and its hit or miss here decides (the defender's own copy).
            const uint32_t cCopy = PlayerCopyFormId(acMessage.AttackerPlayerId);
            const Actor* pCopy = Cast<Actor>(TESForm::GetById(cCopy));
            if (pCopy && PvpProjectiles::Recent(cCopy, glm::distance(glm::vec3(pCopy->position), glm::vec3(pActor->position))))
            {
                const float cTaken = PvpProjectiles::ArrowReported(cCopy, -acMessage.DeltaHealth);
                if (cTaken <= 0.f)
                {
                    spdlog::info("PvP: a hit of {:.0f} from player {} not taken: its projectile is replayed here, and that copy's hit decides", -acMessage.DeltaHealth,
                                 acMessage.AttackerPlayerId);
                    return;
                }

                // The replayed arrow got here first and took what this game makes of it, so only the rest of its shooter's number is left.
                spdlog::info("PvP: an arrow of player {} took {:.0f} here and its shooter's number is {:.0f}; the rest is taken", acMessage.AttackerPlayerId, cTaken,
                             -acMessage.DeltaHealth);
                if (-acMessage.DeltaHealth > cTaken)
                {
                    NotifyHealthChangeBroadcast rest = acMessage;
                    rest.DeltaHealth = acMessage.DeltaHealth + cTaken;
                    ApplyHealthChange(rest);
                }
                return;
            }
            m_heldHits.push_back(acMessage);
            return;
        }
    }
    ApplyHealthChange(acMessage);
}

void ActorValueService::ApplyHealthChange(const NotifyHealthChangeBroadcast& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.Id);
        return;
    }

    // A blow lands at once, from where the health is now; the walk to the last report is over.
    m_healthEases.erase(pActor->formID);

    const float cHealthBefore = pActor->GetActorValue(ActorValueInfo::kHealth);
    const bool cDeadBefore = pActor->IsDead();
    float newHealth = cHealthBefore + acMessage.DeltaHealth;

    // A player's copy is essential and goes down at zero, but only its owner's game decides when that player is down.
    // So the copy keeps a margin above zero, enough to take another hit.
    if (pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer() && newHealth < kRemotePlayerHealthFloor)
    {
        spdlog::info("Remote player {:X} copy kept at {:.0f} health instead of {:.0f}; its owner decides", pActor->formID, kRemotePlayerHealthFloor, newHealth);
        newHealth = kRemotePlayerHealthFloor;
    }
    // Nor does a health value alone kill an NPC copy: its owner's death word does (OnDeathStateChange).
    // Killed here while its owner's actor lived on, the copy lay dead where it fell while the real one fought on.
    else if (IsNpcCopy(pActor) && newHealth < kNpcCopyHealthFloor)
    {
        SayNpcCopyKeptUp(pActor, newHealth);
        newHealth = kNpcCopyHealthFloor;
    }

    // Another player's blow on an actor this game runs: the game's handling of it below names no attacker, so a plugin there can ask (BlowAttacker).
    // Only here: on a copy, the sender can be the game that runs it, reporting a blow of its own NPC.
    const ActorExtension* pHitExtension = pActor->GetExtension();
    const bool cBlowHere = acMessage.DeltaHealth < 0.f && pHitExtension && !pHitExtension->IsRemote();
    if (cBlowHere)
        m_healthResends.insert(pActor->formID);
    t_blowAttacker = cBlowHere && acMessage.AttackerPlayerId ? PlayerCopyFormId(acMessage.AttackerPlayerId) : 0;
    pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, newHealth);
    t_blowAttacker = 0;

    // Another player's blow on this player: the death card may name them.
    if (acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId && pActor == PlayerCharacter::Get())
        DeathCause::RecordPlayerHit(acMessage.AttackerPlayerId);

    const float health = pActor->GetActorValue(ActorValueInfo::kHealth);

    if (acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId && !acMessage.Tick && pActor == PlayerCharacter::Get())
        SaySpellHits(acMessage.AttackerPlayerId, -acMessage.DeltaHealth, health);

    // Another player's blow on an actor other than this player: what it did here, so that one that never kills can be told apart.
    if (acMessage.DeltaHealth < 0.f && acMessage.AttackerPlayerId && pActor != PlayerCharacter::Get())
    {
        const ActorExtension* pExtension = pActor->GetExtension();
        spdlog::info("Death sync: player {}'s blow of {:.0f} on {} {:X}: health {:.0f} to {:.0f}{}{}{}", acMessage.AttackerPlayerId, -acMessage.DeltaHealth,
                     !pExtension ? "actor" : pExtension->IsRemotePlayer() ? "player copy" : pExtension->IsRemote() ? "NPC copy" : "this game's NPC", pActor->formID, cHealthBefore,
                     health, pActor->IsDead() ? ", already dead" : "", pActor->parentCell ? "" : ", in no cell here", pActor->GetNiNode() ? "" : ", no 3D here");
    }

    if (cBlowHere && cDeadBefore && acMessage.AttackerPlayerId && !pHitExtension->IsPlayer())
        RepeatDeath(pActor, acMessage.Id);

    if (!pActor->IsDead() && health <= 0.f)
    {
        ActorExtension* pExtension = pActor->GetExtension();
        // Players should never be killed.
        // Nor should an actor that is bleeding out: that downed state is recoverable, and essential NPCs stay there.
        if (pExtension && !pExtension->IsPlayer() && !pActor->actorState.IsBleedingOut())
        {
            // An essential NPC refuses to die, so IsDead() stays false and every later health broadcast would try again.
            // After three refusals the actor is left to its owner; repeated kills only disturb its ragdoll and recovery.
            static TiltedPhoques::Map<uint32_t, uint32_t> s_refused;
            auto& refusals = s_refused[pActor->formID];
            if (refusals < 3)
            {
                pActor->Kill();
                if (pActor->IsDead())
                {
                    s_refused.erase(pActor->formID);
                    spdlog::info("Death sync: health broadcast killed actor {:X}", pActor->formID);
                    // The death goes out now, with the player whose blow it was, so that their game credits them with the kill.
                    if (cBlowHere && KillCredit::CreditsAttacker(acMessage.DeltaHealth, acMessage.AttackerPlayerId))
                    {
                        const auto cEntity = Utils::FindEntityByServerId(acMessage.Id);
                        if (auto* pLocal = cEntity ? m_world.try_get<LocalComponent>(*cEntity) : nullptr)
                            SendDeathState(*pLocal, pActor, true, acMessage.AttackerPlayerId);
                    }
                }
                else if (++refusals == 3)
                    spdlog::warn("Death sync: actor {:X} refuses to die (essential, or protected); leaving it to its owner", pActor->formID);
            }
        }
    }

    m_world.GetCombatService().FightBack(pActor, acMessage.AttackerPlayerId, acMessage.DeltaHealth);

    // TODO(cosideci): find fix for player health sync so this can be used again
    /*
    if (pActor->GetExtension()->IsRemotePlayer())
        World::Get().GetOverlayService().SetPlayerHealthPercentage(pActor->formID);
    */
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto itor = Utils::FindById<RemoteComponent>(view, acMessage.Id);

    if (itor)
    {
        const auto& remoteFound = view.get<RemoteComponent>(*itor);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("an actor value change", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    if (!itor)
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    const bool isRemotePlayer = pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer();

    for (auto [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        // A player's copy never takes invisibility (actor value 54): it would stay hittable but undrawn until a reconnect.
        // Whatever hides the owner on his own screen stays there.
        if (isRemotePlayer && key == ActorValueInfo::kInvisibility)
        {
            if (value != 0.f)
                spdlog::info("Remote player {:X} copy refused invisibility {:.2f} from its owner; a copy is always drawn", pActor->formID, value);
            continue;
        }

        // A player's copy takes its owner's health as the truth.
        // With damage deltas alone it never heals, and at zero the essential copy lies down for good.
        // NPC copies keep the delta path, which also carries their deaths.
        if (key == ActorValueInfo::kHealth)
        {
            if (pActor->IsDead())
                continue;

            if (!isRemotePlayer)
            {
                // A same-life alive word followed by a genuine healthy owner value can recover an observed graph-only fall.
                // Record even when the value needs no correction here; this is packet evidence, not a local health guess.
                InterpolationSystem::NoteOwnerHealth(m_world, pActor, value, std::isfinite(value) && value > kNpcCopyHealthFloor);
                view.get<RemoteComponent>(*itor).OwnerHealth = value;
                // An NPC's health here is only the sum of the damage deltas that arrived, so it drifts when one is lost.
                // Only a gap that matters is corrected: the delta path carries deaths, and the two would fight every frame.
                // Corrections are logged, to show whether deltas are being dropped.
                const float current = pActor->GetActorValue(ActorValueInfo::kHealth);
                const float gap = std::abs(current - value);
                constexpr float cWorthCorrecting = 25.f;
                if (gap < cWorthCorrecting)
                    continue;

                static std::chrono::steady_clock::time_point s_nextSaid{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= s_nextSaid)
                {
                    s_nextSaid = now + 5s;
                    spdlog::info("NPC {:X} health corrected from {:.0f} to its owner's {:.0f}", pActor->formID, current, value);
                }

                // A number alone does not kill a copy (ApplyHealthChange): with no death played it would stay on its feet.
                if (value < kNpcCopyHealthFloor)
                {
                    SayNpcCopyKeptUp(pActor, value);
                    value = kNpcCopyHealthFloor;
                }

                pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, value);
                continue;
            }

            const float current = pActor->GetActorValue(ActorValueInfo::kHealth);
            if (std::abs(current - value) >= 10.f)
                spdlog::info("Remote player {:X} copy health corrected from {:.0f} to the owner's {:.0f}", pActor->formID, current, value);

            // The owner's death arrives as health at or below zero, which would put the copy into an unrecoverable bleedout.
            // A copy never goes down: the owner's game decides, and the respawn replaces the copy.
            if (value < kRemotePlayerHealthFloor)
            {
                spdlog::info("Remote player {:X} copy kept at {:.0f} health instead of the owner's {:.0f}; a copy never goes down", pActor->formID, kRemotePlayerHealthFloor, value);
                value = kRemotePlayerHealthFloor;
            }
        }

        // A small change in another player's health is walked to over the second until the next report, not stepped to.
        if (isRemotePlayer && key == ActorValueInfo::kHealth)
        {
            const float cCurrent = pActor->GetActorValue(ActorValueInfo::kHealth);
            if (HealthEase::Eases(cCurrent, value))
            {
                const double cNow = SteadyMilliseconds();
                const HealthEase::Binding cFor{acMessage.Id, acMessage.OwnershipEpoch, pActor->formID};
                m_healthEases[pActor->formID] = HealthEase::Ramp{cCurrent, value, cNow, cNow, cFor};
                continue;
            }
            m_healthEases.erase(pActor->formID);
        }

        spdlog::debug("Actor value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        if (key == ActorValueInfo::kStamina || key == ActorValueInfo::kMagicka || key == ActorValueInfo::kHealth)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);
            continue;
        }
        pActor->SetActorValue(key, value);
    }
}

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto it = Utils::FindById<RemoteComponent>(view, acMessage.Id);

    if (it)
    {
        const auto& remoteFound = view.get<RemoteComponent>(*it);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("a health broadcast", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    if (!it)
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) const noexcept
{
    // A copy still waiting to be made, or for its 3D, has no form id yet or no body to kill, but it must not miss the death.
    auto view = m_world.view<RemoteComponent>();

    // Separated from the epoch test so a stale epoch can be told apart from a character we simply do not have.
    const auto it = Utils::FindById<RemoteComponent>(view, acMessage.Id);

    if (it)
    {
        const auto& remoteFound = view.get<RemoteComponent>(*it);
        if (acMessage.OwnershipEpoch == 0 || remoteFound.OwnershipEpoch != acMessage.OwnershipEpoch)
        {
            ReportEpochMiss("a death state change", acMessage.Id, acMessage.OwnershipEpoch, remoteFound.OwnershipEpoch);
            return;
        }
    }

    // Every exit below logs why: "no copy here", "no actor" and "already dead here" are different bugs.
    // Rate limited; a death arrives once per actor, so this is a handful of lines per session.
    const auto say = [&acMessage](const char* acpWhy)
    {
        static std::chrono::steady_clock::time_point s_next{};
        const auto now = std::chrono::steady_clock::now();
        if (now < s_next)
            return;
        s_next = now + std::chrono::seconds(1);
        spdlog::info("Death sync: {} for server id {:X} arrived and was not applied: {}", acMessage.IsDead ? "a death" : "a revival", acMessage.Id, acpWhy);
    };

    if (!it)
    {
        say("no copy of that character here");
        return;
    }

    // Kept even when the copy is already in that state here: the corpse placement waits for this word.
    auto& remote = view.get<RemoteComponent>(*it);
    // A death or new life ends every observation of this copy's previous fall, even between render frames.
    const bool cOwnerLifeChanged = remote.DeadAtOwner != acMessage.IsDead;
    if (acMessage.IsDead || cOwnerLifeChanged)
        InterpolationSystem::Forget(remote.CachedRefId);
    remote.DeadAtOwner = acMessage.IsDead;
    // Its owner's word on its life has come, so a death seen here before the hand-off no longer counts.
    remote.DeadHere = false;
    // Brought back, it keeps its head: what was told for its death is not cut off it now, nor after a later death.
    if (!acMessage.IsDead)
        StuckArrows::OnRevived(acMessage.Id);

    // A life sent here is its owner's revive, which the server now marks (RevivedSinceDeath); a death clears that.
    // The copy is killed or brought back once it has its 3D, from its spawn request (CharacterService::RunRemoteUpdates).
    if (auto* pWaiting = m_world.try_get<WaitingFor3D>(*it))
    {
        pWaiting->SpawnRequest.RevivedSinceDeath = !acMessage.IsDead;
        pWaiting->SpawnRequest.IsDead = acMessage.IsDead;
        spdlog::info("Death sync: server id {:X} is {} at its owner while its copy waits to be made here; it will be made so", acMessage.Id,
                     acMessage.IsDead ? "dead" : "alive");
        return;
    }

    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*it);
    if (!pFormIdComponent)
    {
        say("its copy has no actor in the game");
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));

    if (!pActor)
    {
        say("its copy has no actor in the game");
        return;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (pExtension->IsPlayer())
        return;

    // Kept here too, so that the mark goes with the character if this game announces it again.
    pExtension->RevivedSinceDeath = !acMessage.IsDead;

    if (pActor->IsDead() != acMessage.IsDead)
    {
        // This player's blow killed it in its owner's game: it dies here with this player as its killer, so the kill is theirs here
        // (kill stats, any bounty, and scripts asking who killed it). Its owner's game killed it with no killer.
        const bool cCredited = acMessage.IsDead && KillCredit::CreditedHere(acMessage.KillerPlayerId, m_transport.GetLocalPlayerId());
        if (cCredited)
            pActor->Kill(PlayerCharacter::Get());
        else
            acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
        spdlog::debug(
            "Death sync: remote actor {:X} set {} by its owner{} (now dead: {})", pActor->formID, acMessage.IsDead ? "dead" : "alive",
            cCredited ? ", killed here by this player, whose blow it was" : "", pActor->IsDead());
    }
    else
    {
        // Not a failure: the copy died from a hit landed on this side.
        // Its body then falls under this side's ragdoll and lies elsewhere than the owner's.
        spdlog::info("Death sync: remote actor {:X} was already {} here when its owner's word arrived", pActor->formID, acMessage.IsDead ? "dead" : "alive");
    }
    if (!acMessage.IsDead && !cOwnerLifeChanged && !pActor->IsDead())
        InterpolationSystem::NoteOwnerAlive(m_world, pActor);
}
