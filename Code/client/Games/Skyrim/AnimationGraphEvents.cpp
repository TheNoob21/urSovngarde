#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/AnimationGraphEvents.h>

#include <Games/ActorExtension.h>
#include <Games/Animation/IAnimationGraphManagerHolder.h>
#include <Misc/BSFixedString.h>
#include <Services/HitReactionService.h>
#include <Services/GameExtension.h>
#include <HitReactions.h>
#include <atomic>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <PlayerGraphEvents.h>
#include <Systems/InterpolationSystem.h>
#include <Systems/JumpSync.h>

#include <cstring>
#include <mutex>

extern thread_local uint32_t g_playingAction;

namespace
{
// Character's IAnimationGraphManagerHolder vtable and its graph event function.
constexpr size_t kGraphEventSlot = 1;

using TGraphEvent = bool(IAnimationGraphManagerHolder*, const BSFixedString*);
TGraphEvent* s_realGraphEvent = nullptr;

std::mutex s_replayedLock;
HitReactions::ReplayedReactions s_replayed;

// The player's holder has its own vtable; its graph event slot is hooked once the player exists (TakePlayerEvent).
// Set before the slot points at the hook, and read by the hook on whatever thread sends the event.
std::atomic<TGraphEvent*> s_realPlayerGraphEvent{nullptr};
bool s_playerSlotTried = false;

std::mutex s_playerLock;
PlayerGraphEvents::Outbox s_playerEvents;

// The holder part sits at 0x38 in every reference, on SE and VR, which the slot's vtable is for.
static_assert(offsetof(TESObjectREFR, animationGraphHolder) == 0x38);

Actor* ActorOf(IAnimationGraphManagerHolder* apHolder) noexcept
{
    return reinterpret_cast<Actor*>(reinterpret_cast<uint8_t*>(apHolder) - offsetof(TESObjectREFR, animationGraphHolder));
}

bool HookGraphEvent(IAnimationGraphManagerHolder* apThis, const BSFixedString* apEvent)
{
    // The game's strings are pooled, so the same name is the same pointer.
    static const BSFixedString s_stagger("staggerStart");

    const char* cpName = apEvent ? apEvent->data : nullptr;
    const bool cStagger = cpName && cpName == s_stagger.data;
    if (!cStagger)
    {
        const auto kind = apThis && cpName && !g_playingAction ? GameExtension::GraphReaction(cpName) : 0;
        if (kind && kind <= 3)
        {
            auto* actor = ActorOf(apThis);
            const auto* extension = actor->GetExtension();
            if (extension && extension->IsRemote() && !extension->IsPlayer())
            {
                std::scoped_lock lock(s_replayedLock);
                if (!s_replayed.Recently(actor->formID, std::chrono::steady_clock::now()))
                {
                    HitReactionService::OnCopyReaction(actor->formID, static_cast<HitReactions::Kind>(kind - 1));
                    return false;
                }
            }
        }
        const bool cTaken = s_realGraphEvent(apThis, apEvent);
        // A player copy's own landing, from its character controller, ends its jump as a replayed one does (JumpSync::Landing).
        if (cTaken && apThis && cpName && (cpName[0] == 'J' || cpName[0] == 'j') && JumpSync::IsJumpEvent(cpName) && JumpSync::PhaseOf(cpName) == JumpSync::Phase::Ground)
        {
            const auto* pExtension = ActorOf(apThis)->GetExtension();
            if (pExtension && pExtension->IsRemotePlayer())
                InterpolationSystem::NoteCopyJumpEvent(ActorOf(apThis)->formID, cpName, true);
        }
        return cTaken;
    }

    Actor* pActor = ActorOf(apThis);
    const ActorExtension* pExtension = pActor->GetExtension();

    HitReactions::RawGraphEvent event;
    event.IsCopy = pExtension && pExtension->IsRemote();
    event.IsPlayer = pExtension && pExtension->IsPlayer();
    event.IsStagger = cStagger;
    event.PlayingAction = g_playingAction > 0;
    if (event.IsCopy && cStagger && !event.PlayingAction)
    {
        std::scoped_lock lock(s_replayedLock);
        event.ReplayedReaction = s_replayed.Recently(pActor->formID, std::chrono::steady_clock::now());
    }

    const bool cHeld = HitReactions::HoldsRawStagger(event);
    // Its owner's game decides; a reaction paired with this player's native hit is sent there.
    if (cHeld)
        HitReactionService::OnCopyRawStagger(pActor->formID);
    const bool cTaken = !cHeld && s_realGraphEvent(apThis, apEvent);

    return cTaken;
}

// Papyrus and the console can send the player's graph events from other threads than the game's.
bool HookPlayerGraphEvent(IAnimationGraphManagerHolder* apThis, const BSFixedString* apEvent)
{
    const bool cTaken = s_realPlayerGraphEvent.load(std::memory_order_acquire)(apThis, apEvent);
    // An action the game performs sends its event meanwhile, and the action itself is sent (HookPerformAction).
    if (cTaken && !g_playingAction && apThis && apEvent && apEvent->data && ActorOf(apThis)->formID == 0x14)
    {
        std::scoped_lock lock(s_playerLock);
        s_playerEvents.Push(apEvent->data, std::chrono::steady_clock::now());
    }
    return cTaken;
}

void HookPlayerSlot(PlayerCharacter* apPlayer) noexcept
{
    s_playerSlotTried = true;
    auto* pVtable = *reinterpret_cast<void***>(&apPlayer->animationGraphHolder);
    auto* const pSlot = pVtable ? &pVtable[kGraphEventSlot] : nullptr;
    DWORD oldProtect = 0;
    if (pSlot && *pSlot && *pSlot != reinterpret_cast<void*>(&HookPlayerGraphEvent) && VirtualProtect(pSlot, sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        // Whatever the slot held, the game's function or another plugin's hook (or Character's above, were the vtable shared), still runs.
        s_realPlayerGraphEvent.store(reinterpret_cast<TGraphEvent*>(*pSlot), std::memory_order_release);
        *pSlot = reinterpret_cast<void*>(&HookPlayerGraphEvent);
        VirtualProtect(pSlot, sizeof(void*), oldProtect, &oldProtect);
    }
    if (!s_realPlayerGraphEvent.load())
        spdlog::error("Player graph events: the player's graph event slot not found; emotes sent as graph events are not shown to other players");
}

TiltedPhoques::Initializer s_graphEventHook(
    []()
    {
        POINTER_SKYRIMSE(void*, s_vtable, 207892, 261400);
        auto* pVtable = reinterpret_cast<void**>(s_vtable.Get());
        auto* const pSlot = pVtable ? &pVtable[kGraphEventSlot] : nullptr;
        DWORD oldProtect = 0;
        if (pSlot && *pSlot && VirtualProtect(pSlot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            s_realGraphEvent = reinterpret_cast<TGraphEvent*>(*pSlot);
            *pSlot = reinterpret_cast<void*>(&HookGraphEvent);
            VirtualProtect(pSlot, sizeof(void*), oldProtect, &oldProtect);
        }
        if (!s_realGraphEvent)
            spdlog::error("Hit reaction: Character's graph event slot not found; a copy may stagger on its own here before its owner's stagger comes");
    });
} // namespace

namespace AnimationGraphEvents
{
void NoteReplayedReaction(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_replayedLock);
    s_replayed.Note(aFormId, std::chrono::steady_clock::now());
}

std::optional<std::string_view> TakePlayerEvent() noexcept
{
    if (!s_playerSlotTried)
    {
        if (auto* pPlayer = PlayerCharacter::Get())
            HookPlayerSlot(pPlayer);
    }

    std::scoped_lock lock(s_playerLock);
    return s_playerEvents.Pop();
}

} // namespace AnimationGraphEvents
