#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESIdleForm.h>

#include <Structs/ActionEvent.h>

#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>

#include <Misc/BSFixedString.h>

#include <World.h>
#include <Copies/Takeover.h>
#include <HitReactions.h>
#include <Services/HitReactionService.h>
#include <Games/Skyrim/AnimationGraphEvents.h>
#include <Games/Skyrim/CopyBodyRules.h>
#include <Games/Skyrim/VR/VRBodySync.h>

#include <LogPolicy.h>

#include <mutex>

TP_THIS_FUNCTION(TPerformAction, uint8_t, ActorMediator, TESActionData* apAction);
static TPerformAction* RealPerformAction;

// TODO: make scoped override
thread_local bool g_forceAnimation = false;

// Above zero while the game plays an action this client performs for its own actor or replays on a copy,
// so the graph events sent meanwhile are told from those the game or a mod sends on its own (AnimationGraphEvents).
thread_local uint32_t g_playingAction = 0;

namespace
{
struct ScopedPlayingAction
{
    ScopedPlayingAction() noexcept { ++g_playingAction; }
    ~ScopedPlayingAction() noexcept { --g_playingAction; }
};
} // namespace

namespace
{
// TEMPORARY: logs which actions flood the stream, and how many of them the game refused on the sending actor.
// Logged once per actor every thirty seconds, when one actor performs the same action more than ten times in a second.
void NoteActionRate(uint32_t aActorId, uint32_t aActionId, const char* acpEventName, bool aWorked) noexcept
{
    struct Rate
    {
        std::chrono::steady_clock::time_point WindowStart{};
        uint32_t Count = 0;
        uint32_t Failed = 0;
    };
    static std::mutex s_lock;
    static TiltedPhoques::Map<uint64_t, Rate> s_rates;
    static TiltedPhoques::Map<uint32_t, int64_t> s_nextReportMs;

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock _{s_lock};
    if (s_rates.size() > 512)
        s_rates.clear();
    if (s_nextReportMs.size() > 512)
        s_nextReportMs.clear();
    Rate& rate = s_rates[(static_cast<uint64_t>(aActorId) << 32) | aActionId];
    if (now - rate.WindowStart >= std::chrono::seconds(1))
    {
        const int64_t cNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (rate.Count > 10 && LogPolicy::ReportDue(cNowMs, s_nextReportMs[aActorId], 30000))
        {
            spdlog::info("ActionFlood: actor {:X} performed action {:X} ({}) {} times in a second, {} of them refused by the game here and not sent",
                         aActorId, aActionId, acpEventName ? acpEventName : "?", rate.Count, rate.Failed);
        }
        rate.WindowStart = now;
        rate.Count = 0;
        rate.Failed = 0;
    }
    ++rate.Count;
    if (!aWorked)
        ++rate.Failed;
}
// Whether this action is the one just sent for this actor, again, within a quarter of a second.
// A creature standing still can perform IdleSpecialStart about forty times a second, and other players gain nothing from the repeats.
bool IsRepeatOfLastSent(const ActionEvent& acAction) noexcept
{
    // Idles only. Movement and attacks are never held back, because filtering them breaks the creature gait on remote copies.
    const bool cIdle = acAction.IdleId != 0 || acAction.EventName.rfind("Idle", 0) == 0;
    if (!cIdle)
        return false;

    struct Last
    {
        uint32_t ActionId = 0;
        uint32_t IdleId = 0;
        uint32_t TargetId = 0;
        std::chrono::steady_clock::time_point SentAt{};
    };
    static std::mutex s_lock;
    static TiltedPhoques::Map<uint32_t, Last> s_last;

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock _{s_lock};
    if (s_last.size() > 1024)
        s_last.clear();
    Last& last = s_last[acAction.ActorId];
    if (last.ActionId == acAction.ActionId && last.IdleId == acAction.IdleId && last.TargetId == acAction.TargetId &&
        now - last.SentAt < std::chrono::milliseconds(250))
        return true;
    last = Last{acAction.ActionId, acAction.IdleId, acAction.TargetId, now};
    return false;
}
} // namespace

