#include <Services/HeadTrackService.h>
#include <Services/CopyWorkGate.h>
#include <Helpers/PerfScope.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>

#include <Messages/RequestNpcHeadTrack.h>
#include <Messages/NotifyNpcHeadTrack.h>

#include <World.h>
#include <Utils.h>

#include <PlayerCharacter.h>
#include <Misc/BSFixedString.h>
#include <Copies/EpochMiss.h>

namespace
{
// Words per message, under what a game takes in one (NpcHeadTrackWords::kMax).
constexpr size_t kLookWordsPerMessage = 400;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The graph's vector variables (AE 32884 / 32888; on VR SE 32192 / 32144, pinned in VRAddressOverrides.h).
// The game writes the head-track target point into "TargetLocation" for any actor whose AI runs.
TP_THIS_FUNCTION(TGetGraphVariableNiPoint3, bool, IAnimationGraphManagerHolder, const BSFixedString* apName, NiPoint3* apOut);
TP_THIS_FUNCTION(TSetGraphVariableNiPoint3, bool, IAnimationGraphManagerHolder, const BSFixedString* apName, NiPoint3* apIn);

BSFixedString& TrackingName() noexcept
{
    static BSFixedString s_name("bHeadTracking");
    return s_name;
}

BSFixedString& SpineName() noexcept
{
    static BSFixedString s_name("bHeadTrackSpine");
    return s_name;
}

BSFixedString& TargetName() noexcept
{
    static BSFixedString s_name("TargetLocation");
    return s_name;
}

bool GetTarget(Actor* apActor, NiPoint3& aOut) noexcept
{
    POINTER_SKYRIMSE(TGetGraphVariableNiPoint3, s_getGraphVariableNiPoint3, 32884, 32192);
    if (!s_getGraphVariableNiPoint3.Get())
        return false;
    return TiltedPhoques::ThisCall(s_getGraphVariableNiPoint3.Get(), &apActor->animationGraphHolder, &TargetName(), &aOut);
}

bool SetTarget(Actor* apActor, NiPoint3& aIn) noexcept
{
    POINTER_SKYRIMSE(TSetGraphVariableNiPoint3, s_setGraphVariableNiPoint3, 32888, 32144);
    if (!s_setGraphVariableNiPoint3.Get())
        return false;
    return TiltedPhoques::ThisCall(s_setGraphVariableNiPoint3.Get(), &apActor->animationGraphHolder, &TargetName(), &aIn);
}

//! What one of this game's NPCs looks at, read from its graph.
NpcHeadTrack::Look ReadLook(Actor* apActor) noexcept
{
    if (apActor->IsDead() || !apActor->GetNiNode() || !apActor->animationGraphHolder.IsReady())
        return {};

    bool tracking = false;
    bool spine = false;
    const bool cReadTracking = apActor->animationGraphHolder.GetVariableBool(&TrackingName(), &tracking);
    if (cReadTracking && tracking)
        apActor->animationGraphHolder.GetVariableBool(&SpineName(), &spine);

    NiPoint3 target{};
    const bool cReadPoint = cReadTracking && tracking && GetTarget(apActor, target);
    return NpcHeadTrack::ReadLook(cReadTracking, tracking, spine, cReadPoint, {target.x, target.y, target.z});
}

//! The look written into a copy's graph; false when its graph has no look target to write.
bool WriteLook(Actor* apCopy, const uint8_t aFlags, const NpcHeadTrack::Point& acAt) noexcept
{
    NiPoint3 target{};
    target.x = acAt.X;
    target.y = acAt.Y;
    target.z = acAt.Z;
    if (!SetTarget(apCopy, target))
        return false;
    apCopy->animationGraphHolder.SetVariableBool(&TrackingName(), NpcHeadTrack::IsTracking(aFlags));
    apCopy->animationGraphHolder.SetVariableBool(&SpineName(), (aFlags & NpcHeadTrack::kSpine) != 0);
    return true;
}

void WriteStop(Actor* apCopy) noexcept
{
    apCopy->animationGraphHolder.SetVariableBool(&TrackingName(), false);
    apCopy->animationGraphHolder.SetVariableBool(&SpineName(), false);
}

Actor* ActorOf(World& aWorld, const entt::entity aEntity) noexcept
{
    const auto* pFormId = aWorld.try_get<FormIdComponent>(aEntity);
    return pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
}
} // namespace

HeadTrackService::HeadTrackService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&HeadTrackService::OnUpdate>(this);
    m_npcHeadTrackConnection = aDispatcher.sink<NotifyNpcHeadTrack>().connect<&HeadTrackService::OnNotifyNpcHeadTrack>(this);
}

