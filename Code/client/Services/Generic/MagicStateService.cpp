#include <Services/GameExtension.h>
#include <Services/MagicStateService.h>
#include <Services/MagicService.h>
#include <Services/TransportService.h>

#include <MagicStates.h>
#include <Games/Magic/SharedSelfEffects.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/SpellCastEvent.h>
#include <Events/InterruptCastEvent.h>

#include <Messages/NotifyInterruptCast.h>
#include <Messages/RequestMagicState.h>
#include <Messages/NotifyMagicState.h>

#include <World.h>
#include <Utils.h>
#include <Helpers/PerfScope.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <Games/TES.h>
#include <Games/ActorExtension.h>
#include <Effects/ActiveEffect.h>
#include <Effects/ArtObjects.h>
#include <Magic/ActorMagicCaster.h>
#include <Magic/EffectItem.h>
#include <Forms/EffectSetting.h>
#include <Forms/SpellItem.h>

namespace
{
using Clock = std::chrono::steady_clock;

// A cast other than a held spell (a shout, Invisibility) that came before its caster's copy is cast when the copy comes this soon.
constexpr auto kEarlyCastKept = std::chrono::milliseconds(1500);
// A player not seen here this long is forgotten; the server sends their state again with their character.
constexpr auto kCopyForgotten = std::chrono::seconds(10);

// The nodes a hand's casting art is played on, by casting source.
constexpr const char* kMagicNodes[2] = {"NPC L MagicNode [LMag]", "NPC R MagicNode [RMag]"};

bool HandIndex(const int32_t aSource, size_t& aIndex) noexcept
{
    if (aSource == MagicSystem::LEFT_HAND || aSource == MagicSystem::RIGHT_HAND)
    {
        aIndex = static_cast<size_t>(aSource);
        return true;
    }
    return false;
}

EffectItem* ItemOf(const ActiveEffect* apEffect) noexcept
{
    return static_cast<EffectItem*>(apEffect->pEffect);
}

// What the share rule reads about one effect on aBody, caster or not.
SharedSelfEffects::Facts FactsOf(const ActiveEffect* apEffect, const Actor* apBody) noexcept
{
    SharedSelfEffects::Facts facts{};
    const EffectItem* pItem = ItemOf(apEffect);
    const EffectSetting* pSetting = pItem ? pItem->pEffectSetting : nullptr;
    if (!pSetting || !apEffect->pSpell)
        return facts;

    facts.FromSelf = TESObjectREFR::GetByHandle(apEffect->hCaster) == apBody;
    facts.Elapsed = apEffect->fElapsedSeconds;
    facts.Duration = apEffect->fDuration;
    facts.Flags = apEffect->uiFlags;
    facts.Archetype = pSetting->eArchetype;
    facts.Visual = SharedSelfEffects::HasVisual(pSetting->pEffectShader != nullptr, pSetting->pHitEffectArt != nullptr, pSetting->pLight != nullptr, pSetting->eArchetype);
    facts.Personal = MagicStateService::IsPersonalBlessing(apEffect->pSpell);
    facts.LocalControl = MagicService::IsLocalControlForm(apEffect->pSpell->formID) || MagicService::IsLocalControlForm(pSetting->formID);
    return facts;
}

// The copy of another player, by their character's server id, and whether its body is there to show a cast or an effect.
Actor* FindCopy(World& aWorld, const uint32_t aServerId, bool& aHasBody) noexcept
{
    aHasBody = false;
    auto view = aWorld.view<RemoteComponent, FormIdComponent>();
    for (const auto cEntity : view)
    {
        if (view.get<RemoteComponent>(cEntity).Id != aServerId)
            continue;
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
        aHasBody = pActor && pActor->GetNiNode();
        return pActor;
    }
    return nullptr;
}

// The art the game plays on a hand charging the spell: the casting art of its first effect that has one.
BGSArtObject* ChargeArtOf(const MagicItem* apSpell) noexcept
{
    for (uint32_t i = 0; i < apSpell->listOfEffects.length; ++i)
    {
        const EffectItem* pItem = apSpell->listOfEffects[i];
        if (pItem && pItem->pEffectSetting && pItem->pEffectSetting->pCastingArt)
            return static_cast<BGSArtObject*>(pItem->pEffectSetting->pCastingArt);
    }
    return nullptr;
}

NiAVObject* HandNode(Actor* apActor, const size_t aIndex) noexcept
{
    using TGetObjectByName = void*(void*, const char* const*, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 76207, 76207);
    NiNode* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return nullptr;
    const BSFixedString cName(kMagicNodes[aIndex]);
    return static_cast<NiAVObject*>(GetObjectByName(pRoot, &cName.data, 1));
}

uint32_t PluginFormId(const SharedSelfEffects::PluginForm& acForm) noexcept
{
    ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(std::string(acForm.Plugin).c_str()) : nullptr;
    if (!pMod || !pMod->IsLoaded())
        return 0;
    if (pMod->IsLite())
        return 0xFE000000u | (static_cast<uint32_t>(pMod->liteId) << 12) | (acForm.BaseId & 0xFFF);
    return (static_cast<uint32_t>(pMod->standardId) << 24) | (acForm.BaseId & 0xFFFFFF);
}
} // namespace

