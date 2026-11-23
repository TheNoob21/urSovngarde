#include <Services/ActivationCatchUpService.h>

#include <World.h>

#include <Events/ActivateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyActivationCatchUp.h>
#include <Messages/RequestActivationState.h>

#include <Services/TransportService.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Games/TES.h>
#include <Interface/UI.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>
#include <NetImmerse/NiTree.h>

namespace
{
bool IsInGame() noexcept
{
    static const BSFixedString s_loadingMenu("Loading Menu");
    static const BSFixedString s_mainMenu("Main Menu");
    const UI* pUI = UI::Get();
    return pUI && PlayerCharacter::Get() && !pUI->GetMenuOpen(s_loadingMenu) && !pUI->GetMenuOpen(s_mainMenu);
}

uint32_t PrefixOf(const char* acpPlugin) noexcept
{
    const Mod* pPlugin = ModManager::Get()->GetByName(acpPlugin);
    return pPlugin && pPlugin->IsLoaded() && !pPlugin->IsLite() ? pPlugin->standardId : 0x100;
}

// The local transforms of the first objects below the reference's 3D root; empty without 3D.
ActivationCatchUp::Pose PoseOf(TESObjectREFR* apRef) noexcept
{
    ActivationCatchUp::Pose pose;
    NiNode* pRoot = apRef ? apRef->GetNiNode() : nullptr;
    if (!pRoot)
        return pose;

    NiTree::VisitBelow(
        pRoot, 4 * ActivationCatchUp::kPoseObjects,
        [&pose](void* apObject)
        {
            if (pose.size() >= ActivationCatchUp::kMaxPoseValues)
                return false;
            const auto& cLocal = *reinterpret_cast<const NiTransform*>(static_cast<const uint8_t*>(apObject) + NiLayout::kLocalOffset);
            for (const auto& cRow : cLocal.rotate.data)
                for (const float cValue : cRow)
                    pose.push_back(ActivationCatchUp::QuantizeRotation(cValue));
            pose.push_back(ActivationCatchUp::QuantizeTranslation(cLocal.translate.x));
            pose.push_back(ActivationCatchUp::QuantizeTranslation(cLocal.translate.y));
            pose.push_back(ActivationCatchUp::QuantizeTranslation(cLocal.translate.z));
            return true;
        });
    return pose;
}
} // namespace

ActivationCatchUpService::ActivationCatchUpService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ActivationCatchUpService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ActivationCatchUpService::OnDisconnected>(this);
    m_notifyConnection = aDispatcher.sink<NotifyActivationCatchUp>().connect<&ActivationCatchUpService::OnNotifyActivationCatchUp>(this);
    m_activateConnection = aDispatcher.sink<ActivateEvent>().connect<&ActivationCatchUpService::OnActivate>(this);
}

bool ActivationCatchUpService::FollowsReplay(const TESObjectREFR* apObject) noexcept
{
    return apObject && apObject->baseForm && apObject->baseForm->formType == FormType::Activator &&
           ActivationCatchUp::FollowsReplay(s_lastReplay, std::chrono::steady_clock::now());
}

void ActivationCatchUpService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pacer.Clear();
    m_statePacer.Clear();
    m_reporter.Clear();
}

// This player's own use of a mechanism: its end state is reported once it has come to rest. A replay's Activate does not
// come here (the activation hook does not see it), nor is what its script uses as this player reported (FollowsReplay).
void ActivationCatchUpService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !m_transport.IsConnected() || acEvent.ActivatorId != pPlayer->formID)
        return;

    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
    if (!pRef || pRef->IsTemporary() || FollowsReplay(pRef) || ReadinessOf(pRef->formID) != ActivationCatchUp::Readiness::Go)
        return;

    m_reporter.Used(pRef->formID, PoseOf(pRef), std::chrono::steady_clock::now());
}

void ActivationCatchUpService::ReportStates() noexcept
{
    const auto cPose = [this](uint32_t aFormId)
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
        return pRef && ReadinessOf(aFormId) == ActivationCatchUp::Readiness::Go ? PoseOf(pRef) : ActivationCatchUp::Pose{};
    };
    const auto cSend = [this](uint32_t aFormId, ActivationCatchUp::Pose aPose)
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
        const TESObjectCELL* pCell = pRef ? pRef->GetParentCellEx() : nullptr;
        RequestActivationState request;
        if (!pCell || !m_world.GetModSystem().GetServerModId(aFormId, request.State.Id) || !m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
            return;
        request.State.Pose.assign(aPose.begin(), aPose.end());
        m_transport.Send(request);
        spdlog::debug("ActivationCatchUp: {:X} came to rest in a new position; its end state sent", aFormId);
    };
    m_reporter.Update(std::chrono::steady_clock::now(), cPose, cSend);
}