void HeadTrackService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfScope perfScope("HeadTrackService::OnUpdate");

    // Not while the game exits or loads, nor just after a load: the graphs read and written here may be made again (CopyWork.h).
    static CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate))
        return;

    const uint64_t cNow = NowMs();
    // Every frame, so the copies' heads turn smoothly between their owners' words.
    UpdateCopyLooks(cNow, static_cast<float>(acEvent.Delta * 1000.0));
    StepOwnLooks(cNow);
}

// The owner's side: where each NPC this game runs looks, sent when it changes and at each heartbeat. The NPCs are read a few
// each frame, so that a scene full of them does not cost one frame all of it.
void HeadTrackService::StepOwnLooks(const uint64_t aNowMs) noexcept
{
    // Only once the server has accepted this player: it kicks a connection that sends anything before.
    if (!m_transport.IsOnline())
    {
        if (m_lookRound.Active())
        {
            m_lookRound.End();
            m_lookOutbox.Abandon();
        }
        return;
    }

    if (!m_lookRound.Active())
    {
        if (!m_lookRound.Due(aNowMs))
            return;

        m_lookQueue.clear();
        const auto cView = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<PlayerComponent>);
        for (const entt::entity cEntity : cView)
            m_lookQueue.push_back(cEntity);
        m_lookRound.Start(aNowMs, m_lookQueue.size());
    }

    const NpcHeadTrack::ReadRound::Slice cSlice = m_lookRound.Next();
    for (size_t i = cSlice.Begin; i < cSlice.End; ++i)
    {
        // Looked up again: an NPC can be gone or no longer this game's since the round began.
        const entt::entity cEntity = m_lookQueue[i];
        if (!m_world.valid(cEntity))
            continue;
        const auto* pForm = m_world.try_get<FormIdComponent>(cEntity);
        const auto* pLocal = m_world.try_get<LocalComponent>(cEntity);
        if (!pForm || !pLocal || m_world.all_of<PlayerComponent>(cEntity))
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(pForm->Id));
        if (!pActor || pActor == PlayerCharacter::Get())
            continue;

        m_lookOutbox.Put(pLocal->Id, ReadLook(pActor), pLocal->OwnershipEpoch);
    }

    if (m_lookRound.Finished())
    {
        m_lookRound.End();
        SendOwnLooks(aNowMs);
    }
}

void HeadTrackService::SendOwnLooks(const uint64_t aNowMs) noexcept
{
    const auto cWords = m_lookOutbox.Take(aNowMs);
    for (size_t first = 0; first < cWords.size(); first += kLookWordsPerMessage)
    {
        RequestNpcHeadTrack request{};
        const size_t cLast = std::min(cWords.size(), first + kLookWordsPerMessage);
        request.Words.reserve(cLast - first);
        for (size_t i = first; i < cLast; ++i)
        {
            const NpcHeadTrack::Word& cWord = cWords[i];
            request.Words.push_back({cWord.NpcId, cWord.What.Flags, cWord.What.At.X, cWord.What.At.Y, cWord.What.At.Z, cWord.OwnershipEpoch});
        }
        m_transport.Send(request);
    }
}

// The copy's side: each copy looks where its owner's NPC looks until the owner says it stopped, or says nothing for 2.5 s.
void HeadTrackService::OnNotifyNpcHeadTrack(const NotifyNpcHeadTrack& acMessage) noexcept
{
    const uint64_t cNow = NowMs();
    for (const NpcHeadTrackWord& cWord : acMessage.Words)
    {
        const std::optional<entt::entity> cEntity = Utils::FindEntityByServerId(cWord.NpcId);
        const auto* pRemote = cEntity ? m_world.try_get<RemoteComponent>(*cEntity) : nullptr;
        const NpcHeadTrack::CopyVerdict cVerdict = NpcHeadTrack::CopyTakes(cEntity.has_value(), pRemote != nullptr, pRemote ? pRemote->OwnershipEpoch : 0, cWord.OwnershipEpoch);
        if (cVerdict == NpcHeadTrack::CopyVerdict::kEpochMiss)
            ReportEpochMiss("where an NPC looks", cWord.NpcId, cWord.OwnershipEpoch, pRemote->OwnershipEpoch);
        if (cVerdict != NpcHeadTrack::CopyVerdict::kTake || m_world.all_of<PlayerComponent>(*cEntity))
            continue;

        // A copy that never looked has nothing to stop.
        auto* pLook = m_world.try_get<CopyHeadTrackComponent>(*cEntity);
        if (!pLook && !NpcHeadTrack::IsTracking(cWord.Flags))
            continue;
        if (!pLook)
            pLook = &m_world.emplace<CopyHeadTrackComponent>(*cEntity);

        // The look this word continues was another owner's: the head is not eased from it.
        if (pLook->OwnershipEpoch != cWord.OwnershipEpoch)
            pLook->Showing = false;
        pLook->Wanted = {cWord.Flags, {cWord.X, cWord.Y, cWord.Z}};
        pLook->AtMs = cNow;
        pLook->OwnershipEpoch = cWord.OwnershipEpoch;
    }
}