MagicStateService::MagicStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MagicStateService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&MagicStateService::OnDisconnected>(this);
    m_spellCastEventConnection = aDispatcher.sink<SpellCastEvent>().connect<&MagicStateService::OnSpellCastEvent>(this);
    m_interruptCastEventConnection = aDispatcher.sink<InterruptCastEvent>().connect<&MagicStateService::OnInterruptCastEvent>(this);
    m_notifySpellCastConnection = aDispatcher.sink<NotifySpellCast>().connect<&MagicStateService::OnNotifySpellCast>(this);
    m_notifyInterruptCastConnection = aDispatcher.sink<NotifyInterruptCast>().connect<&MagicStateService::OnNotifyInterruptCast>(this);
    m_notifyMagicStateConnection = aDispatcher.sink<NotifyMagicState>().connect<&MagicStateService::OnNotifyMagicState>(this);
}

bool MagicStateService::IsPersonalBlessing(const MagicItem* apSpell) noexcept
{
    if (!apSpell)
        return false;

    struct Resolved
    {
        TiltedPhoques::Vector<uint32_t> Stones;
        uint32_t ShrineCure = 0;
        bool Done = false;
    };
    static Resolved s_forms;

    // The plugin list is complete once the game data is loaded, which is before any spell can be cast.
    if (!s_forms.Done)
    {
        ModManager* pManager = ModManager::Get();
        if (!pManager || !pManager->GetByName("Skyrim.esm"))
            return false;
        for (const auto& cForm : SharedSelfEffects::kStandingStoneSpells)
            if (const uint32_t cId = PluginFormId(cForm))
                s_forms.Stones.push_back(cId);
        s_forms.ShrineCure = PluginFormId(SharedSelfEffects::kShrineCureEffect);
        s_forms.Done = true;
    }

    if (std::find(s_forms.Stones.begin(), s_forms.Stones.end(), apSpell->formID) != s_forms.Stones.end())
        return true;

    if (s_forms.ShrineCure == 0)
        return false;
    for (uint32_t i = 0; i < apSpell->listOfEffects.length; ++i)
    {
        const EffectItem* pItem = apSpell->listOfEffects[i];
        if (pItem && pItem->pEffectSetting && pItem->pEffectSetting->formID == s_forms.ShrineCure)
            return true;
    }
    return false;
}

void MagicStateService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("MagicStateService::OnUpdate");

    if (!m_transport.IsConnected())
        return;

    // A charge is said as soon as it starts or ends, so the copy's hand glows while its owner's does.
    GameId charges[2];
    ReadOwnCharges(charges);
    const bool cChargeChanged = charges[0] != m_ownCharges[0] || charges[1] != m_ownCharges[1];
    m_ownCharges[0] = charges[0];
    m_ownCharges[1] = charges[1];

    const auto cNow = Clock::now();
    if (!cChargeChanged && cNow - m_readAt < std::chrono::milliseconds(MagicStates::kReadEveryMs))
    {
        RenewDueGlows(cNow);
        return;
    }
    m_readAt = cNow;

    SendOwnState();
    UpdateCopies();
}

