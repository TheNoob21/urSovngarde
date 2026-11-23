#include <TiltedOnlinePCH.h>
#include <Helpers/PerfScope.h>

#include <Services/HitReactionService.h>
#include <Services/GameExtension.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/RequestHitReaction.h>
#include <Messages/NotifyHitReaction.h>

#include <Games/References.h>
#include <Games/ActorExtension.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Forms/BGSAction.h>
#include <PlayerCharacter.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>
#include <Games/Skyrim/AnimationGraphEvents.h>

#include <algorithm>
#include <limits>
#include <mutex>

using namespace std::chrono_literals;
using HitReactions::Kind;

namespace
{
// A hit's reaction and the hit itself reach this service apart, in either order, from the game's threads.
struct Pending
{
    std::chrono::steady_clock::time_point At{};
    uint8_t Value{};
};

std::mutex s_lock;
TiltedPhoques::Map<uint32_t, Pending> s_hits;      // this player's hits on NPC copies: the hit's flags
TiltedPhoques::Map<uint32_t, Pending> s_reactions; // reactions the game wanted on NPC copies: the kind

// A reaction is the hit's when the game asks for it at most this long before the hit's event, which can be sent after it.
// After the hit, it has HitReactions::kWaitForGame, when the hit's flags decide instead.
constexpr auto kPairBefore = 150ms;
// One reaction sent per copy in this time: a refused action can be tried again every frame.
constexpr auto kSendEvery = 500ms;

struct HitSink final : BSTEventSink<TESHitEvent>
{
    BSTEventResult OnEvent(const TESHitEvent* apEvent, const EventDispatcher<TESHitEvent>*) override
    {
        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (!apEvent || !apEvent->hit || !pPlayer)
            return BSTEventResult::kOk;
        if (!apEvent->hitter)
            return BSTEventResult::kOk;
        // This player's blow on an NPC copy, which may stagger it; or a copy's blow this player blocked, which may make it recoil.
        TESObjectREFR* pCopy = nullptr;
        if (apEvent->hitter == pPlayer)
            pCopy = apEvent->hit;
        else if (apEvent->hit == pPlayer && (apEvent->flags & HitReactions::kBlocked))
            pCopy = apEvent->hitter;
        Actor* pActor = pCopy ? Cast<Actor>(pCopy) : nullptr;
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        if (pExtension && pExtension->IsRemote() && !pExtension->IsPlayer())
            HitReactionService::OnCopyHit(pActor->formID, apEvent->flags);
        return BSTEventResult::kOk;
    }
};

HitSink s_sink;

const char* KindName(const Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::kRecoil: return "recoil";
    case Kind::kLargeRecoil: return "large recoil";
    default: return "stagger";
    }
}

// Once per actor every 10 s, so that a fight's blows say what happened without a line each.
bool Said(TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point>& aSaidAt, const uint32_t aFormId) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    if (aSaidAt.size() > 256)
        aSaidAt.clear();
    auto& saidAt = aSaidAt[aFormId];
    if (saidAt.time_since_epoch().count() && cNow - saidAt < 10s)
        return true;
    saidAt = cNow;
    return false;
}

float GraphFloat(Actor* apActor, const char* acpName) noexcept
{
    BSFixedString name(acpName);
    float value = 0.f;
    if (!apActor->animationGraphHolder.GetVariableFloat(&name, &value))
        return std::numeric_limits<float>::quiet_NaN();
    return value;
}

bool GraphBool(Actor* apActor, const char* acpName) noexcept
{
    BSFixedString name(acpName);
    bool value = false;
    return apActor->animationGraphHolder.GetVariableBool(&name, &value) && value;
}

void SetStaggerFloats(Actor* apActor, const float aMagnitude, const float aDirection) noexcept
{
    static BSFixedString s_magnitude("staggerMagnitude");
    static BSFixedString s_direction("staggerDirection");
    apActor->animationGraphHolder.SetVariableFloat(&s_magnitude, aMagnitude);
    apActor->animationGraphHolder.SetVariableFloat(&s_direction, aDirection);
}