void HeadTrackService::UpdateCopyLooks(const uint64_t aNowMs, const float aDeltaMs) noexcept
{
    Vector<entt::entity> done;
    const auto cView = m_world.view<CopyHeadTrackComponent>();
    for (const entt::entity cEntity : cView)
    {
        auto& look = cView.get<CopyHeadTrackComponent>(cEntity);
        Actor* pCopy = ActorOf(m_world, cEntity);
        // A copy this game took over runs its own AI again, which writes its own look once ours is stopped.
        const bool cIsCopy = pCopy && m_world.all_of<RemoteComponent>(cEntity) && !m_world.all_of<PlayerComponent>(cEntity);
        const bool cReady = pCopy && pCopy->GetNiNode() && pCopy->animationGraphHolder.IsReady();
        // A word was taken from its owner at an epoch, and a look written into one form. A copy that has another owner since, or
        // is another form, is not written to for the old word; and the other form is not told to stop (it may not be ours).
        const auto* pRemote = m_world.try_get<RemoteComponent>(cEntity);
        const bool cRebound = look.Written && pCopy && pCopy->formID != look.FormId;
        const bool cStale = cRebound || (pRemote && pRemote->OwnershipEpoch != look.OwnershipEpoch);
        const bool cWritten = look.Written && !cRebound;
        // Combat points the copy's head at its enemy (CombatService): that look owns the graph's flags while it lasts.
        const auto* pFight = m_world.try_get<CopyFightComponent>(cEntity);
        const bool cCombatLooks = pFight && pFight->LookingAtServerId != 0;

        switch (
            NpcHeadTrack::CopyAction(cIsCopy, pCopy && pCopy->IsDead(), look.Wanted.Flags, NpcHeadTrack::CopyExpired(look.AtMs, aNowMs), cReady, cCombatLooks, cWritten, cStale))
        {
        case NpcHeadTrack::CopyStep::kLook:
        {
            // Eased over the time since the last write, which adds up to the same as a step every frame.
            look.PendingMs += aDeltaMs;
            const NpcHeadTrack::Point cNext = NpcHeadTrack::Ease(look.Showing, look.Shown, look.Wanted.At, look.PendingMs);
            if (!NpcHeadTrack::NeedsWrite(look.Showing, look.ShownFlags, look.Wanted.Flags, look.Shown, cNext, look.PendingMs))
                break;

            if (!WriteLook(pCopy, look.Wanted.Flags, cNext))
            {
                // A graph with no look target (a creature's): nothing to show on this copy.
                spdlog::debug("HeadTrack: copy {:X} has no look target in its graph; its owner's look is not shown", pCopy->formID);
                done.push_back(cEntity);
                break;
            }
            if (!look.Written)
            {
                spdlog::debug(
                    "HeadTrack: copy {:X} looks at ({:.0f}, {:.0f}, {:.0f}) as its owner's NPC does", pCopy->formID, look.Wanted.At.X, look.Wanted.At.Y, look.Wanted.At.Z);
                look.FormId = pCopy->formID;
            }
            look.Shown = cNext;
            look.ShownFlags = look.Wanted.Flags;
            look.PendingMs = 0.f;
            look.Showing = true;
            look.Written = true;
            break;
        }
        case NpcHeadTrack::CopyStep::kWait:
            // Shown again from the owner's point when the combat look ends, not eased from where it was before the fight.
            look.Showing = look.Showing && !cCombatLooks;
            look.PendingMs = 0.f;
            break;
        case NpcHeadTrack::CopyStep::kStop:
            WriteStop(pCopy);
            spdlog::debug(
                "HeadTrack: copy {:X} stops looking: {}", pCopy->formID,
                !cIsCopy ? "this game runs it now"
                         : (cStale ? "its owner changed" : (NpcHeadTrack::CopyExpired(look.AtMs, aNowMs) ? "no word from its owner for 2.5 s" : "its owner's NPC stopped")));
            done.push_back(cEntity);
            break;
        case NpcHeadTrack::CopyStep::kForget:
        default: done.push_back(cEntity); break;
        }
    }

    for (const entt::entity cEntity : done)
        m_world.remove<CopyHeadTrackComponent>(cEntity);
}
