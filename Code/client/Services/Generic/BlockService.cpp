#include <TiltedOnlinePCH.h>

#include <Services/BlockService.h>
#include <Services/TransportService.h>
#include <Services/PvpProjectiles.h>
#include <ClientTrust.h>
#ifndef SKYRIMVR
#include <Services/CopyWorkGate.h>
#include <Forms/TESObjectCELL.h>
#endif

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ClashEvent.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyClash.h>
#include <Messages/ClashRequest.h>

#include <Systems/InterpolationSystem.h>
#include <Games/Events.h>
#include <Events/EventDispatcher.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Forms/BGSAction.h>
#include <Misc/MiddleProcess.h>
#include <AI/AIProcess.h>
#include <HitReactions.h>

#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Forms/TESObjectWEAP.h>
#include <PlayerCharacter.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>

bool BlockService::Active() noexcept
{
#ifdef SKYRIMVR
    return false;
#else
    return true;
#endif
}

namespace
{

// The behavior graph's own block flag, which the game's block sets and the animation sync sends.
bool IsBlocking(Actor* apActor) noexcept
{
    static BSFixedString s_isBlocking("IsBlocking");
    bool value = false;
    return apActor->animationGraphHolder.GetVariableBool(&s_isBlocking, &value) && value;
}

// Where a block met a blow: just in front of this player, at chest height, towards the one who struck.
glm::vec3 BlockPoint(const Actor* apPlayer, const Actor* apCopy) noexcept
{
    const glm::vec3 cFrom(apPlayer->position);
    glm::vec3 toward = glm::vec3(apCopy->position) - cFrom;
    toward.z = 0.f;
    const float cLength = glm::length(toward);
    toward = cLength > 1.f ? toward / cLength : glm::vec3(std::sin(apPlayer->rotation.z), std::cos(apPlayer->rotation.z), 0.f);
    return cFrom + toward * 40.f + glm::vec3(0.f, 0.f, 100.f);
}

// The hand that holds the weapon of the one who struck: the right, unless only the left holds one.
uint8_t WeaponSide(const Actor* apCopy) noexcept
{
    if (Cast<TESObjectWEAP>(apCopy->GetEquippedWeapon(1)))
        return 1;
    return Cast<TESObjectWEAP>(apCopy->GetEquippedWeapon(0)) ? 0 : 1;
}

// HitData flags (CommonLibVR-NG RE/H/HitData.h).
constexpr uint32_t kHitBlocked = 1;

// The game's own numbers for a melee blow of apAggressor on apTarget as things stand now: HitData::Populate, which the game's melee hit
// runs before it applies one (Precision calls it the same way). It weighs the target's block: held or not, facing the blow, shield or
// weapon, the target's skill and perks.
std::optional<BlockService::GameBlow> GameBlowOf(Actor* apAggressor, Actor* apTarget) noexcept
{
    // HitData, 0x90 bytes; the offsets read below are CommonLibVR-NG's.
    using TCtor = void*(void* apThis);
    using TPopulate = void(void* apThis, Actor* apAggressor, Actor* apTarget, InventoryEntry* apWeapon, bool aLeftHand);
    POINTER_SKYRIMSE(TCtor, s_ctor, 43995, 42826);
    POINTER_SKYRIMSE(TPopulate, s_populate, 44001, 42832);
    if (!s_ctor.Get() || !s_populate.Get() || !apAggressor || !apTarget)
        return std::nullopt;

    const bool cLeftHand = WeaponSide(apAggressor) == 0;
    const MiddleProcess* pMiddle = apAggressor->currentProcess ? apAggressor->currentProcess->middleProcess : nullptr;
    InventoryEntry* pWeapon = pMiddle ? (cLeftHand ? pMiddle->leftEquippedObject : pMiddle->rightEquippedObject) : nullptr;

    alignas(16) std::array<uint8_t, 0x90> data{};
    s_ctor.Get()(data.data());
    s_populate.Get()(data.data(), apAggressor, apTarget, pWeapon, cLeftHand);

    BlockService::GameBlow blow{};
    std::memcpy(&blow.TotalDamage, data.data() + 0x50, sizeof(float));
    std::memcpy(&blow.PercentBlocked, data.data() + 0x5C, sizeof(float));
    std::memcpy(&blow.Stagger, data.data() + 0x68, sizeof(float));
    std::memcpy(&blow.Flags, data.data() + 0x80, sizeof(uint32_t));
    // The attack data it took (a counted pointer at 0x28) is given back.
    NiRefObject* pAttackData = nullptr;
    std::memcpy(&pAttackData, data.data() + 0x28, sizeof(pAttackData));
    if (pAttackData)
        pAttackData->DecRef();
    return blow;
}

// When the game itself last ran a blow of another player's copy on this player (their flat swing, played here), by the copy's form id.
// Its block, sound and stagger then already showed here; only the damage is the other game's.
std::mutex s_gameHitsLock;
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_gameHits;
constexpr auto kGameHitShown = std::chrono::milliseconds(1000);

struct HitSink final : BSTEventSink<TESHitEvent>
{
    BSTEventResult OnEvent(const TESHitEvent* apEvent, const EventDispatcher<TESHitEvent>*) override
    {
        if (!apEvent || !apEvent->hit || !apEvent->hitter || apEvent->hit != PlayerCharacter::Get())
            return BSTEventResult::kOk;
        Actor* pHitter = Cast<Actor>(apEvent->hitter);
        const ActorExtension* pExtension = pHitter ? pHitter->GetExtension() : nullptr;
        if (pExtension && pExtension->IsRemotePlayer())
        {
            std::scoped_lock lock(s_gameHitsLock);
            if (s_gameHits.size() > 64)
                s_gameHits.clear();
            s_gameHits[pHitter->formID] = std::chrono::steady_clock::now();
        }
        return BSTEventResult::kOk;
    }
};
HitSink s_hitSink;

bool GameShowedHit(const uint32_t aCopyFormId) noexcept
{
    std::scoped_lock lock(s_gameHitsLock);
    const auto it = s_gameHits.find(aCopyFormId);
    return it != s_gameHits.end() && std::chrono::steady_clock::now() - it->second < kGameHitShown;
}

// The game's stagger on this player, of the strength the game gave the blocked blow (the action the game itself asks for).
bool PlayStagger(Actor* apPlayer, const Actor* apFrom, const float aMagnitude) noexcept
{
    auto* pAction = Cast<BGSAction>(TESForm::GetById(HitReactions::ActionOf(HitReactions::Kind::kStagger)));
    if (!pAction)
        return false;
    static BSFixedString s_magnitude("staggerMagnitude");
    static BSFixedString s_direction("staggerDirection");
    const float cDirection = HitReactions::DirectionFrom(apPlayer->position.x, apPlayer->position.y, apPlayer->rotation.z, apFrom->position.x, apFrom->position.y);
    apPlayer->animationGraphHolder.SetVariableFloat(&s_magnitude, aMagnitude);
    apPlayer->animationGraphHolder.SetVariableFloat(&s_direction, cDirection);
    TESActionData action(2, apPlayer, pAction, nullptr);
    return ActorMediator::Get()->PerformOwnAction(&action);
}
} // namespace