void MagicStateService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    for (Hand& hand : m_ownCasts)
        hand = {};
    for (GameId& charge : m_ownCharges)
        charge = {};
    m_sent = {};
    m_sentBefore = false;
    ClearCopies();
}

void MagicStateService::OnSpellCastEvent(const SpellCastEvent& acEvent) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !acEvent.pCaster || acEvent.pCaster->pCasterActor != pPlayer)
        return;

    size_t index = 0;
    if (!HandIndex(acEvent.pCaster->GetCastingSource(), index))
        return;

    // Only a held spell is still going after its cast; as MagicService sends it.
    const SpellItem* pSpell = Cast<SpellItem>(TESForm::GetById(acEvent.SpellId));
    if (!pSpell || pSpell->eCastingType != MagicSystem::CONCENTRATION || pSpell->HasSummonEffect() || MagicService::IsLocalControlForm(pSpell->formID) ||
        !MagicSystem::IsSentAsCast(pSpell->eCastingType, pSpell->IsWardSpell(), pSpell->IsInvisibilitySpell()))
        return;

    Hand hand{};
    if (!m_world.GetModSystem().GetSharedSpellId(pSpell->formID, hand.Spell))
    {
        if (GameExtension::ChecksMagic())
            m_ownCasts[index] = {};
        return;
    }
    hand.IsDualCasting = acEvent.pCaster->GetIsDualCasting();
    m_ownCasts[index] = hand;
}

void MagicStateService::OnInterruptCastEvent(const InterruptCastEvent& acEvent) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    size_t index = 0;
    if (pPlayer && acEvent.CasterFormID == pPlayer->formID && HandIndex(acEvent.CastingSource, index))
        m_ownCasts[index] = {};
}

void MagicStateService::ReadOwnCharges(GameId (&aCharges)[2]) const noexcept
{
    aCharges[MagicSystem::LEFT_HAND] = {};
    aCharges[MagicSystem::RIGHT_HAND] = {};

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    const bool cDead = pPlayer->IsDead();

    for (const int32_t cSource : {MagicSystem::LEFT_HAND, MagicSystem::RIGHT_HAND})
    {
        const ActorMagicCaster* pCaster = pPlayer->casters[cSource];
        const SpellItem* pSpell = pCaster ? Cast<SpellItem>(pCaster->pCurrentSpell) : nullptr;
        if (!pSpell || MagicSystem::IsVoiceCast(pSpell->eSpellType) || MagicService::IsLocalControlForm(pSpell->formID))
            continue;

        const auto cState = static_cast<uint32_t>(pCaster->eState);
        const bool cSentAsCast = MagicSystem::IsSentAsCast(pSpell->eCastingType, pSpell->IsWardSpell(), pSpell->IsInvisibilitySpell());
        if (!MagicStates::IsCharging(cState, pSpell->eCastingType == MagicSystem::FIRE_AND_FORGET, cSentAsCast, cDead))
            continue;
        if (!m_world.GetModSystem().GetSharedSpellId(pSpell->formID, aCharges[cSource]))
            aCharges[cSource] = {};
    }
}