uint8_t TP_MAKE_THISCALL(HookPerformAction, ActorMediator, TESActionData* apAction)
{
    auto pActor = apAction->actor;
    // GetExtension() returns null for an actor this client did not allocate (Actor::ForcePosition): its actions are the game's alone.
    const auto pExtension = pActor ? pActor->GetExtension() : nullptr;
    if (!pExtension)
        return TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);

    if (!pExtension->IsRemote() || g_forceAnimation)
    {
        ActionEvent action;
        action.State1 = pActor->actorState.flags1;
        action.State2 = pActor->actorState.flags2;
        action.Type = apAction->unkInput | (apAction->someFlag ? 0x4 : 0);
        action.Tick = World::Get().GetTick();
        action.ActorId = pActor->formID;
        action.ActionId = apAction->action->formID;
        action.TargetId = apAction->target ? apAction->target->formID : 0;

        pActor->SaveAnimationVariables(action.Variables);

        uint8_t res = 0;
        {
            ScopedPlayingAction playing;
            res = TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);
        }
        // TEMPORARY: what this game's AI performs on an NPC it just gained.
        Takeover::NoteOwnAction(action.ActorId, apAction->eventName.AsAscii(), res != 0);

        // spdlog::debug("Action event name: {}, target name: {}", apAction->eventName.AsAscii(), apAction->targetEventName.AsAscii());

        // This is a weird case where it gets spammed and doesn't do much, not sure if it still needs to be sent over the network
        if (apAction->someFlag == 1 || g_forceAnimation)
            return res;

        action.EventName = apAction->eventName.AsAscii();
        action.TargetEventName = apAction->targetEventName.AsAscii();
        action.IdleId = apAction->idleForm ? apAction->idleForm->formID : 0;

        // Save for later
        if (res)
        {
            pExtension->LatestAnimation = action;
        }

        NoteActionRate(action.ActorId, action.ActionId, apAction->eventName.AsAscii(), res != 0);

        // An action the game refused here did not happen, so it is not sent.
        // Otherwise other players would replay something the owner's actor never did, and refused actions are often retried every frame.
        if (!res)
            return res;

        if (IsRepeatOfLastSent(action))
            return res;

        World::Get().GetRunner().Trigger(action);

        return res;
    }

    // The game ends a copy's bleedout itself (Actor::SetLifeState, after a heal here or when its bleedout runs out): it writes the copy
    // alive, then performs its stop. Dropped like the game's other actions on a copy, that stop left its graph kneeling for good while
    // it read alive (live-copy-essential-up). It plays once its owner's NPC is up; until then it is held back, and the copy kneels on
    // as its owner's does, remembered so that it gets up when its owner's does (InterpolationSystem).
    if (!pExtension->IsPlayer() && apAction->action && apAction->action->formID == CopyBodyRules::kActionBleedoutStop)
    {
        const uint32_t cFlags1 = pActor->actorState.flags1;
        const auto cOwner = static_cast<CopyBodyRules::OwnerWord>(pExtension->OwnerWordOfCopy.load(std::memory_order_relaxed));
        if (CopyBodyRules::LetsEngineStopThrough(true, true, cFlags1, cOwner))
        {
            uint8_t res = 0;
            {
                ScopedPlayingAction playing;
                res = TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);
            }
            pExtension->EngineStopPlayed.store(res ? 1 : 2, std::memory_order_relaxed);
            if (res)
                pExtension->KneelsInGraph.store(false, std::memory_order_relaxed);
            spdlog::debug("KnockDown: NPC copy {:X} ended its bleedout the game's way; its stop {}", pActor->formID, res ? "played" : "was refused");
            return res;
        }
        if (CopyBodyRules::KneelsInGraphOnly(true, true, cFlags1))
        {
            pExtension->KneelsInGraph.store(true, std::memory_order_relaxed);
            spdlog::debug("KnockDown: NPC copy {:X} ended its bleedout here while its owner's NPC is not up; it kneels on until its owner's gets up", pActor->formID);
        }
        pExtension->EngineStopPlayed.store(2, std::memory_order_relaxed);
        return 0;
    }

    // A copy plays only its owner's actions. A stagger or recoil it wants here is this game's hit at work,
    // so its owner's game is told, and plays it (HitReactionService).
    if (const auto cKind = HitReactions::KindOfAction(apAction->action ? apAction->action->formID : 0); cKind && !pExtension->IsPlayer())
        HitReactionService::OnCopyReaction(pActor->formID, *cKind);

    return 0;
}