BlockService::BlockService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
#ifndef SKYRIMVR
    // In VR, the blade contact is the block (CharacterService::SendClashes), and a clash is seen, heard and felt there.
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&BlockService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&BlockService::OnDisconnected>(this);
    m_healthConnection = aDispatcher.sink<NotifyHealthChangeBroadcast>().connect<&BlockService::OnHealthChangeBroadcast>(this);
    m_clashConnection = aDispatcher.sink<NotifyClash>().connect<&BlockService::OnNotifyClash>(this);
    EventDispatcherManager::Get()->hitEvent.RegisterSink(&s_hitSink);
#else
    (void)aDispatcher;
#endif
}

void BlockService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected() || !Active())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const uint64_t cNow = m_transport.GetClock().GetCurrentTick();
    const bool cBlocking = IsBlocking(pPlayer);
    if (cBlocking != m_wasBlocking)
    {
        spdlog::debug("Flat block: IsBlocking now {}", cBlocking);
        m_wasBlocking = cBlocking;
    }
    m_history.Note(cNow, cBlocking);

    if (!m_blows.empty())
        Decide(cNow, cBlocking);
}

void BlockService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_history.Clear();
    m_blows.clear();
    m_wasBlocking = false;
}

void BlockService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) noexcept
{
    // Another player's weapon blow on this player: the defender's rule holds it (ActorValueService), and the block is weighed here.
    // A spell's hit has no tick: a block does not stop it. Nor is the part of a blocked blow given back here, which has none.
    if (acMessage.DeltaHealth >= 0.f || !acMessage.AttackerPlayerId || !acMessage.Tick || !Active())
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor || pActor != PlayerCharacter::Get())
        return;

    // An arrow is replayed here, and that copy's hit decides; the game's own block already counted on it.
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (const auto cEntity : view)
        if (view.get<PlayerComponent>(cEntity).Id == acMessage.AttackerPlayerId)
        {
            const uint32_t cCopy = view.get<FormIdComponent>(cEntity).Id;
            const Actor* pCopy = Cast<Actor>(TESForm::GetById(cCopy));
            if (pCopy && PvpProjectiles::Recent(cCopy, glm::distance(glm::vec3(pCopy->position), glm::vec3(pActor->position))))
                return;
            break;
        }

    HeldBlow blow{};
    blow.Id = acMessage.Id;
    blow.AttackerPlayerId = acMessage.AttackerPlayerId;
    blow.Tick = acMessage.Tick;
    blow.Damage = -acMessage.DeltaHealth;
    m_blows.push_back(blow);
}