void MagicStateService::SendOwnState() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto cEntity = Utils::FindByFormId(view, pPlayer->formID);
    if (!cEntity)
        return;

    if (pPlayer->IsDead())
        for (Hand& hand : m_ownCasts)
            hand = {};

    MagicState state;
    state.LeftHand = {m_ownCasts[MagicSystem::LEFT_HAND].Spell, m_ownCasts[MagicSystem::LEFT_HAND].IsDualCasting};
    state.RightHand = {m_ownCasts[MagicSystem::RIGHT_HAND].Spell, m_ownCasts[MagicSystem::RIGHT_HAND].IsDualCasting};
    state.LeftHand.ChargeId = m_ownCharges[MagicSystem::LEFT_HAND];
    state.RightHand.ChargeId = m_ownCharges[MagicSystem::RIGHT_HAND];

    {
        for (ActiveEffect* pEffect : pPlayer->magicTarget.GetActiveEffects())
        {
            if (state.Effects.size() >= MagicState::kMaxEffects)
                break;
            if (!SharedSelfEffects::IsShared(FactsOf(pEffect, pPlayer)))
                continue;

            MagicStateEffect effect{};
            if (!m_world.GetModSystem().GetSharedSpellId(pEffect->pSpell->formID, effect.SpellId) ||
                !m_world.GetModSystem().GetSharedSpellId(ItemOf(pEffect)->pEffectSetting->formID, effect.EffectId))
                continue;
            effect.Magnitude = pEffect->fMagnitude;
            effect.Elapsed = pEffect->fElapsedSeconds;
            effect.Duration = pEffect->fDuration;
            state.Effects.push_back(effect);
        }
    }

    const auto cNow = Clock::now();
    const auto cSinceSent = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - m_sentAt).count());
    if (!MagicStates::ShouldSend(m_sentBefore, MagicStates::SameShape(state, m_sent), state.Empty(), cSinceSent))
        return;

    RequestMagicState request;
    request.CharacterId = view.get<LocalComponent>(*cEntity).Id;
    request.State = state;
    m_transport.Send(request);

    spdlog::debug(
        "MagicState: sent {} effects, casting {:X} and {:X}, charging {:X} and {:X}", state.Effects.size(), state.LeftHand.SpellId.LogFormat(), state.RightHand.SpellId.LogFormat(),
        state.LeftHand.ChargeId.LogFormat(), state.RightHand.ChargeId.LogFormat());

    m_sent = std::move(state);
    m_sentBefore = true;
    m_sentAt = cNow;
}

void MagicStateService::OnNotifySpellCast(const NotifySpellCast& acMessage) noexcept
{
    if (m_replaying)
        return;

    auto& copy = m_copies[acMessage.CasterId];
    bool hasBody = false;
    const Actor* pActor = FindCopy(m_world, acMessage.CasterId, hasBody);

    if (GameExtension::RejectMagic(acMessage.SpellFormId))
        return;
    const SpellItem* pSpell = Cast<SpellItem>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.SpellFormId)));
    size_t index = 0;
    if (pSpell && pSpell->eCastingType == MagicSystem::CONCENTRATION && HandIndex(acMessage.CastingSource, index))
    {
        copy.Wanted[index] = {acMessage.SpellFormId, acMessage.IsDualCasting};
        // MagicService casts it now on a copy with a body; otherwise it is cast here once the body is there.
        if (hasBody)
            copy.CastingHere[index] = acMessage.SpellFormId;
        else
            copy.Pending = true;
        return;
    }

    // MagicService casts it on any copy it finds; one not made yet gets it when it comes, if soon.
    if (!pActor)
        copy.Early.push_back({Clock::now(), acMessage});
}

void MagicStateService::OnNotifyInterruptCast(const NotifyInterruptCast& acMessage) noexcept
{
    if (m_replaying)
        return;

    size_t index = 0;
    if (!HandIndex(acMessage.CastingSource, index))
        return;
    auto it = m_copies.find(acMessage.CasterId);
    if (it == m_copies.end())
        return;
    it.value().Wanted[index] = {};
    it.value().CastingHere[index] = {};
}