bool ActorMediator::PerformOwnAction(TESActionData* apAction) noexcept
{
    return HookPerformAction(this, apAction) != 0;
}

ActorMediator* ActorMediator::Get() noexcept
{
    POINTER_SKYRIMSE(ActorMediator*, s_actorMediator, 403567, 403567);

    return *(s_actorMediator.Get());
}

bool ActorMediator::PerformAction(TESActionData* apAction) noexcept
{
    if (apAction->actor->formID == 0x13482)
    {
        /*static Set<uint32_t> s_ids;

        spdlog::error("New frame");
        for(auto i = 0; i < action.Variables.size(); ++i)
        {
            auto& oldVars = pExtension->LatestVariables.Variables;
            auto& newVars = action.Variables;
            if(oldVars[i] != newVars[i] && s_ids.count(i) == 0)
            {
                //s_ids.insert(i);
                spdlog::info("Var {} changed from {} to {}", i, oldVars[i], newVars[i]);
            }
        }*/
        // spdlog::info("Play animation name: {} with idle {:X} and target {:X} and unk {:X}", apAction->action->keyword.AsAscii(), (apAction->idleForm ? apAction->idleForm->formID : 0), (apAction->target ? apAction->target->formID : 0), apAction->unkInput);
    }

    const auto res = TiltedPhoques::ThisCall(RealPerformAction, this, apAction);
    // const auto res = RePerformAction(apAction, aValue);

    if (res && apAction->actor->formID == 0x13482)
    {
        //    spdlog::info("Passed !");
    }

    return res != 0;
}

bool ActorMediator::ForceAction(TESActionData* apAction) noexcept
{
    TP_THIS_FUNCTION(TAnimationStep, uint8_t, ActorMediator, TESActionData*);
    using TApplyAnimationVariables = void*(void*, TESActionData*);

    POINTER_SKYRIMSE(TApplyAnimationVariables, ApplyAnimationVariables, 39004, 39004);
    POINTER_SKYRIMSE(TAnimationStep, PerformComplexAction, 38953, 38953);
    POINTER_SKYRIMSE(void*, qword_142F271B8, 403566, 403566);

    uint8_t result = 0;

    auto pActor = static_cast<Actor*>(apAction->actor);
    if (pActor)
    {
        // A copy replaying its owner's stagger or recoil: the graph may take the stagger after this returns.
        if (HitReactions::KindOfAction(apAction->action ? apAction->action->formID : 0))
            AnimationGraphEvents::NoteReplayedReaction(pActor->formID);

        ScopedPlayingAction playing;
        result = TiltedPhoques::ThisCall(PerformComplexAction, this, apAction);

        ApplyAnimationVariables(*qword_142F271B8.Get(), apAction);
    }

    return result;
}

ActionInput::ActionInput(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
{
    // skip vtable as we never use this directly
    actor = apActor;
    target = apTarget;
    action = apAction;
    unkInput = aParam1;
}

void ActionInput::Release()
{
    actor.Release();
    target.Release();
}

ActionOutput::ActionOutput()
    : eventName("")
    , targetEventName("")
{
    // skip vtable as we never use this directly

    result = 0;
    targetIdleForm = nullptr;
    idleForm = nullptr;
    unk1 = 0;
}

void ActionOutput::Release()
{
    eventName.Release();
    targetEventName.Release();
}

BGSActionData::BGSActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : ActionInput(aParam1, apActor, apAction, apTarget)
{
    // skip vtable as we never use this directly
    someFlag = 0;
}

TESActionData::TESActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : BGSActionData(aParam1, apActor, apAction, apTarget)
{
    POINTER_SKYRIMSE(void*, s_vtbl, 188603, 232777);

    someFlag = false;

    *reinterpret_cast<void**>(this) = s_vtbl.Get();
}

TESActionData::~TESActionData()
{
    ActionOutput::Release();
    ActionInput::Release();
}

static TiltedPhoques::Initializer s_animationHook(
    []()
    {
        POINTER_SKYRIMSE(TPerformAction, performAction, 38949, 38949);

        RealPerformAction = performAction.Get();

        TP_HOOK(&RealPerformAction, HookPerformAction);
    });