std::optional<uint32_t> ServerIdOf(World& aWorld, const uint32_t aFormId) noexcept
{
    const auto cEntity = Utils::FindByFormId(aWorld.view<FormIdComponent>(entt::exclude<ObjectComponent>), aFormId);
    return cEntity ? Utils::GetServerId(*cEntity) : std::nullopt;
}
} // namespace

HitReactionService::HitReactionService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&HitReactionService::OnUpdate>(this);
    m_notifyConnection = aDispatcher.sink<NotifyHitReaction>().connect<&HitReactionService::OnNotifyHitReaction>(this);
    EventDispatcherManager::Get()->hitEvent.RegisterSink(&s_sink);
}

namespace
{
void NoteReaction(const uint32_t aFormId, const Kind aKind) noexcept
{
    std::scoped_lock lock(s_lock);
    // The first ask is the one that counts: the game may ask again each frame it is refused, which would keep an old reaction fresh.
    if (!s_reactions.count(aFormId) && s_reactions.size() < 256)
        s_reactions.emplace(aFormId, Pending{std::chrono::steady_clock::now(), static_cast<uint8_t>(aKind)});
}
} // namespace

void HitReactionService::OnCopyReaction(const uint32_t aFormId, const Kind aKind) noexcept
{
    NoteReaction(aFormId, aKind);
}

void HitReactionService::OnCopyRawStagger(const uint32_t aFormId) noexcept
{
    NoteReaction(aFormId, Kind::kStagger);
}

void HitReactionService::OnCopyHit(const uint32_t aFormId, const uint8_t aFlags) noexcept
{
    std::scoped_lock lock(s_lock);
    if (s_hits.size() < 256 || s_hits.count(aFormId))
    {
        // Two blows before the game's answer: either one's power or bash counts.
        auto& hit = s_hits[aFormId];
        hit.Value = hit.At.time_since_epoch().count() ? static_cast<uint8_t>(hit.Value | aFlags) : aFlags;
        hit.At = std::chrono::steady_clock::now();
    }
}

void HitReactionService::OnUpdate(const UpdateEvent&) noexcept
{
    PerfScope perfScope("HitReactionService::OnUpdate");

    TiltedPhoques::Map<uint32_t, Pending> hits;
    TiltedPhoques::Map<uint32_t, Pending> reactions;
    {
        std::scoped_lock lock(s_lock);
        if (s_hits.empty() && s_reactions.empty())
            return;
        hits = s_hits;
        reactions = s_reactions;
    }

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<uint32_t> doneHits;
    TiltedPhoques::Vector<uint32_t> doneReactions;

    // The game wanted a reaction on a copy this player just hit: that is what the hit did.
    for (const auto& [formId, reaction] : reactions)
    {
        const auto cHit = hits.find(formId);
        const bool cPaired = cHit != hits.end() && reaction.At + kPairBefore >= cHit->second.At && reaction.At <= cHit->second.At + HitReactions::kWaitForGame;
        if (!cPaired)
        {
            // No hit of this player's can still come for it.
            if (cNow - reaction.At <= kPairBefore)
                continue;
            doneReactions.push_back(formId);
            if (GameExtension::HasReactionObservers())
            {
                if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)))
                    GameExtension::Unpaired(formId, reaction.Value, GraphFloat(pActor, "staggerMagnitude"), GraphFloat(pActor, "staggerDirection"));
            }
            continue;
        }

        doneReactions.push_back(formId);
        doneHits.push_back(formId);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        const auto cServerId = pActor ? ServerIdOf(m_world, formId) : std::nullopt;
        if (!cServerId)
            continue;

        const auto cKind = static_cast<Kind>(reaction.Value);
        // The game set this hit's numbers on the copy's graph before it asked for the action; no other player's game writes these.
        float magnitude = GraphFloat(pActor, "staggerMagnitude");
        float direction = GraphFloat(pActor, "staggerDirection");
        if (!std::isfinite(magnitude) || magnitude <= 0.f)
            magnitude = HitReactions::FallbackMagnitude(cHit->second.Value).value_or(0.25f);
        const Actor* pPlayer = PlayerCharacter::Get();
        if (!std::isfinite(direction) && pPlayer)
        {
            direction = HitReactions::DirectionFrom(pActor->position.x, pActor->position.y, pActor->rotation.z, pPlayer->position.x, pPlayer->position.y);
        }
        Send(pActor, *cServerId, cKind, magnitude, direction, "as the game wanted here");
    }

    // A hit the game answered with nothing on the copy: a power attack or bash still staggers its owner's actor.
    for (const auto& [formId, hit] : hits)
    {
        if (std::find(doneHits.begin(), doneHits.end(), formId) != doneHits.end() || cNow - hit.At < HitReactions::kWaitForGame)
            continue;
        doneHits.push_back(formId);

        const auto cMagnitude = HitReactions::FallbackMagnitude(hit.Value);
        Actor* pActor = cMagnitude ? Cast<Actor>(TESForm::GetById(formId)) : nullptr;
        const auto cServerId = pActor ? ServerIdOf(m_world, formId) : std::nullopt;
        if (!cServerId)
            continue;

        const Actor* pPlayer = PlayerCharacter::Get();
        if (!pPlayer)
            continue;
        const float cDirection = HitReactions::DirectionFrom(pActor->position.x, pActor->position.y, pActor->rotation.z, pPlayer->position.x, pPlayer->position.y);
        // This copy plays its owner's stagger when its replay comes, with this hit's numbers.
        SetStaggerFloats(pActor, *cMagnitude, cDirection);
        Send(pActor, *cServerId, Kind::kStagger, *cMagnitude, cDirection, "from the hit being a power attack or bash");
    }

    std::scoped_lock lock(s_lock);
    for (const uint32_t cFormId : doneHits)
        if (const auto it = s_hits.find(cFormId); it != s_hits.end() && it->second.At <= hits[cFormId].At)
            s_hits.erase(it);
    for (const uint32_t cFormId : doneReactions)
        if (const auto it = s_reactions.find(cFormId); it != s_reactions.end() && it->second.At <= reactions[cFormId].At)
            s_reactions.erase(it);
}