void MagicStateService::OnNotifyMagicState(const NotifyMagicState& acMessage) noexcept
{
    auto& copy = m_copies[acMessage.CharacterId];
    copy.State = MagicStates::Sanitized(acMessage.State);
    if (GameExtension::RejectMagic(copy.State.LeftHand.SpellId))
        copy.State.LeftHand = {};
    if (GameExtension::RejectMagic(copy.State.RightHand.SpellId))
        copy.State.RightHand = {};
    for (MagicStateCast* pHand : {&copy.State.LeftHand, &copy.State.RightHand})
        if (GameExtension::RejectMagic(pHand->ChargeId))
            pHand->ChargeId = {};
    std::erase_if(copy.State.Effects, [](const auto& e) { return GameExtension::RejectMagic(e.SpellId) || GameExtension::RejectMagic(e.EffectId); });
    copy.StateAt = Clock::now();
    copy.EndMissing = true;
    copy.Pending = true;
    copy.Wanted[MagicSystem::LEFT_HAND] = {copy.State.LeftHand.SpellId, copy.State.LeftHand.IsDualCasting};
    copy.Wanted[MagicSystem::RIGHT_HAND] = {copy.State.RightHand.SpellId, copy.State.RightHand.IsDualCasting};

    spdlog::debug(
        "MagicState: player {:X} has {} effects, casting {:X} and {:X}", acMessage.CharacterId, copy.State.Effects.size(), copy.State.LeftHand.SpellId.LogFormat(),
        copy.State.RightHand.SpellId.LogFormat());

    ApplyCharges(acMessage.CharacterId, copy);
}

void MagicStateService::UpdateCopies() noexcept
{
    const auto cNow = Clock::now();
    TiltedPhoques::Vector<uint32_t> forgotten;
    // Handlers may add to m_copies, so the casts wait until the loop is done.
    TiltedPhoques::Vector<NotifySpellCast> replay;

    for (auto it = m_copies.begin(); it != m_copies.end(); ++it)
    {
        const uint32_t cServerId = it->first;
        Copy& copy = it.value();

        bool hasBody = false;
        Actor* pActor = FindCopy(m_world, cServerId, hasBody);

        std::erase_if(copy.Early, [cNow](const auto& acEarly) { return cNow - acEarly.first > kEarlyCastKept; });

        if (!pActor)
        {
            StopGlows(copy);
            if (copy.LastSeen == Clock::time_point{})
                copy.LastSeen = cNow;
            if (cNow - copy.LastSeen > kCopyForgotten && copy.Early.empty())
                forgotten.push_back(cServerId);
            continue;
        }
        copy.LastSeen = cNow;

        if (!copy.Early.empty())
        {
            auto early = std::move(copy.Early);
            copy.Early.clear();
            for (const auto& [cAt, cCast] : early)
            {
                spdlog::debug("MagicState: a cast for player {:X} that came before their copy is cast now", cServerId);
                replay.push_back(cCast);
            }
        }

        ApplyCharges(cServerId, copy);

        if (!copy.Pending || !hasBody)
            continue;

        ApplyCasts(cServerId, copy);
        ApplyEffects(cServerId, copy);
        copy.Pending = false;
        copy.EndMissing = false;
    }

    for (const uint32_t cServerId : forgotten)
        m_copies.erase(cServerId);

    for (const NotifySpellCast& cCast : replay)
        m_dispatcher.trigger(cCast);
}

void MagicStateService::ApplyCasts(const uint32_t aServerId, Copy& aCopy) noexcept
{
    for (const int32_t cSource : {MagicSystem::LEFT_HAND, MagicSystem::RIGHT_HAND})
    {
        const auto cIndex = static_cast<size_t>(cSource);
        const Hand& cWanted = aCopy.Wanted[cIndex];
        const MagicStates::CastStep cStep = MagicStates::PlanCast(aCopy.CastingHere[cIndex].LogFormat(), cWanted.Spell.LogFormat());

        m_replaying = true;
        if (cStep.Interrupt)
        {
            NotifyInterruptCast interrupt;
            interrupt.CasterId = aServerId;
            interrupt.CastingSource = cSource;
            m_dispatcher.trigger(interrupt);
        }
        if (cStep.Cast)
        {
            NotifySpellCast cast;
            cast.CasterId = aServerId;
            cast.SpellFormId = cWanted.Spell;
            cast.CastingSource = cSource;
            cast.IsDualCasting = cWanted.IsDualCasting;
            cast.DesiredTarget = 0;
            m_dispatcher.trigger(cast);
        }
        m_replaying = false;

        if (cStep.Interrupt || cStep.Cast)
            spdlog::debug("MagicState: copy of player {:X}, hand {}: {} to match its owner", aServerId, cSource, cStep.Cast ? "cast" : "interrupted");
        aCopy.CastingHere[cIndex] = cWanted.Spell;
    }
}