void ActivationCatchUpService::OnNotifyActivationCatchUp(const NotifyActivationCatchUp& acMessage) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    for (const GameId& cId : acMessage.Activations)
    {
        if (const uint32_t cFormId = m_world.GetModSystem().GetGameId(cId))
            m_pacer.Add(cFormId, cNow);
    }
    for (const ActivationState& cState : acMessage.States)
    {
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(cState.Id);
        if (cFormId && ActivationCatchUp::IsValidPoseSize(cState.Pose.size()))
            m_statePacer.Add(cFormId, ActivationCatchUp::Pose(cState.Pose.begin(), cState.Pose.end()), cNow);
    }

    spdlog::debug(
        "ActivationCatchUp: {} use(s) and {} end state(s) from before this player came near; {} use(s) and {} state(s) waiting", acMessage.Activations.size(),
        acMessage.States.size(), m_pacer.Size(), m_statePacer.Size());
}

ActivationCatchUp::Plugin ActivationCatchUpService::PluginOf(uint32_t aFormId) noexcept
{
    if (!m_pluginsLooked)
    {
        m_pluginsLooked = true;
        m_dawnguardPrefix = PrefixOf("Dawnguard.esm");
        m_dragonbornPrefix = PrefixOf("Dragonborn.esm");
    }

    const uint32_t cPrefix = aFormId >> 24;
    if (cPrefix == 0)
        return ActivationCatchUp::Plugin::Skyrim;
    if (cPrefix == m_dawnguardPrefix)
        return ActivationCatchUp::Plugin::Dawnguard;
    if (cPrefix == m_dragonbornPrefix)
        return ActivationCatchUp::Plugin::Dragonborn;
    return ActivationCatchUp::Plugin::Other;
}

ActivationCatchUp::Readiness ActivationCatchUpService::ReadinessOf(uint32_t aFormId) noexcept
{
    const TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    if (!pRef)
        return ActivationCatchUp::Readiness::Wait;
    if (!pRef->baseForm || pRef->baseForm->formType != FormType::Activator || pRef->IsDeleted() || pRef->IsDisabled() ||
        !ActivationCatchUp::IsMechanism(PluginOf(pRef->baseForm->formID), pRef->baseForm->formID & 0xFFFFFF))
        return ActivationCatchUp::Readiness::Skip;

    const TESObjectCELL* pCell = pRef->GetParentCellEx();
    return pCell && pCell->IsAttached() ? ActivationCatchUp::Readiness::Go : ActivationCatchUp::Readiness::Wait;
}

void ActivationCatchUpService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_pacer.Size() == 0 && m_statePacer.Size() == 0 && m_reporter.Size() == 0)
        return;
    if (!m_transport.IsConnected())
    {
        m_pacer.Clear();
        m_statePacer.Clear();
        m_reporter.Clear();
        return;
    }
    if (!IsInGame())
        return;

    ReportStates();

    const auto cNow = std::chrono::steady_clock::now();
    const auto cStateStep = m_statePacer.Next(
        cNow,
        [this](uint32_t aFormId)
        {
            ActivationCatchUp::StatePacer::Look look{ReadinessOf(aFormId), {}};
            if (look.State == ActivationCatchUp::Readiness::Go)
            {
                look.Current = PoseOf(Cast<TESObjectREFR>(TESForm::GetById(aFormId)));
                if (look.Current.empty())
                    look.State = ActivationCatchUp::Readiness::Wait;
            }
            return look;
        });

    using Action = ActivationCatchUp::StatePacer::Action;
    switch (cStateStep.What)
    {
    case Action::Use: ReplayUse(cStateStep.FormId); return;
    case Action::Done: spdlog::debug("ActivationCatchUp: {:X} is in the end state another game left it in", cStateStep.FormId); break;
    case Action::GaveUp: spdlog::debug("ActivationCatchUp: {:X} cannot be brought to another game's end state here; left as it is", cStateStep.FormId); break;
    case Action::Dropped: spdlog::debug("ActivationCatchUp: {:X} was not loaded here in time; its end state is not set", cStateStep.FormId); break;
    case Action::None: break;
    }

    // A use waits while a state is being set: a keyhole needs its rings to have stopped.
    if (m_statePacer.Busy())
        return;

    const auto cStep = m_pacer.Next(cNow, [this](uint32_t aFormId) { return ReadinessOf(aFormId); });
    if (!cStep.FormId)
        return;

    if (cStep.Dropped)
    {
        spdlog::debug("ActivationCatchUp: {:X} was not loaded here in time; not replayed", cStep.FormId);
        return;
    }

    ReplayUse(cStep.FormId);
}

// As this player: the mechanisms' scripts answer a player (SkyrimTogetherUtils.IsPlayer), and Activate does not come back
// through the activation hook, so it is not sent again; nor is what its script uses as this player (FollowsReplay).
void ActivationCatchUpService::ReplayUse(uint32_t aFormId) noexcept
{
    TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    if (!pRef)
        return;
    spdlog::debug("ActivationCatchUp: {:X} used in another game before this player came; used here too", aFormId);
    s_lastReplay = std::chrono::steady_clock::now();
    pRef->Activate(PlayerCharacter::Get(), 0, nullptr, 1, false);
}