void HitReactionService::Send(Actor* apActor, const uint32_t aServerId, const Kind aKind, float aMagnitude, float aDirection, const char* acpWhy) noexcept
{
    if (!m_transport.IsConnected() || !HitReactions::Sanitize(static_cast<uint8_t>(aKind), aMagnitude, aDirection))
        return;

    const auto cNow = std::chrono::steady_clock::now();
    if (m_sentAt.size() > 256)
        m_sentAt.clear();
    auto& sentAt = m_sentAt[apActor->formID];
    if (sentAt.time_since_epoch().count() && cNow - sentAt < kSendEvery)
        return;
    sentAt = cNow;

    RequestHitReaction request;
    request.Id = aServerId;
    request.Kind = static_cast<uint8_t>(aKind);
    request.Magnitude = aMagnitude;
    request.Direction = aDirection;
    m_transport.Send(request);

    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_saidAt;
    if (!Said(s_saidAt, apActor->formID))
        spdlog::debug(
            "Hit reaction: this player makes NPC copy {:X} {} (magnitude {:.2f}, direction {:.2f}, {}); sent to its owner", apActor->formID, KindName(aKind), aMagnitude,
            aDirection, acpWhy);
}

void HitReactionService::OnNotifyHitReaction(const NotifyHitReaction& acMessage) noexcept
{
    float magnitude = acMessage.Magnitude;
    float direction = acMessage.Direction;
    if (!HitReactions::Sanitize(acMessage.Kind, magnitude, direction))
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
    if (!pExtension || pExtension->IsPlayer())
        return;

    const auto cKind = static_cast<Kind>(acMessage.Kind);
    // The server never sends a game its own reaction back.
    switch (HitReactions::RoleOf(false, pExtension->IsLocal()))
    {
    case HitReactions::Role::kPlay: Play(pActor, cKind, magnitude, direction); break;
    case HitReactions::Role::kPresetFloats:
        if (cKind == Kind::kStagger)
            SetStaggerFloats(pActor, magnitude, direction);
        break;
    case HitReactions::Role::kNothing: break;
    }
}