void MagicStateService::ApplyEffects(const uint32_t aServerId, Copy& aCopy) noexcept
{
    bool hasBody = false;
    Actor* pCopy = FindCopy(m_world, aServerId, hasBody);
    if (!pCopy)
        return;
    const TiltedPhoques::Vector<ActiveEffect*> cEffects = pCopy->magicTarget.GetActiveEffects();

    const float cAge = std::chrono::duration<float>(Clock::now() - aCopy.StateAt).count();
    const MagicState cState = MagicStates::Aged(aCopy.State, cAge);

    TiltedPhoques::Vector<MagicStates::Key> wanted;
    TiltedPhoques::Vector<float> left;
    for (const auto& cEffect : cState.Effects)
    {
        wanted.push_back({m_world.GetModSystem().GetGameId(cEffect.SpellId), m_world.GetModSystem().GetGameId(cEffect.EffectId)});
        left.push_back(wanted.back().Spell && wanted.back().Effect ? cEffect.Duration - cEffect.Elapsed : 0.f);
    }

    TiltedPhoques::Vector<MagicStates::OnCopy> onCopy;
    TiltedPhoques::Vector<ActiveEffect*> copyEffects;
    for (ActiveEffect* pEffect : cEffects)
    {
        const EffectItem* pItem = ItemOf(pEffect);
        if (!pItem || !pItem->pEffectSetting || !pEffect->pSpell)
            continue;
        onCopy.push_back({{pEffect->pSpell->formID, pItem->pEffectSetting->formID}, pEffect->fElapsedSeconds, SharedSelfEffects::IsShared(FactsOf(pEffect, pCopy))});
        copyEffects.push_back(pEffect);
    }

    const MagicStates::Plan cPlan = MagicStates::PlanEffects(wanted, left, onCopy, aCopy.EndMissing);

    // Ended as it would run out: it wears off at the copy's next update.
    for (const size_t cIndex : cPlan.End)
    {
        copyEffects[cIndex]->fElapsedSeconds = copyEffects[cIndex]->fDuration;
        spdlog::debug(
            "MagicState: copy of player {:X}: effect {:X} of spell {:X} ended, its owner no longer has it", aServerId, onCopy[cIndex].Which.Effect, onCopy[cIndex].Which.Spell);
    }

    for (const size_t cIndex : cPlan.Add)
    {
        const MagicStateEffect& cEffect = cState.Effects[cIndex];
        MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(wanted[cIndex].Spell));
        if (!pSpell || IsPersonalBlessing(pSpell) || MagicService::IsLocalControlForm(pSpell->formID))
            continue;
        EffectItem* pItem = pSpell->GetEffect(wanted[cIndex].Effect);
        if (!pItem || !pItem->pEffectSetting)
            continue;

        // The owner's game sends only effects that are for show (IsShared); a state naming a summon or a hazard is not put on the copy, where it would run for real.
        const EffectSetting* pSetting = pItem->pEffectSetting;
        if (!SharedSelfEffects::IsShownArchetype(pSetting->eArchetype) ||
            !SharedSelfEffects::HasVisual(pSetting->pEffectShader != nullptr, pSetting->pHitEffectArt != nullptr, pSetting->pLight != nullptr, pSetting->eArchetype))
            continue;

        MagicTarget::AddTargetData data{};
        data.pCaster = pCopy;
        data.pSpell = pSpell;
        data.pEffectItem = pItem;
        data.fMagnitude = cEffect.Magnitude;
        data.fUnkFloat1 = 1.0f;
        data.eCastingSource = MagicSystem::CastingSource::CASTING_SOURCE_COUNT;
        if (!pCopy->magicTarget.AddTarget(data, 1.f, false))
            continue;

        // It runs for the time its owner's has left.
        for (ActiveEffect* pEffect : pCopy->magicTarget.GetActiveEffects())
            if (pEffect->pSpell == pSpell && ItemOf(pEffect) == pItem)
            {
                pEffect->fDuration = cEffect.Duration;
                pEffect->fElapsedSeconds = cEffect.Elapsed;
            }

        spdlog::debug(
            "MagicState: copy of player {:X}: effect {:X} of spell {:X} put on with {:.1f} s left", aServerId, wanted[cIndex].Effect, wanted[cIndex].Spell,
            cEffect.Duration - cEffect.Elapsed);
    }
}

