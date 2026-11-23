#include <TiltedOnlinePCH.h>

#include <Services/VRShoutService.h>
#include <Services/MagicService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/SpellCastEvent.h>
#include <Events/UpdateEvent.h>
#include <Structs/ActionEvent.h>

#include <FollowerCommands.h>
#include <Forms/SpellItem.h>
#include <Magic/ActorMagicCaster.h>
#include <PlayerCharacter.h>
#include <World.h>

#include <algorithm>

VRShoutService::VRShoutService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&VRShoutService::OnUpdate>(this);
    m_actionConnection = aDispatcher.sink<ActionEvent>().connect<&VRShoutService::OnActionEvent>(this);
    m_spellCastConnection = aDispatcher.sink<SpellCastEvent>().connect<&VRShoutService::OnSpellCastEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&VRShoutService::OnDisconnected>(this);
}

void VRShoutService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingTick = 0;
    m_releaseNext = false;
    m_ownTick = 0;
    m_sent.clear();
}

void VRShoutService::OnActionEvent(const ActionEvent& acEvent) noexcept
{
    const auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acEvent.ActorId != pPlayer->formID || !VRShout::IsVoiceAction(acEvent.ActionId))
        return;
    const auto cOwn = std::find(m_sent.begin(), m_sent.end(), std::make_pair(acEvent.ActionId, acEvent.Tick));
    if (cOwn != m_sent.end())
    {
        m_sent.erase(cOwn);
        return;
    }
    // The game performed a voice action for this player itself: its shout is shown already.
    m_ownTick = acEvent.Tick;
}

void VRShoutService::OnSpellCastEvent(const SpellCastEvent& acEvent) noexcept
{
    if (!m_world.GetTransport().IsOnline())
        return;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !acEvent.pCaster || acEvent.pCaster->pCasterActor != pPlayer)
        return;
    if (FollowerCommands::IsMenuSpell(acEvent.SpellId) || MagicService::IsLocalControlForm(acEvent.SpellId))
        return;
    const SpellItem* pSpell = Cast<SpellItem>(TESForm::GetById(acEvent.SpellId));
    if (!pSpell || !VRShout::IsShout(static_cast<int32_t>(pSpell->eSpellType)))
        return;

    const uint64_t cNow = m_world.GetTick();
    if (!VRShout::IsNewShout(cNow, m_shownTick))
        return;
    m_pendingTick = cNow;
    spdlog::debug("VRShout: shout {:X} cast, shown on the copies unless the game performs its voice action", acEvent.SpellId);
}

void VRShoutService::Send(const uint32_t aActionId, const std::string_view acEventName, const uint64_t aTick) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // As the game's own action would be (VRSwingService::Send).
    ActionEvent action;
    action.State1 = pPlayer->actorState.flags1;
    action.State2 = pPlayer->actorState.flags2;
    action.Type = 0;
    action.Tick = aTick;
    action.ActorId = pPlayer->formID;
    action.ActionId = aActionId;
    action.TargetId = 0;
    action.EventName = TiltedPhoques::String(acEventName.data(), acEventName.size());
    pPlayer->SaveAnimationVariables(action.Variables);

    if (m_sent.size() > 8)
        m_sent.erase(m_sent.begin());
    m_sent.emplace_back(aActionId, aTick);
    m_world.GetRunner().Trigger(action);
}

void VRShoutService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_world.GetTransport().IsOnline() || !PlayerCharacter::Get())
    {
        m_pendingTick = 0;
        m_releaseNext = false;
        return;
    }

    const uint64_t cNow = m_world.GetTick();

    if (m_releaseNext)
    {
        m_releaseNext = false;
        Send(VRShout::kActionVoiceRelease, VRShout::kReleaseEvent, std::max(cNow, m_releaseTick + 1));
    }

    if (m_pendingTick == 0)
        return;

    switch (VRShout::Decide(m_pendingTick, m_ownTick, cNow))
    {
    case VRShout::Decision::Wait: return;
    case VRShout::Decision::Shown: spdlog::debug("VRShout: shout already shown by the game's own voice action"); break;
    case VRShout::Decision::Send:
        Send(VRShout::kActionVoice, VRShout::kStartEvent, m_pendingTick);
        m_releaseTick = m_pendingTick;
        m_releaseNext = true;
        m_shownTick = m_pendingTick;
        spdlog::debug("VRShout: shout sent as voice actions");
        break;
    }
    m_pendingTick = 0;
}