void BlockService::Decide(const uint64_t aNow, const bool aBlocking) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    for (auto it = m_blows.begin(); it != m_blows.end();)
    {
        // The one who struck, and how far behind them their copy is shown here right now (as the defender's rule reckons it).
        uint32_t copy = 0;
        uint64_t playbackTick = 0;
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (const auto cEntity : view)
            if (view.get<PlayerComponent>(cEntity).Id == it->AttackerPlayerId)
            {
                copy = view.get<FormIdComponent>(cEntity).Id;
                if (const auto* pInterpolation = m_world.try_get<InterpolationComponent>(cEntity))
                    playbackTick = InterpolationSystem::GetPlaybackTick(*pInterpolation);
                break;
            }

        const uint64_t cSeen = it->Tick + PvpBlock::SeenAfter(aNow, playbackTick);
        // A tick from the future (a clock not yet in step) is not waited for, as in the defender's rule.
        Actor* pCopy = copy ? Cast<Actor>(TESForm::GetById(copy)) : nullptr;
        if (!pCopy || it->Tick >= aNow + 5000)
        {
            it = m_blows.erase(it);
            continue;
        }

        // The game weighs the block while it is held as the blow comes: its numbers are taken at the first such frame.
        if (aBlocking && !it->Game && aNow + PvpBlock::kBlockBefore >= cSeen)
            it->Game = GameBlowOf(pCopy, pPlayer);

        const PvpBlock::Verdict cVerdict = PvpBlock::Decide(m_history, cSeen, aNow);
        if (cVerdict == PvpBlock::Verdict::kWait)
        {
            ++it;
            continue;
        }

        if (cVerdict == PvpBlock::Verdict::kBlocked)
        {
            if (it->Game && (it->Game->Flags & kHitBlocked) && it->Game->PercentBlocked > 0.f)
                Blocked(*it, pPlayer, pCopy, cSeen);
            else if (it->Game)
                spdlog::debug(
                    "Flat block: a blow of {:.0f} from player {} came while blocking, but the game does not block it (flags {:X}, {:.2f} blocked)", it->Damage,
                    it->AttackerPlayerId, it->Game->Flags, it->Game->PercentBlocked);
            else
                spdlog::debug("Flat block: a blow of {:.0f} from player {} was told after the block went down; it lands", it->Damage, it->AttackerPlayerId);
        }
        it = m_blows.erase(it);
    }
}

void BlockService::Blocked(const HeldBlow& acBlow, Actor* apPlayer, Actor* apCopy, const uint64_t aSeen) noexcept
{
    const GameBlow& cGame = *acBlow.Game;
    const float cTaken = acBlow.Damage * (1.f - std::clamp(cGame.PercentBlocked, 0.f, 1.f));

    // The whole blow is weighed as a blade contact, so the defender's rule drops it (and none seen later, PvpBlock::ClashTick);
    // what the block lets through is taken here, as a blow with no tick, which goes straight on.
    m_world.GetDispatcher().trigger(ClashEvent{apCopy->formID, PvpBlock::ClashTick(aSeen)});
    if (cTaken > 0.f)
    {
        NotifyHealthChangeBroadcast through{};
        through.Id = acBlow.Id;
        through.DeltaHealth = -cTaken;
        through.AttackerPlayerId = acBlow.AttackerPlayerId;
        through.Tick = 0;
        m_world.GetDispatcher().trigger(through);
    }

    // Their flat swing, played here, already ran the game's own hit on this player: its block, sound and stagger showed then.
    const bool cShown = GameShowedHit(apCopy->formID);
    const bool cStaggered = !cShown && cGame.Stagger > 0.f && PlayStagger(apPlayer, apCopy, cGame.Stagger);
    spdlog::debug(
        "Flat block: a blow of {:.0f} from player {} blocked, {:.2f} of it by the game's block (stagger {:.2f}); {:.1f} taken{}{}", acBlow.Damage, acBlow.AttackerPlayerId,
        cGame.PercentBlocked, cGame.Stagger, cTaken, cStaggered ? ", staggered" : "", cShown ? "; the game's own hit showed it here" : "");

    SendBlocked(apPlayer, apCopy, aSeen, !cShown);
}

void BlockService::SendBlocked(const Actor* apPlayer, const Actor* apCopy, const uint64_t aSeen, const bool aHeardHere) noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = Utils::FindByFormId(view, apCopy->formID);
    if (!it)
        return;

    const glm::vec3 cPoint = BlockPoint(apPlayer, apCopy);

    // The one who struck hears it and feels it in the hand holding the weapon (CharacterService::OnNotifyClash);
    // the server sends it to the others near it, not back here.
    ClashRequest request{};
    request.OtherId = view.get<RemoteComponent>(*it).Id;
    request.OtherSide = WeaponSide(apCopy);
    request.OwnSide = 2;
    request.Point = cPoint;
    request.Speed = 0.f;
    request.Tick = aSeen;
    m_transport.Send(request);
    if (aHeardHere)
        VRBodySync::PlayClashSound(cPoint);
}

void BlockService::OnNotifyClash(const NotifyClash& acMessage) const noexcept
{
    if (!ClientTrust::IsInsideWorld({acMessage.Point.x, acMessage.Point.y, acMessage.Point.z}))
        return;
    // Another player's weapon met a weapon near here: the game's blade-block sound, where it happened.
    // The sparks and the hand's pulse are VR's (CharacterService::OnNotifyClash).
    if (!Active())
        return;
    const glm::vec3 point(acMessage.Point);
    VRBodySync::PlayClashSound(point);
}