void MagicStateService::ApplyCharges(const uint32_t aServerId, Copy& aCopy) noexcept
{
    bool hasBody = false;
    Actor* pCopy = FindCopy(m_world, aServerId, hasBody);
    const void* pBody = hasBody ? pCopy->GetNiNode() : nullptr;
    const bool cBodyChanged = pBody != aCopy.pGlowBody;
    aCopy.pGlowBody = pBody;

    const auto cNow = Clock::now();
    const GameId cWanted[2] = {aCopy.State.LeftHand.ChargeId, aCopy.State.RightHand.ChargeId};
    for (size_t i = 0; i < 2; ++i)
    {
        const GameId cCharge = pBody ? cWanted[i] : GameId{};
        const MagicStates::ChargeStep cStep = MagicStates::PlanCharge(aCopy.GlowingHere[i].LogFormat(), cCharge.LogFormat(), cBodyChanged);
        const auto cMsLeft = std::chrono::duration_cast<std::chrono::milliseconds>(aCopy.GlowUntil[i] - cNow).count();
        const bool cRenew = !cStep.Play && MagicStates::RenewsGlow(cCharge.LogFormat(), cMsLeft < 0 ? 0 : static_cast<uint64_t>(cMsLeft));
        aCopy.GlowingHere[i] = cCharge;
        // An ended charge's last glow runs out by itself, within kGlowSeconds.
        if (!cStep.Play && !cRenew)
            continue;

        // A spell with no casting art has nothing to show; one whose hand is not there yet is tried again next pass.
        const MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(m_world.GetModSystem().GetGameId(cCharge)));
        BGSArtObject* pArt = pSpell ? ChargeArtOf(pSpell) : nullptr;
        if (!pArt)
            continue;
        NiAVObject* pNode = HandNode(pCopy, i);
        if (!pNode || !ArtObjects::Play(pCopy, pArt, pNode, MagicStates::kGlowSeconds))
        {
            aCopy.GlowingHere[i] = {};
            continue;
        }
        aCopy.GlowUntil[i] = cNow + std::chrono::milliseconds(static_cast<int64_t>(MagicStates::kGlowSeconds * 1000.f));
        if (cStep.Play)
            spdlog::debug("MagicState: copy of player {:X}, hand {}: glows charging spell {:X}", aServerId, i, cCharge.LogFormat());
    }
}

void MagicStateService::RenewDueGlows(const std::chrono::steady_clock::time_point aNow) noexcept
{
    // Between two passes of the copies: only a glow about to run out is played again, just before it does.
    for (auto it = m_copies.begin(); it != m_copies.end(); ++it)
    {
        Copy& copy = it.value();
        bool due = false;
        for (size_t i = 0; i < 2 && !due; ++i)
        {
            const auto cMsLeft = std::chrono::duration_cast<std::chrono::milliseconds>(copy.GlowUntil[i] - aNow).count();
            due = MagicStates::RenewsGlow(copy.GlowingHere[i].LogFormat(), cMsLeft < 0 ? 0 : static_cast<uint64_t>(cMsLeft));
        }
        if (due)
            ApplyCharges(it->first, copy);
    }
}

void MagicStateService::StopGlows(Copy& aCopy) noexcept
{
    for (size_t i = 0; i < 2; ++i)
    {
        aCopy.GlowingHere[i] = {};
        aCopy.GlowUntil[i] = {};
    }
    aCopy.pGlowBody = nullptr;
}

void MagicStateService::ClearCopies() noexcept
{
    for (auto it = m_copies.begin(); it != m_copies.end(); ++it)
        StopGlows(it.value());
    m_copies.clear();
}