uint32_t HitReactionService::PresentReaction(uint32_t aId, uint32_t aEpoch, uint32_t aKind, float aMagnitude, float aDirection, uint32_t aObserver)
{
    if (aKind > 2 || !HitReactions::Sanitize(static_cast<uint8_t>(aKind), aMagnitude, aDirection))
        return 0;
    auto* actor = Utils::GetByServerId<Actor>(aId);
    const auto* ext = actor ? actor->GetExtension() : nullptr;
    if (!ext || ext->IsPlayer() || ext->IsRemote() != !!aObserver || !aEpoch)
        return 0;
    if (aObserver)
    {
        const auto owner = Utils::GetRemoteOwnershipToken(actor->formID);
        if (!owner || owner->ServerId != aId || owner->OwnershipEpoch != aEpoch)
            return 0;
        if (aKind == 0)
            SetStaggerFloats(actor, aMagnitude, aDirection);
        return 1;
    }
    const auto entity = Utils::FindEntityByServerId(aId);
    const auto* local = entity ? m_world.try_get<LocalComponent>(*entity) : nullptr;
    return local && local->OwnershipEpoch == aEpoch && Play(actor, static_cast<Kind>(aKind), aMagnitude, aDirection);
}

bool HitReactionService::Play(Actor* apActor, const Kind aKind, const float aMagnitude, const float aDirection) noexcept
{
    HitReactions::ActorState state;
    state.IsPlayer = apActor->GetExtension()->IsPlayer();
    state.DeadOrDying = apActor->IsDead() || apActor->actorState.IsDeadOrDying();
    state.BleedingOut = apActor->actorState.IsBleedingOut();
    state.KnockedDown = apActor->actorState.IsKnockedDown();
    state.Flying = apActor->actorState.IsFlying();
    // SIT_SLEEP_STATE, bits 14-17 of flags1: sitting, in furniture, asleep or on a horse.
    state.SittingOrMounted = ((apActor->actorState.flags1 >> 14) & 0xF) != 0;
    state.Swimming = apActor->actorState.IsSwimming();
    state.Has3D = apActor->GetNiNode() != nullptr;
    if (state.Has3D)
    {
        state.InPairedAnimation = GraphBool(apActor, "bIsSynced");
        state.NoStagger = GraphBool(apActor, "bNoStagger");
        // This game runs the actor, so its own graph says whether it is already reacting.
        state.Staggering = GraphBool(apActor, "IsStaggering");
        state.Recoiling = GraphBool(apActor, "IsRecoiling");
    }

    const auto cNow = std::chrono::steady_clock::now();
    if (m_playedAt.size() > 256)
        m_playedAt.clear();
    auto& playedAt = m_playedAt[apActor->formID];

    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_saidAt;
    if (const auto cRefusal = HitReactions::CanPlay(state, aKind, cNow, playedAt); cRefusal != HitReactions::Refusal::kNone)
    {
        if (!Said(s_saidAt, apActor->formID))
            spdlog::debug("Hit reaction: another player's {} on NPC {:X} not played: it is {}", KindName(aKind), apActor->formID, HitReactions::RefusalName(cRefusal));
        return false;
    }

    auto* pAction = Cast<BGSAction>(TESForm::GetById(HitReactions::ActionOf(aKind)));
    if (!pAction)
        return false;

    if (aKind == Kind::kStagger)
        SetStaggerFloats(apActor, aMagnitude, aDirection);

    // The game's own action on this game's actor, so the replay shows it on every copy.
    TESActionData action(2, apActor, pAction, nullptr);
    const bool cPlayed = ActorMediator::Get()->PerformOwnAction(&action);
    if (cPlayed)
        playedAt = cNow;

    if (!Said(s_saidAt, apActor->formID))
        spdlog::debug(
            "Hit reaction: another player's {} on NPC {:X} (magnitude {:.2f}, direction {:.2f}) {}", KindName(aKind), apActor->formID, aMagnitude, aDirection,
            cPlayed ? "played" : "refused by the game");
    return cPlayed;
}
