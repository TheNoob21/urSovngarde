#include <TiltedOnlinePCH.h>
#include "SEBodyPresentation.h"

#ifndef SKYRIMVR
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Helpers/PatchCheck.h>
#include <VersionDb.h>
#include <Systems/BodyPosePresentation.h>
#include <Systems/BodyPoseWorldInput.h>
#include <Systems/BodyPoseCommit.h>
#include <Systems/BodyPoseBounds.h>
#include <Systems/HandReach.h>
#include <Systems/PosePublishThread.h>
#include <Systems/RemoteBodyPoser.h>
#include <Systems/RemoteBodySlots.h>
#include <Services/PosedHands.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace SEBodyPresentation
{
namespace
{
// Skyrim SE 1.7.104.
constexpr uintptr_t kTreeVtableRva = 0x1A492B8;
constexpr uintptr_t kHelperRva = 0xEF4F50;
constexpr uint64_t kHelperId = 70746;
constexpr size_t kMaxActors = 8, kMaxSourceLedgers = 128, kMaxGraphNodes = 512, kMaxDepth = 64;
constexpr size_t kMaxTreeBones = 512;
constexpr std::array<const char*, VRPose::kBoneCount> kBoneNames{
    "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",     "NPC Neck [Neck]",       "NPC Head [Head]",      "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]",
    "NPC L Hand [LHnd]", "NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",     "NPC Pelvis [Pelv]",     "NPC L Thigh [LThg]",
    "NPC L Calf [LClf]", "NPC L Foot [Lft ]",     "NPC R Thigh [RThg]",    "NPC R Calf [RClf]",    "NPC R Foot [Rft ]"};
bool s_skinCallsReady{};
std::atomic<bool> s_commitPassReady{};
struct Binding
{
    uint32_t FormId{};
    RemotePose::Sample Sample{};
    RemotePose::Identity TrustedIdentity{}; // receiver lifecycle identity, never inferred from packets
    double PublishedNetworkMs{};
    double ExpiresAtSteadyMs{};
    uintptr_t RootHint{}, TreeHint{};
};
// Registry-mutex-owned committed values only. These numeric root/tree hints confer no engine lifetime;
// every consumer must still qualify the current native retained-root pass before using them.
struct PresentationGrace
{
    BodyPoseCommit::GraceWindow Window{};
    std::shared_ptr<const Binding> Source;
    BodyPoseCommit::FlipPose Pose{};
    bool FlipHeld{};
    BodyPoseCommit::FlipDecision GuardDecision{};
    double GuardAssessedMs{};
    uint64_t SourceCommit{};
    double LastNetworkMs{};
};
BodyPoseCommit::GraceKey GraceKeyOf(const Binding& acBinding) noexcept
{
    return {
        acBinding.TrustedIdentity.Session,
        acBinding.TrustedIdentity.Owner,
        acBinding.TrustedIdentity.Entity,
        acBinding.Sample.Generation,
        acBinding.RootHint,
        acBinding.TreeHint,
        acBinding.FormId};
}
bool GraceSourcePoseValid(const VRPose& acPose) noexcept
{
    if (!acPose.HasData || acPose.NoBones || !BodyPosePresentation::Finite(glm::vec3{acPose.HipOffset[0], acPose.HipOffset[1], acPose.HipOffset[2]}))
        return false;
    for (size_t wire = 0; wire < (acPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount); ++wire)
        if (!BodyPosePresentation::Unit(static_cast<glm::quat>(acPose.Bones[wire])))
            return false;
    if (acPose.HasFingers)
        for (const auto& finger : acPose.Fingers)
            if (!BodyPosePresentation::Unit(static_cast<glm::quat>(finger)))
                return false;
    if (acPose.HasHandCheck)
        for (size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(acPose.LeftHandOffset[axis]) || !std::isfinite(acPose.RightHandOffset[axis]))
                return false;
    return true;
}
using Clock = std::chrono::steady_clock;
using Helper = void(void*);
Helper* s_original{};
uintptr_t s_imageBase{};
volatile uint32_t* s_engineFrame{};
std::atomic<bool> s_hookReady{};
std::atomic<DWORD> s_ownerThread{};
std::atomic<size_t> s_activeCount{};
std::atomic<uint64_t> s_gameFrame{}, s_presentFrame{};
std::mutex s_mutex;
uint64_t s_registryEpoch{}; // guarded by s_mutex; prevents a concurrent Forget/Clear being undone by Publish
void Disarm(uint32_t aFormId) noexcept;

double SteadyMs() noexcept
{
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
uint32_t EngineFrame() noexcept
{
    return s_engineFrame ? *s_engineFrame : 0;
}

struct JobIdentity
{
    std::atomic<uint64_t> Revision{}, Generation{}, SourceTick{};
    std::atomic<uint32_t> Actor{};
    std::atomic<uintptr_t> Tree{}, RootHint{};
    std::atomic<double> Deadline{};
};
// One slot per remote VR player's copy the native commit poses (RemoteBodySlots). The hooks read Actor, Identity, Epoch
// and the two words without the registry mutex; Grace, Dwell and DwellActor are the mutex's, and s_owners mirrors the
// slots' owners for the slot choice.
struct BodySlot
{
    std::atomic<uint32_t> Actor{};
    JobIdentity Identity;
    PresentationGrace Grace;
    BodyPoseCommit::PresentationDwell Dwell; // deliberately survives every binding/lifecycle reset
    uint32_t DwellActor{};
    std::atomic<uint64_t> Epoch{}; // changes with the slot's copy; the commit state then starts over
    // The copy whose hands the last commit posed from the VR pose, which of them, and in which engine frame: what a held
    // object may be drawn from. One word, so the three are read together: actor << 32 | frame (30 bits) << 2 | sides.
    std::atomic<uint64_t> PosedHands{};
    // The copy the last commit posed and its engine frame (RemoteBodyPoser::CommitWord).
    std::atomic<uint64_t> CommittedBody{};
};
constexpr size_t kSlots = kMaxActors, kNoSlot = kSlots;
std::array<BodySlot, kSlots> s_slots;
std::array<RemoteBodySlots::Slot, kSlots> s_owners; // guarded by s_mutex
size_t SlotOf(const uint32_t aActor) noexcept
{
    for (size_t i = 0; aActor && i < s_slots.size(); ++i)
        if (s_slots[i].Actor.load(std::memory_order_acquire) == aActor)
            return i;
    return kNoSlot;
}
void InvalidateGraceLocked(BodySlot& aSlot) noexcept
{
    aSlot.Grace.Window.Invalidate();
    aSlot.Grace.Source.reset();
}
void InvalidateGraceLocked(uint32_t aActor = 0) noexcept
{
    for (auto& slot : s_slots)
        if (!aActor || slot.Grace.Window.Key.Actor == aActor)
            InvalidateGraceLocked(slot);
}
constexpr uint32_t kPosedFrameMask = 0x3FFFFFFF;
uint64_t PosedHandsWord(const uint32_t aActor, const uint32_t aFrame, const uint32_t aSides) noexcept
{
    return static_cast<uint64_t>(aActor) << 32 | static_cast<uint64_t>(aFrame & kPosedFrameMask) << 2 | (aSides & 3u);
}
bool HandsPosedFromVR(const uint32_t aActorFormId, const size_t aSide) noexcept
{
    const size_t cSlot = SlotOf(aActorFormId);
    if (cSlot == kNoSlot || aSide > 1)
        return false;
    const uint64_t cWord = s_slots[cSlot].PosedHands.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(cWord >> 32) != aActorFormId || !((cWord >> aSide) & 1u))
        return false;
    // Committed this engine frame or the one before: the hands are read on the main-thread update after the commit.
    // Any older and the native traversal has put the animation hand back.
    const uint32_t cAge = (EngineFrame() - static_cast<uint32_t>(cWord >> 2)) & kPosedFrameMask;
    return cAge <= 1;
}
// aActor's copy (0: every copy) is no longer posed by the native commit: frame end poses it from this frame on, and its
// hands are no longer the VR pose's. A word a newer commit wrote meanwhile is kept.
void WithdrawCommitted(const uint32_t aActor) noexcept
{
    for (auto& slot : s_slots)
        for (auto* pWord : {&slot.CommittedBody, &slot.PosedHands})
        {
            uint64_t word = pWord->load(std::memory_order_acquire);
            const uint64_t cWithdrawn = RemoteBodyPoser::Withdrawn(word, aActor);
            if (cWithdrawn != word)
                pWord->compare_exchange_strong(word, cWithdrawn, std::memory_order_acq_rel);
        }
}
void WithdrawDwellLocked(BodySlot& aSlot) noexcept
{
    aSlot.Dwell.Withdraw(SteadyMs(), s_presentFrame.load(std::memory_order_acquire));
}
void WithdrawDwellLocked(uint32_t aActor = 0) noexcept
{
    for (auto& slot : s_slots)
        if (!aActor || aActor == slot.DwellActor)
            WithdrawDwellLocked(slot);
}

// Read only our own process. A failed/torn observation is discarded; this is not synchronization with engine writers.
bool Read(const uintptr_t aAddress, void* apOut, size_t aSize) noexcept
{
    if (!aAddress || aAddress > UINTPTR_MAX - aSize)
        return false;
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(aAddress), apOut, aSize, &read) && read == aSize;
}
template <class T> bool ReadAt(const void* apObject, size_t aOffset, T& aValue) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(apObject);
    return base && base <= UINTPTR_MAX - aOffset && Read(base + aOffset, &aValue, sizeof(T));
}

// Optional SE1.7.104 job correlation. These are value observations, not a writer fence.
// Installed PE SHA256: 846efccf0c1374d71f892907f46549560f2fcb0a75cb87a3eed438baa0f1402f.
// Both joins call EBB1D0 as void(void*). Worker 784240 takes a serial-handle
// address in RCX and a bool in DL; its recursive call at 784350 re-enters this scope.
// The unique 78426D lookup returns bool in AL and fills the native owned actor slot.
using JobWorker = void(uint32_t*, bool);
using JobLookup = bool(uint32_t*, void**);
using JobGet3D = void*(const Actor*);
using JobDispatch = void(void*, const void*, uint32_t);
static_assert(offsetof(TESForm, formID) == 0x14);
JobWorker* s_jobWorker{};
JobLookup* s_jobLookup{};
std::atomic<bool> s_jobReady{};
uint32_t s_imageSize{};
uint64_t BeginWorldDispatch(void* apRoot) noexcept;
void EndWorldDispatch(uint64_t aToken) noexcept;
void ForwardDescentRoot(JobDispatch* apOriginal, void* apReceiver, const void* apData, uint32_t aFlags);
struct WorldPass;
bool AcquireSelectedBinding(WorldPass& aPass) noexcept;
bool FreshSelectedSource(WorldPass& aPass) noexcept;
bool PresentationSelectedSource(WorldPass& aPass) noexcept;
bool CommitPostRoot() noexcept;
void CommitPreRoot() noexcept;

struct JobIdentityValue
{
    uint32_t Actor{};
    uint64_t Generation{}, SourceTick{};
    uintptr_t Tree{}, RootHint{};
};
bool ReadJobIdentity(const JobIdentity& acIdentity, JobIdentityValue& aValue, const void* apTree = nullptr, bool aPresentation = false) noexcept
{
    if (!s_jobReady.load(std::memory_order_relaxed) || !s_activeCount.load(std::memory_order_relaxed))
        return false;
    const auto revision = acIdentity.Revision.load(std::memory_order_acquire);
    if (revision & 1)
        return false;
    aValue = {acIdentity.Actor.load(), acIdentity.Generation.load(), acIdentity.SourceTick.load(), acIdentity.Tree.load(), acIdentity.RootHint.load()};
    const auto deadline = acIdentity.Deadline.load();
    return aValue.Actor && aValue.Tree && (!apTree || aValue.Tree == reinterpret_cast<uintptr_t>(apTree)) && std::isfinite(deadline) &&
           SteadyMs() <= deadline + (aPresentation ? BodyPoseCommit::kPresentationGraceMs : 0.) && acIdentity.Revision.load(std::memory_order_acquire) == revision;
}
// These writers run under the existing registry mutex, never in a native job wrapper.
void PublishJobIdentity(BodySlot& aSlot, uint32_t aActor, uint64_t aGeneration, uint64_t aSourceTick, uintptr_t aTree, uintptr_t aRoot, double aDeadline) noexcept
{
    if (aActor != aSlot.Actor.load())
        return;
    auto& identity = aSlot.Identity;
    identity.Revision.fetch_add(1);
    identity.Actor.store(aActor);
    identity.Generation.store(aGeneration);
    identity.SourceTick.store(aSourceTick);
    identity.Tree.store(aTree);
    identity.RootHint.store(aRoot);
    identity.Deadline.store(aDeadline);
    identity.Revision.fetch_add(1, std::memory_order_release);
}
void DisarmJobIdentity(BodySlot& aSlot) noexcept
{
    auto& identity = aSlot.Identity;
    identity.Revision.fetch_add(1);
    identity.Actor.store(0);
    identity.Tree.store(0);
    identity.RootHint.store(0);
    identity.Deadline.store(0);
    identity.Revision.fetch_add(1, std::memory_order_release);
}
void DisarmJobIdentity(uint32_t aActor) noexcept
{
    for (auto& slot : s_slots)
        if (!aActor || aActor == slot.Identity.Actor.load())
            DisarmJobIdentity(slot);
}

struct JobEvent
{
    uint64_t Sequence{}, WorkerSequence{}, ParentWorkerSequence{}, HelperSequence{}, ParentHelperSequence{}, JoinSequence{};
    uint64_t ActorObservationSequence{}, Generation{}, SourceTick{};
    uint64_t ReturnedRootSequence{}, DispatchRootSequence{};
    uint32_t Frame{}, Thread{}, WorkerDepth{}, HelperDepth{}, JoinList{}, Handle{}, BoundActor{}, OwnedActorForm{};
    uintptr_t HandleAddress{}, List{}, Tree{}, BindingRootHint{}, OwnedActor{}, Caller{};
    uintptr_t ReturnedRoot{}, ReturnedRootActor{}, DispatchRoot{};
    double ObservedSteadyMs{};
    bool HandleReadable{}, LookupObserved{}, OwnedActorObserved{}, WorkerOverflow{}, JoinOverflow{}, Recursive{};
    bool ReturnedRootObserved{}, DispatchRootObserved{}, DispatchActive{}, NativeRetentionPathQualified{};
    uint16_t StackCount{};
    std::array<uintptr_t, 16> Stack{};
};
std::atomic<uint64_t> s_jobSequence{};
struct WorkerContext
{
    JobEvent Entry{};
};
thread_local std::array<WorkerContext, 16> s_jobWorkers{};
thread_local size_t s_jobWorkerDepth{};
static_assert(
    std::atomic<uint64_t>::is_always_lock_free && std::atomic<uintptr_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free &&
    std::atomic<double>::is_always_lock_free);

WorkerContext* CurrentJobWorker() noexcept
{
    // Overflow is deliberately empty: never attribute a deeper worker to an outer actor.
    return s_jobWorkerDepth && s_jobWorkerDepth <= s_jobWorkers.size() ? &s_jobWorkers[s_jobWorkerDepth - 1] : nullptr;
}
JobEvent NewJobEvent() noexcept
{
    JobEvent event{};
    event.Sequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    event.Frame = EngineFrame();
    event.Thread = GetCurrentThreadId();
    event.ObservedSteadyMs = SteadyMs();
    event.WorkerDepth = static_cast<uint32_t>(s_jobWorkerDepth);
    return event;
}

void CopyJobActorObservation(JobEvent& aEvent, const JobEvent& acWorker) noexcept
{
    aEvent.WorkerSequence = acWorker.WorkerSequence;
    aEvent.ParentWorkerSequence = acWorker.ParentWorkerSequence;
    aEvent.HandleAddress = acWorker.HandleAddress;
    aEvent.Handle = acWorker.Handle;
    aEvent.HandleReadable = acWorker.HandleReadable;
    aEvent.LookupObserved = acWorker.LookupObserved;
    aEvent.OwnedActorObserved = acWorker.OwnedActorObserved;
    aEvent.OwnedActor = acWorker.OwnedActor;
    aEvent.OwnedActorForm = acWorker.OwnedActorForm;
    aEvent.ActorObservationSequence = acWorker.ActorObservationSequence;
    aEvent.Recursive = acWorker.Recursive;
    aEvent.ReturnedRoot = acWorker.ReturnedRoot;
    aEvent.ReturnedRootActor = acWorker.ReturnedRootActor;
    aEvent.ReturnedRootSequence = acWorker.ReturnedRootSequence;
    aEvent.ReturnedRootObserved = acWorker.ReturnedRootObserved;
    aEvent.DispatchRoot = acWorker.DispatchRoot;
    aEvent.DispatchRootSequence = acWorker.DispatchRootSequence;
    aEvent.DispatchRootObserved = acWorker.DispatchRootObserved;
    aEvent.DispatchActive = acWorker.DispatchActive;
    aEvent.NativeRetentionPathQualified = acWorker.DispatchActive && acWorker.NativeRetentionPathQualified;
}
void HookJobWorker(uint32_t* apHandle, bool aRecursive)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!s_jobReady.load(std::memory_order_relaxed))
    {
        s_jobWorker(apHandle, aRecursive);
        return;
    }
    const auto* parent = CurrentJobWorker();
    const auto parentSequence = parent ? parent->Entry.WorkerSequence : 0;
    ++s_jobWorkerDepth;
    struct Scope
    {
        ~Scope() { --s_jobWorkerDepth; }
    } scope;
    if (auto* worker = CurrentJobWorker())
    {
        *worker = {};
        worker->Entry = NewJobEvent();
        worker->Entry.WorkerSequence = worker->Entry.Sequence;
        worker->Entry.ParentWorkerSequence = parentSequence;
        worker->Entry.Caller = caller;
        worker->Entry.HandleAddress = reinterpret_cast<uintptr_t>(apHandle);
        worker->Entry.HandleReadable = Read(reinterpret_cast<uintptr_t>(apHandle), &worker->Entry.Handle, sizeof(uint32_t));
        worker->Entry.Recursive = aRecursive;
    }
    s_jobWorker(apHandle, aRecursive); // native exactly once; native actor/root retention and release are untouched
}
bool HookJobLookup(uint32_t* apHandle, void** apOwnedActor)
{
    const bool result = s_jobLookup(apHandle, apOwnedActor); // original AL and output slot unchanged
    if (s_jobReady.load(std::memory_order_relaxed))
        if (auto* worker = CurrentJobWorker(); worker && worker->Entry.HandleAddress == reinterpret_cast<uintptr_t>(apHandle))
        {
            auto& entry = worker->Entry;
            entry.LookupObserved = true;
            entry.ActorObservationSequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            entry.OwnedActor = 0;
            entry.OwnedActorForm = 0;
            entry.OwnedActorObserved = result && Read(reinterpret_cast<uintptr_t>(apOwnedActor), &entry.OwnedActor, sizeof(uintptr_t)) && entry.OwnedActor &&
                                       ReadAt(reinterpret_cast<void*>(entry.OwnedActor), offsetof(TESForm, formID), entry.OwnedActorForm);
        }
    return result;
}
void* HookJobGet3D(const Actor* apActor)
{
    // Original 78435B loads vslot70 from the already-owned actor. No additional
    // Get3D call or retain: forward this one call to its current virtual target.
    const auto* table = *reinterpret_cast<const uintptr_t* const*>(apActor);
    const auto original = reinterpret_cast<JobGet3D*>(table[0x70]);
    void* const root = original(apActor);
    if (s_jobReady.load(std::memory_order_relaxed))
        if (auto* worker = CurrentJobWorker(); worker && worker->Entry.OwnedActorObserved && worker->Entry.OwnedActor == reinterpret_cast<uintptr_t>(apActor))
        {
            auto& entry = worker->Entry;
            // Numeric result only, before the native caller's later 78436E IncRef.
            // Do not read or retain the returned object here.
            entry.ReturnedRoot = reinterpret_cast<uintptr_t>(root);
            entry.ReturnedRootActor = reinterpret_cast<uintptr_t>(apActor);
            entry.ReturnedRootObserved = true;
            entry.ReturnedRootSequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    return root;
}
void HookJobDispatch(void* apRoot, const void* apData, uint32_t aFlags, const uintptr_t* apOriginalR9Table)
{
    // At 6AFF4F R9 is the vtable already loaded by native 6AFF38. The fourth
    // wrapper argument preserves that actual value, so no root dereference is added.
    const auto original = reinterpret_cast<JobDispatch*>(apOriginalR9Table[0x2C]);
    auto* worker = s_jobReady.load(std::memory_order_relaxed) ? CurrentJobWorker() : nullptr;
    JobEvent previous{};
    if (worker)
    {
        previous = worker->Entry;
        auto& entry = worker->Entry;
        entry.DispatchRoot = reinterpret_cast<uintptr_t>(apRoot);
        entry.DispatchRootObserved = true;
        entry.DispatchActive = true;
        entry.DispatchRootSequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        // This qualifies only the statically retained native path during this
        // dispatch. The increment itself, descendant attachment and exclusion
        // against other writers are not directly observed or established.
        entry.NativeRetentionPathQualified =
            entry.OwnedActorObserved && entry.ReturnedRootObserved && entry.ReturnedRoot && entry.ReturnedRootActor == entry.OwnedActor && entry.ReturnedRoot == entry.DispatchRoot;
    }
    struct Scope
    {
        WorkerContext* Worker;
        const JobEvent& Previous;
        ~Scope()
        {
            if (!Worker)
                return;
            auto& entry = Worker->Entry;
            // No completed dispatch identity leaks into a later helper or the
            // worker's post-release exit. Nested dispatches recover their outer scope.
            entry.DispatchRoot = Previous.DispatchRoot;
            entry.DispatchRootSequence = Previous.DispatchRootSequence;
            entry.DispatchRootObserved = Previous.DispatchRootObserved;
            entry.NativeRetentionPathQualified = Previous.NativeRetentionPathQualified;
            entry.DispatchActive = Previous.DispatchActive;
        }
    } scope{worker, previous};
    const auto worldToken = BeginWorldDispatch(apRoot);
    struct WorldScope
    {
        uint64_t Token;
        ~WorldScope() { EndWorldDispatch(Token); }
    } worldScope{worldToken};
    CommitPreRoot();                                      // synchronize rebind state before native traversal
    ForwardDescentRoot(original, apRoot, apData, aFlags); // original RCX/RDX/R8 exactly once; native return is void
    // After the whole native traversal, before native root release (784453). A copy's native update the commit did not
    // follow put its animation pose back: frame end poses it in this same frame.
    if (!CommitPostRoot() && worker && worker->Entry.NativeRetentionPathQualified && worker->Entry.OwnedActorForm && s_jobWorkerDepth == 1 && !worker->Entry.Recursive)
        WithdrawCommitted(worker->Entry.OwnedActorForm);
}
bool EntryReachesHook(uintptr_t aEntry, uintptr_t aHook) noexcept
{
    // MinHook normally uses entry E9 -> nearby FF25 relay -> hook. Witness the
    // actual destination rather than accepting any patched first opcode.
    for (size_t step = 0; step < 3; ++step)
    {
        if (aEntry == aHook)
            return true;
        std::array<uint8_t, 6> bytes{};
        if (!Read(aEntry, bytes.data(), bytes.size()))
            return false;
        int32_t displacement{};
        if (bytes[0] == 0xE9)
        {
            std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
            aEntry = aEntry + 5 + static_cast<intptr_t>(displacement);
        }
        else if (bytes[0] == 0xFF && bytes[1] == 0x25)
        {
            std::memcpy(&displacement, bytes.data() + 2, sizeof(displacement));
            if (!Read(aEntry + 6 + static_cast<intptr_t>(displacement), &aEntry, sizeof(aEntry)))
                return false;
        }
        else
            return false;
    }
    return aEntry == aHook;
}

// A five-byte call at apSite reaches aTarget.
bool CallReaches(const uint8_t* apSite, const uintptr_t aTarget) noexcept
{
    const auto cDisplacement = static_cast<int64_t>(aTarget) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(apSite) + 5);
    return cDisplacement >= INT32_MIN && cDisplacement <= INT32_MAX;
}

bool WriteJobCall(uint8_t* apSite, size_t aWidth, uintptr_t aTarget) noexcept
{
    // Startup only. Protect/flush the entire original instruction, including
    // padding for the six-byte Get3D and seven-byte dispatch replacements.
    if (aWidth < 5 || aWidth > 7)
        return false;
    const auto displacement = static_cast<int64_t>(aTarget) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(apSite) + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
        return false;
    std::array<uint8_t, 7> bytes{};
    bytes.fill(0x90);
    bytes[0] = 0xE8;
    const auto relative = static_cast<int32_t>(displacement);
    std::memcpy(bytes.data() + 1, &relative, sizeof(relative));
    DWORD previous{};
    if (!VirtualProtect(apSite, aWidth, PAGE_EXECUTE_READWRITE, &previous))
        return false;
    std::memcpy(apSite, bytes.data(), aWidth);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), apSite, aWidth) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtect(apSite, aWidth, previous, &ignored) != FALSE;
    return flushed && restored;
}
bool JobCallMatches(const uint8_t* apSite, size_t aWidth, uintptr_t aTarget) noexcept
{
    std::array<uint8_t, 7> bytes{};
    if (aWidth < 5 || aWidth > bytes.size() || !Read(reinterpret_cast<uintptr_t>(apSite), bytes.data(), aWidth) || bytes[0] != 0xE8)
        return false;
    int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    if (reinterpret_cast<uintptr_t>(apSite) + 5 + static_cast<intptr_t>(displacement) != aTarget)
        return false;
    for (size_t i = 5; i < aWidth; ++i)
        if (bytes[i] != 0x90)
            return false;
    return true;
}

bool TypeName(uint32_t aTypeRva, const char* acpName) noexcept
{
    char name[96]{};
    return Read(s_imageBase + aTypeRva + 0x10, name, sizeof(name)) && std::memchr(name, 0, sizeof(name)) && std::strcmp(name, acpName) == 0;
}

// MSVC x64 COL/self-RVA and bounded base hierarchy; no unverified engine AsNode virtual call.
bool HasType(const void* apObject, const char* acpName) noexcept
{
    uintptr_t vtable{}, col{};
    std::array<uint32_t, 6> locator{};
    if (!ReadAt(apObject, 0, vtable) || vtable < sizeof(uintptr_t) || !Read(vtable - sizeof(uintptr_t), &col, sizeof(col)) || !Read(col, locator.data(), sizeof(locator)) ||
        locator[0] != 1 || locator[1] != 0 || col < locator[5] || col - locator[5] != s_imageBase)
        return false;
    std::array<uint32_t, 4> hierarchy{};
    if (!Read(s_imageBase + locator[4], hierarchy.data(), sizeof(hierarchy)) || !hierarchy[2] || hierarchy[2] > 64)
        return false;
    for (uint32_t i = 0; i < hierarchy[2]; ++i)
    {
        uint32_t descriptor{}, type{};
        if (!Read(s_imageBase + hierarchy[3] + i * sizeof(uint32_t), &descriptor, sizeof(descriptor)) || !Read(s_imageBase + descriptor, &type, sizeof(type)))
            return false;
        if (TypeName(type, acpName))
            return true;
    }
    return false;
}

bool IsTree(const void* apObject) noexcept
{
    uintptr_t vtable{};
    return ReadAt(apObject, 0, vtable) && vtable == s_imageBase + kTreeVtableRva;
}

template <class F> void Walk(void* apRoot, F&& aVisit)
{
    struct Pending
    {
        void* Node{};
        size_t Depth{};
    };
    std::array<Pending, kMaxGraphNodes> pending{};
    std::array<void*, kMaxGraphNodes> visited{};
    size_t queued = 1, seen = 0;
    pending[0] = {apRoot, 0};
    while (queued && seen < visited.size())
    {
        const auto item = pending[--queued];
        if (!item.Node || std::find(visited.begin(), visited.begin() + seen, item.Node) != visited.begin() + seen)
            continue;
        visited[seen++] = item.Node;
        if (!aVisit(item.Node))
            return;
        if (item.Depth >= kMaxDepth || !HasType(item.Node, ".?AVNiNode@@"))
            continue;
        void** children{};
        uint16_t count{};
        // +0x122 is the populated bound. +0x120 is allocation, and must not be used for this traversal.
        if (!ReadAt(item.Node, 0x118, children) || !ReadAt(item.Node, 0x122, count) || count > kMaxGraphNodes || !children)
            continue;
        for (uint16_t i = 0; i < count && queued < pending.size(); ++i)
        {
            void* child{};
            if (ReadAt(children, i * sizeof(void*), child) && child)
                pending[queued++] = {child, item.Depth + 1};
        }
    }
}

bool UnderRoot(void* apTree, void* apRoot) noexcept
{
    std::array<void*, kMaxDepth> seen{};
    for (size_t i = 0; apTree && i < seen.size(); ++i)
    {
        if (apTree == apRoot)
            return true;
        if (std::find(seen.begin(), seen.begin() + i, apTree) != seen.begin() + i)
            return false;
        seen[i] = apTree;
        if (!ReadAt(apTree, 0x30, apTree))
            return false;
    }
    return false;
}

int WireIndex(const void* apNode) noexcept
{
    uintptr_t name{};
    char text[64]{};
    if (!ReadAt(apNode, 0x10, name) || !name || !Read(name, text, sizeof(text)) || !std::memchr(text, 0, sizeof(text)))
        return -1;
    for (size_t i = 0; i < kBoneNames.size(); ++i)
        if (std::strcmp(text, kBoneNames[i]) == 0)
            return static_cast<int>(i);
    return -1;
}

// Native world inputs, read-only. One admission covers the WHOLE synchronous
// selected root pass; there is no per-bone throttle. A retained root is not a
// descendant/topology lease, and these copied observations do not imply coherence.
using WorldCompose = void*(const void*, void*, const void*);
constexpr size_t kWorldPassDepth = 8, kWorldTreeDepth = 8, kWorldInputsPerPass = 512;
constexpr uint32_t kWorldCallsPerPass = 2048;
constexpr uint32_t kWorldPassLimit = 32;
constexpr size_t kCommitDescentNodes = 128; // native descent world receivers recorded per admitted commit pass
struct CollisionBodyChain
{
    uintptr_t Collision{}, Target{}, Body{}, Table{}, Getter{}, LeafRTTI{}, RTTIParent{};
    uintptr_t Hkp{}, HkpWorld{}, MotionTable{};
    uint32_t CollisionFlagsStorage{};
    uint16_t CollisionFlagsNativeLow{};
    uint8_t Motion{};
    bool ScopeMatched{}, CollisionReadable{}, TargetReadable{}, BodyReadable{}, TableReadable{}, GetterReadable{}, ParentReadable{};
    bool Reciprocal{}, KnownBodyTable{}, LeafBytesMatch{}, LeafReturnMatches{}, BodyGetterLeafObserved{};
    bool FlagsStorageReadable{}, FlagsNativeLowReadable{}, HkpReadable{}, MotionReadable{}, HkpWorldReadable{}, MotionTableReadable{};
    bool SkipPredicateReadable{}, SkipPredicateValue{}, KeyframedIdentityObserved{};
    float BlendWeight{};
    uint32_t CachedMode{};
    uintptr_t CachedWorld{}, CollisionTable{};
    bool BlendReadable{}, CachedModeReadable{}, CachedWorldReadable{}, CollisionTableReadable{};
};
CollisionBodyChain ReadCollisionBodyChain(uintptr_t aNode, bool aScopeMatched) noexcept
{
    CollisionBodyChain result{};
    result.ScopeMatched = aScopeMatched;
    if (!aScopeMatched)
        return result;
    result.CollisionReadable = ReadAt(reinterpret_cast<void*>(aNode), 0x40, result.Collision);
    if (!result.CollisionReadable || !result.Collision)
        return result;
    result.BlendReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x28, result.BlendWeight) && std::isfinite(result.BlendWeight);
    if (!result.BlendReadable)
        result.BlendWeight = 0.f;
    result.CachedModeReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x30, result.CachedMode);
    result.CachedWorldReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x38, result.CachedWorld);
    result.CollisionTableReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0, result.CollisionTable);
    result.TargetReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x10, result.Target);
    // CommonLib bhkNiCollisionObject declares uint32 storage at +18; the pinned
    // native blend path uses MOVZX/AND word operations on its low 16 bits.
    result.FlagsStorageReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x18, result.CollisionFlagsStorage);
    result.FlagsNativeLowReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x18, result.CollisionFlagsNativeLow);
    result.Reciprocal = result.TargetReadable && result.Target == aNode;
    result.BodyReadable = ReadAt(reinterpret_cast<void*>(result.Collision), 0x20, result.Body);
    if (!result.BodyReadable || !result.Body)
        return result;
    result.HkpReadable = ReadAt(reinterpret_cast<void*>(result.Body), 0x10, result.Hkp);
    if (result.HkpReadable && result.Hkp)
    {
        result.MotionReadable = ReadAt(reinterpret_cast<void*>(result.Hkp), 0x160, result.Motion);
        result.HkpWorldReadable = ReadAt(reinterpret_cast<void*>(result.Hkp), 0x10, result.HkpWorld);
        result.MotionTableReadable = ReadAt(reinterpret_cast<void*>(result.Hkp), 0x150, result.MotionTable);
    }
    result.TableReadable = ReadAt(reinterpret_cast<void*>(result.Body), 0, result.Table);
    if (!result.TableReadable || !result.Table)
        return result;
    result.GetterReadable = ReadAt(reinterpret_cast<void*>(result.Table), 0x10, result.Getter);
    constexpr std::array<uint8_t, 8> rigidBytes{0x48, 0x8D, 0x05, 0x51, 0xD5, 0x03, 0x03, 0xC3};
    constexpr std::array<uint8_t, 8> rigidTBytes{0x48, 0x8D, 0x05, 0xF1, 0x5D, 0x2B, 0x02, 0xC3};
    std::array<uint8_t, 8> bytes{};
    const bool rigid = result.Table == s_imageBase + 0x1828198 && result.Getter == s_imageBase + 0x2FA120;
    const bool rigidT = result.Table == s_imageBase + 0x1A82EF8 && result.Getter == s_imageBase + 0x1081A00;
    result.KnownBodyTable = rigid || rigidT;
    if (!result.GetterReadable || !result.KnownBodyTable || !Read(result.Getter, bytes.data(), bytes.size()))
        return result;
    result.LeafBytesMatch = bytes == (rigid ? rigidBytes : rigidTBytes);
    if (!result.LeafBytesMatch)
        return result;
    int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
    // This is evaluation of the observed LEA/RET bytes, not an engine call.
    result.LeafRTTI = result.Getter + 7 + static_cast<intptr_t>(displacement);
    result.ParentReadable = ReadAt(reinterpret_cast<void*>(result.LeafRTTI), 8, result.RTTIParent);
    result.LeafReturnMatches =
        rigid ? result.LeafRTTI == s_imageBase + 0x3337678 : result.LeafRTTI == s_imageBase + 0x33377F8 && result.ParentReadable && result.RTTIParent == s_imageBase + 0x3337678;
    result.BodyGetterLeafObserved = result.Reciprocal && result.LeafReturnMatches;
    result.SkipPredicateReadable = result.BodyGetterLeafObserved && result.HkpReadable && (!result.Hkp || (result.MotionReadable && result.HkpWorldReadable));
    result.SkipPredicateValue = result.SkipPredicateReadable && (!result.Hkp || (result.Motion == 5 && result.HkpWorld));
    result.KeyframedIdentityObserved =
        result.BodyGetterLeafObserved && result.MotionReadable && result.Motion == 4 && result.MotionTableReadable && result.MotionTable == s_imageBase + 0x19EB528;
    return result;
}
using BodyTransform = BodyPoseWorldInput::Transform;
static_assert(sizeof(BodyTransform) == 52);
bool ReadWorldValue(uintptr_t aAddress, std::array<float, 13>& aValue) noexcept;
// Closed leaf exception: unchanged native numeric/visibility controllers, not
// an arbitrary NiTimeController allowance. No method or interpolator is invoked.
struct WorldPass
{
    JobEvent Identity{};
    uint64_t PassSequence{}, IdentityRevision{};
    size_t Slot{}; // the slot of the pass's copy (s_slots)
    uint32_t TreeScopes{}, TreeArrayEndpointMismatches{};
    bool Active{}, IdentityStable = true;
    bool ObservationOnly{};
    bool PresentationIdentity{}, GraceHold{}, FreshSourceAtAdmission{}, GraceFlipHeld{};
    BodyPoseCommit::FlipPose GracePose{};
    BodyPoseCommit::FlipDecision GraceGuardDecision{};
    double GraceGuardAssessedMs{};
    uint64_t GraceSourceCommit{};
    bool CommitTreeQualified{}, CommitTreeRejected{};
    uintptr_t CommitTreeEntries{};
    uint32_t CommitTreeCount{};
    bool CommitWatch{}, CommitRootTicket{}; // only: per-frame admission and the qualified normal root descent ticket
    uint32_t CommitDescentCalls{};
    std::array<uintptr_t, kCommitDescentNodes> CommitDescentNodes{};
    std::shared_ptr<const Binding> SourceBinding;
    bool DwellBlocked{};
    BodyPoseCommit::DwellObservation Dwell{};
    std::array<float, 13> RootPostWorldValue{};
    uint64_t RootPostWorldSequence{};
    uint32_t RootPostWorldFrame{};
    double RootPostWorldMs{};
    bool RootPostWorldObserved{}, RootPostWorldReadable{};
};
struct WorldTree
{
    uint64_t PassSequence{};
    uintptr_t Tree{}, Entries{};
    uint32_t Count{};
    bool Valid{};
};
thread_local std::array<WorldPass, kWorldPassDepth> s_worldPasses{};
thread_local std::array<WorldTree, kWorldTreeDepth> s_worldTrees{};
thread_local size_t s_worldDepth{}, s_worldTreeDepth{};
WorldPass* CurrentWorldPass() noexcept
{
    return s_worldDepth && s_worldDepth <= s_worldPasses.size() && s_worldPasses[s_worldDepth - 1].Active ? &s_worldPasses[s_worldDepth - 1] : nullptr;
}
// Caller holds s_mutex. This gate confers presentation admission only, never source freshness or engine lifetime.
bool DwellAdmissionLocked(WorldPass& aPass, bool aFreshEligible, bool aCommittedHoldEligible) noexcept
{
    if (!aPass.CommitWatch)
        return true;
    auto& dwell = s_slots[aPass.Slot].Dwell;
    const bool allowed = dwell.Allows(SteadyMs(), s_presentFrame.load(std::memory_order_acquire), aFreshEligible, aCommittedHoldEligible);
    aPass.Dwell = dwell.Snapshot();
    aPass.DwellBlocked = !allowed;
    return allowed;
}
bool WorldIdentityMatches(WorldPass& aPass) noexcept
{
    // Once invalidated, an equal-looking tuple cannot revive this pass. The
    // revision also catches cancellation/rearm ABA and benign republication.
    if (!aPass.IdentityStable)
        return false;
    JobIdentityValue current{};
    const auto* worker = CurrentJobWorker();
    const auto& saved = aPass.Identity;
    if (aPass.ObservationOnly)
    {
        uint32_t serial{};
        aPass.IdentityStable = worker && worker->Entry.WorkerSequence == saved.WorkerSequence && worker->Entry.DispatchActive &&
                               worker->Entry.DispatchRootSequence == saved.DispatchRootSequence && worker->Entry.OwnedActorObserved &&
                               worker->Entry.OwnedActorForm == saved.BoundActor && worker->Entry.ReturnedRoot == saved.BindingRootHint &&
                               worker->Entry.DispatchRoot == saved.BindingRootHint && worker->Entry.HandleReadable && worker->Entry.Handle == saved.Handle &&
                               Read(worker->Entry.HandleAddress, &serial, sizeof(serial)) && serial == saved.Handle; // pass identity is revision + worker/dispatch sequence
        return aPass.IdentityStable;                                                                                 // observation only; never exposes an expired pose for RHS use
    }
    const auto& identity = s_slots[aPass.Slot].Identity;
    const bool valid = identity.Revision.load(std::memory_order_acquire) == aPass.IdentityRevision && !(aPass.IdentityRevision & 1) &&
                       ReadJobIdentity(identity, current, nullptr, aPass.PresentationIdentity) && worker && worker->Entry.WorkerSequence == saved.WorkerSequence &&
                       worker->Entry.DispatchActive && worker->Entry.DispatchRootSequence == saved.DispatchRootSequence && worker->Entry.OwnedActorObserved &&
                       worker->Entry.OwnedActorForm == saved.BoundActor && worker->Entry.ReturnedRoot == saved.BindingRootHint &&
                       worker->Entry.DispatchRoot == saved.BindingRootHint && current.Actor == saved.BoundActor && current.Generation == saved.Generation &&
                       current.SourceTick == saved.SourceTick && current.Tree == saved.Tree && current.RootHint == saved.BindingRootHint &&
                       identity.Revision.load(std::memory_order_acquire) == aPass.IdentityRevision; // a list-9 pass may straddle a frame tick
    aPass.IdentityStable = valid;
    return aPass.IdentityStable;
}
bool ReadWorldValue(uintptr_t aAddress, std::array<float, 13>& aValue) noexcept
{
    const bool valid = Read(aAddress, aValue.data(), sizeof(aValue)) && std::all_of(aValue.begin(), aValue.end(), [](float aPart) { return std::isfinite(aPart); });
    if (!valid)
        aValue.fill(0.f);
    return valid;
}
uint64_t BeginWorldDispatch(void* apRoot) noexcept
{
    if (!s_commitPassReady.load(std::memory_order_relaxed))
        return 0;
    ++s_worldDepth;
    if (s_worldDepth > s_worldPasses.size())
        return s_worldDepth; // empty identity while overflowing
    auto& pass = s_worldPasses[s_worldDepth - 1];
    pass.SourceBinding.reset();
    pass.Active = false; // unrelated/nested dispatches cannot inherit the outer identity
    JobIdentityValue identity{};
    const auto* worker = CurrentJobWorker();
    uint32_t invalidHandle{}, currentHandle{};
    // This admission is the tested normal list-9 caller, not an arbitrary worker
    // with a numerically similar tree. Keep the native retained-root qualification.
    if (!worker || worker->Entry.Caller != s_imageBase + 0x7893F4 || !worker->Entry.HandleReadable || !worker->Entry.Handle || worker->Entry.Handle == UINT32_MAX ||
        !worker->Entry.LookupObserved || !worker->Entry.ActorObservationSequence || !Read(s_imageBase + 0x219E55C, &invalidHandle, sizeof(invalidHandle)) ||
        worker->Entry.Handle == invalidHandle || !Read(worker->Entry.HandleAddress, &currentHandle, sizeof(currentHandle)) || currentHandle != worker->Entry.Handle ||
        !worker->Entry.DispatchActive || !worker->Entry.NativeRetentionPathQualified || !worker->Entry.OwnedActorObserved || !worker->Entry.ReturnedRoot ||
        worker->Entry.ReturnedRoot != reinterpret_cast<uintptr_t>(apRoot) || worker->Entry.DispatchRoot != reinterpret_cast<uintptr_t>(apRoot))
        return s_worldDepth;
    // Only a copy that holds a slot has an identity to match.
    const size_t cSlot = SlotOf(worker->Entry.OwnedActorForm);
    if (cSlot == kNoSlot)
        return s_worldDepth;
    const auto& slotIdentity = s_slots[cSlot].Identity;
    const auto identityRevision = slotIdentity.Revision.load(std::memory_order_acquire);
    const bool fresh = !(identityRevision & 1) && ReadJobIdentity(slotIdentity, identity) && worker->Entry.OwnedActorForm == identity.Actor &&
                       identity.RootHint == reinterpret_cast<uintptr_t>(apRoot) && UnderRoot(reinterpret_cast<void*>(identity.Tree), apRoot) &&
                       slotIdentity.Revision.load(std::memory_order_acquire) == identityRevision;
    const bool commitWatch = s_jobWorkerDepth == 1 && !worker->Entry.Recursive;
    // Allow an expired source to qualify for a separate committed-pose grace check.
    const bool presentation = !fresh && commitWatch && !(identityRevision & 1) && ReadJobIdentity(slotIdentity, identity, nullptr, true) &&
                              worker->Entry.OwnedActorForm == identity.Actor && identity.RootHint == reinterpret_cast<uintptr_t>(apRoot) &&
                              UnderRoot(reinterpret_cast<void*>(identity.Tree), apRoot) && slotIdentity.Revision.load(std::memory_order_acquire) == identityRevision;
    if (!fresh && !commitWatch)
        return s_worldDepth;
    pass = {};
    pass.Slot = cSlot;
    pass.IdentityStable = true;
    pass.IdentityRevision = identityRevision;
    pass.Active = true;
    pass.CommitWatch = commitWatch;
    pass.PresentationIdentity = commitWatch;
    pass.ObservationOnly = !fresh && !presentation;
    pass.Identity = NewJobEvent();
    CopyJobActorObservation(pass.Identity, worker->Entry);
    pass.Identity.BoundActor = worker->Entry.OwnedActorForm;
    pass.Identity.Generation = fresh || presentation ? identity.Generation : 0;
    pass.Identity.SourceTick = fresh || presentation ? identity.SourceTick : 0;
    pass.Identity.Tree = fresh || presentation ? identity.Tree : 0;
    pass.Identity.BindingRootHint = reinterpret_cast<uintptr_t>(apRoot);
    pass.PassSequence = pass.Identity.Sequence;
    WorldIdentityMatches(pass);
    if (commitWatch && (fresh || presentation))
        AcquireSelectedBinding(pass); // immutable value pin; grace is commit-only
    return s_worldDepth;
}
void EndWorldDispatch(uint64_t aToken) noexcept
{
    if (!aToken)
        return;
    if (aToken != s_worldDepth)
    {
        for (auto& pass : s_worldPasses)
        {
            pass.Active = false;
            pass.SourceBinding.reset();
        }
        s_worldDepth = 0; // fail closed; an inconsistent nested scope never inherits an outer pass
        return;
    }
    if (auto* pass = CurrentWorldPass())
    {
        WorldIdentityMatches(*pass);
        pass->SourceBinding.reset();
    }
    --s_worldDepth;
}
struct WorldTreeScope
{
    WorldTreeScope(void* apTree) noexcept
    {
        ++s_worldTreeDepth;
        if (s_worldTreeDepth > s_worldTrees.size())
            return;
        auto& context = s_worldTrees[s_worldTreeDepth - 1];
        context = {};
        auto* pass = CurrentWorldPass();
        if (!pass || !WorldIdentityMatches(*pass) || reinterpret_cast<uintptr_t>(apTree) != pass->Identity.Tree || !IsTree(apTree))
            return;
        context.Tree = reinterpret_cast<uintptr_t>(apTree);
        context.PassSequence = pass->PassSequence;
        context.Valid = ReadAt(apTree, 0x128, context.Count) && context.Count && context.Count <= kMaxTreeBones && ReadAt(apTree, 0x130, context.Entries) && context.Entries &&
                        context.Entries <= UINTPTR_MAX - static_cast<uintptr_t>(context.Count) * 0x80;
        ++pass->TreeScopes;
        if (pass->CommitWatch)
        {
            pass->CommitTreeQualified = pass->CommitTreeQualified || context.Valid;
            pass->CommitTreeRejected =
                pass->CommitTreeRejected || !context.Valid || (pass->CommitTreeEntries && (pass->CommitTreeEntries != context.Entries || pass->CommitTreeCount != context.Count));
            pass->CommitTreeEntries = context.Entries;
            pass->CommitTreeCount = context.Count;
        }
    }
    ~WorldTreeScope()
    {
        if (auto* pass = CurrentWorldPass(); pass && s_worldTreeDepth && s_worldTreeDepth <= s_worldTrees.size())
        {
            const auto& context = s_worldTrees[s_worldTreeDepth - 1];
            uintptr_t entries{};
            uint32_t count{};
            if (context.Valid && context.PassSequence == pass->PassSequence &&
                (!ReadAt(reinterpret_cast<void*>(context.Tree), 0x128, count) || count != context.Count || !ReadAt(reinterpret_cast<void*>(context.Tree), 0x130, entries) ||
                 entries != context.Entries))
            {
                ++pass->TreeArrayEndpointMismatches;
                if (pass->CommitWatch)
                    pass->CommitTreeRejected = true;
            }
        }
        --s_worldTreeDepth;
    }
};

// The normal-entry fallback uses a one-shot receiver/data/flags ticket, not a
// generated thunk. Every native entry and forwarding scope shadows prior state.
// A same-receiver controller re-entry has no ticket and is therefore inactive.
using DescentCall = void(void*, const void*, uint32_t);
using DescentWorldCall = void(void*, const void*);
DescentCall* s_descentOriginal{};
std::atomic<bool> s_descentReady{};
constexpr size_t kDescentDepth = 128;
struct DescentContext
{
    uint64_t PassSequence{}, Sequence{}, ParentSequence{};
    uintptr_t Receiver{}, Parent{}, Data{}, Target{}, EntryCaller{};
    uint32_t Flags{}, Depth{}, FixtureSlot{};
    bool Active{};
};
struct DescentTicket : DescentContext
{
};
thread_local DescentTicket s_descentExpected{};
thread_local std::array<DescentContext, kDescentDepth> s_descentNodes{}, s_descentWorlds{};
thread_local size_t s_descentNodeDepth{}, s_descentWorldDepth{};
DescentContext* CurrentDescentNode() noexcept
{
    return s_descentNodeDepth && s_descentNodeDepth <= s_descentNodes.size() && s_descentNodes[s_descentNodeDepth - 1].Active ? &s_descentNodes[s_descentNodeDepth - 1] : nullptr;
}
DescentContext* CurrentDescentWorld() noexcept
{
    return s_descentWorldDepth && s_descentWorldDepth <= s_descentWorlds.size() && s_descentWorlds[s_descentWorldDepth - 1].Active ? &s_descentWorlds[s_descentWorldDepth - 1]
                                                                                                                                   : nullptr;
}
struct DescentNodeShadow
{
    DescentNodeShadow() noexcept
    {
        ++s_descentNodeDepth;
        if (s_descentNodeDepth <= s_descentNodes.size())
            s_descentNodes[s_descentNodeDepth - 1] = {};
    }
    ~DescentNodeShadow() { --s_descentNodeDepth; }
};
struct DescentWorldShadow
{
    DescentWorldShadow() noexcept
    {
        ++s_descentWorldDepth;
        if (s_descentWorldDepth <= s_descentWorlds.size())
            s_descentWorlds[s_descentWorldDepth - 1] = {};
    }
    ~DescentWorldShadow() { --s_descentWorldDepth; }
};
bool KnownDescentTarget(uintptr_t aTarget) noexcept
{
    return aTarget == s_imageBase + 0xEE24D0 || aTarget == s_imageBase + 0xEF4D10;
}
void ForwardDescentTicket(DescentCall* apOriginal, void* apReceiver, const void* apData, uint32_t aFlags, DescentTicket aTicket)
{
    DescentNodeShadow nodeShadow;
    DescentWorldShadow worldShadow;
    struct ClearTicket
    {
        ~ClearTicket() { s_descentExpected = {}; }
    } clear;
    s_descentExpected = {};
    if (s_descentNodeDepth <= s_descentNodes.size() && s_descentWorldDepth <= s_descentWorlds.size())
        s_descentExpected = aTicket;
    // No engine call occurs between ticket publication and the original target.
    apOriginal(apReceiver, apData, aFlags);
}
void ForwardDescentRoot(JobDispatch* apOriginal, void* apReceiver, const void* apData, uint32_t aFlags)
{
    if (!s_descentReady.load(std::memory_order_relaxed))
    {
        apOriginal(apReceiver, apData, aFlags);
        return;
    }
    DescentTicket ticket{};
    auto* pass = CurrentWorldPass();
    const auto* worker = CurrentJobWorker();
    uint32_t updateFlags{}, serial{};
    const auto target = reinterpret_cast<uintptr_t>(apOriginal);
    if (pass && worker && !s_descentNodeDepth && !s_descentWorldDepth && s_jobWorkerDepth == 1 && worker->Entry.WorkerDepth == 1 && !worker->Entry.Recursive &&
        worker->Entry.Caller == s_imageBase + 0x7893F4 && worker->Entry.HandleReadable && worker->Entry.Handle && worker->Entry.LookupObserved &&
        worker->Entry.OwnedActorObserved && Read(worker->Entry.HandleAddress, &serial, sizeof(serial)) && serial == worker->Entry.Handle && ReadAt(apData, 4, updateFlags) &&
        updateFlags == 0x2001 && WorldIdentityMatches(*pass) && reinterpret_cast<uintptr_t>(apReceiver) == pass->Identity.BindingRootHint && KnownDescentTarget(target))
    {
        ticket.Active = true;
        ticket.PassSequence = pass->PassSequence;
        ticket.Sequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        ticket.Receiver = reinterpret_cast<uintptr_t>(apReceiver);
        ReadAt(apReceiver, 0x30, ticket.Parent); // root's borrowed native parent, not a child-edge proof
        ticket.Data = reinterpret_cast<uintptr_t>(apData);
        ticket.Target = target;
        ticket.Flags = aFlags;
        ticket.Depth = 1;
        if (pass->CommitWatch)
            pass->CommitRootTicket = true; // qualified normal 0x2001 root descent
    }
    ForwardDescentTicket(apOriginal, apReceiver, apData, aFlags, ticket);
}
void HookDescentEntry(void* apReceiver, const void* apData, uint32_t aFlags)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!s_descentReady.load(std::memory_order_relaxed))
    {
        s_descentOriginal(apReceiver, apData, aFlags);
        return;
    }
    const auto ticket = s_descentExpected;
    s_descentExpected = {}; // consumed once, before controllers or any other native work
    DescentNodeShadow nodeScope;
    DescentWorldShadow worldShadow;
    auto* pass = CurrentWorldPass();
    const bool route = ticket.Target == s_imageBase + 0xEE24D0 || (ticket.Target == s_imageBase + 0xEF4D10 && caller == s_imageBase + 0xEF4D1E);
    if (ticket.Active && route && pass && ticket.PassSequence == pass->PassSequence && WorldIdentityMatches(*pass) && ticket.Receiver == reinterpret_cast<uintptr_t>(apReceiver) &&
        ticket.Data == reinterpret_cast<uintptr_t>(apData) && ticket.Flags == aFlags && s_descentNodeDepth <= s_descentNodes.size())
    {
        auto& context = s_descentNodes[s_descentNodeDepth - 1];
        context = static_cast<const DescentContext&>(ticket);
        context.EntryCaller = caller;
    }
    s_descentOriginal(apReceiver, apData, aFlags);
}
void ForwardDescentChild(void* apReceiver, const void* apData, uint32_t aFlags, uintptr_t aCaller)
{
    const auto* table = *reinterpret_cast<const uintptr_t* const*>(apReceiver);
    const auto original = reinterpret_cast<DescentCall*>(table[0x2C]);
    if (!s_descentReady.load(std::memory_order_relaxed))
    {
        original(apReceiver, apData, aFlags);
        return;
    }
    DescentTicket ticket{};
    const auto* parent = CurrentDescentNode();
    auto* pass = CurrentWorldPass();
    uintptr_t parentPointer{};
    if (parent && pass && parent->PassSequence == pass->PassSequence && WorldIdentityMatches(*pass) && (aCaller == s_imageBase + 0xEE2591 || aCaller == s_imageBase + 0xEE2621) &&
        parent->Data == reinterpret_cast<uintptr_t>(apData) && ReadAt(apReceiver, 0x30, parentPointer) && parentPointer == parent->Receiver &&
        KnownDescentTarget(reinterpret_cast<uintptr_t>(original)))
    {
        ticket.Active = true;
        ticket.PassSequence = pass->PassSequence;
        ticket.Sequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        ticket.ParentSequence = parent->Sequence;
        ticket.Receiver = reinterpret_cast<uintptr_t>(apReceiver);
        ticket.Parent = parent->Receiver;
        ticket.Data = reinterpret_cast<uintptr_t>(apData);
        ticket.Target = reinterpret_cast<uintptr_t>(original);
        ticket.Flags = aFlags;
        ticket.Depth = parent->Depth + 1;
    }
    ForwardDescentTicket(original, apReceiver, apData, aFlags, ticket);
}
void HookDescentChildA(void* apReceiver, const void* apData, uint32_t aFlags)
{
    ForwardDescentChild(apReceiver, apData, aFlags, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
void HookDescentChildB(void* apReceiver, const void* apData, uint32_t aFlags)
{
    ForwardDescentChild(apReceiver, apData, aFlags, reinterpret_cast<uintptr_t>(_ReturnAddress()));
}
void HookDescentWorld(void* apReceiver, const void* apData)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto* table = *reinterpret_cast<const uintptr_t* const*>(apReceiver);
    const auto original = reinterpret_cast<DescentWorldCall*>(table[0x30]);
    if (!s_descentReady.load(std::memory_order_relaxed))
    {
        original(apReceiver, apData);
        return;
    }
    const auto* node = CurrentDescentNode();
    const DescentContext saved = node ? *node : DescentContext{};
    DescentWorldShadow worldScope;
    auto* pass = CurrentWorldPass();
    if (node && pass && WorldIdentityMatches(*pass) && saved.PassSequence == pass->PassSequence && caller == s_imageBase + 0xEE252B &&
        saved.Receiver == reinterpret_cast<uintptr_t>(apReceiver) && saved.Data == reinterpret_cast<uintptr_t>(apData) &&
        reinterpret_cast<uintptr_t>(original) == s_imageBase + 0xEEBF40 && s_descentWorldDepth <= s_descentWorlds.size())
    {
        auto& context = s_descentWorlds[s_descentWorldDepth - 1];
        context = saved;
        context.Target = reinterpret_cast<uintptr_t>(original);
        context.Sequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        context.ParentSequence = saved.Sequence; // exact consuming native node activation
    }
    original(apReceiver, apData);
    const auto* current = CurrentDescentWorld();
    if (current && pass && current->PassSequence == pass->PassSequence && current->Receiver == pass->Identity.BindingRootHint && WorldIdentityMatches(*pass))
    {
        pass->RootPostWorldObserved = true;
        pass->RootPostWorldReadable = ReadWorldValue(current->Receiver + 0x7C, pass->RootPostWorldValue);
        pass->RootPostWorldSequence = current->Sequence;
        pass->RootPostWorldFrame = EngineFrame();
        pass->RootPostWorldMs = SteadyMs();
    }
    if (current && pass && pass->CommitWatch && current->PassSequence == pass->PassSequence)
    {
        // coverage only: every node world the qualified native descent composed in this pass.
        if (pass->CommitDescentCalls < pass->CommitDescentNodes.size())
            pass->CommitDescentNodes[pass->CommitDescentCalls] = current->Receiver;
        ++pass->CommitDescentCalls;
    }
}

bool VerifyFallbackCallerSites() noexcept
{
    constexpr std::array<uintptr_t, 4> sites{0x108D412, 0x108CEA8, 0x108CFA6, 0x105D13C};
    if (!PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEBFBF), {0x48, 0x8B, 0x4B, 0x40, 0x48, 0x85, 0xC9, 0x74, 0x0E, 0x48, 0x8B, 0x01,
                                                                             0xFF, 0x90, 0x30, 0x01, 0x00, 0x00, 0xE9, 0x4C, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x108D310), {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x108CDD0), {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48,
                                                                              0x8D, 0x6C, 0x24, 0xA0, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF9}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x105D0E0), {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                                                              0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x79, 0x20, 0x48, 0x8B, 0xF1}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x105D12C), {0x48, 0x8B, 0x7E, 0x10, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0xA4, 0x01, 0x00,
                                                                              0x00, 0x48, 0x8B, 0xCF, 0xE8, 0x5F, 0xF0, 0xE8, 0xFF, 0x48, 0x85, 0xDB}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x105D14A), {0x48, 0x8B, 0x43, 0x10, 0x48, 0x85, 0xC0, 0x74, 0x0D, 0x0F, 0xB6, 0x80, 0x60, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0x108D3AE), {0x48, 0x8B, 0xCB, 0xE8, 0x2A, 0xFD, 0xFC, 0xFF, 0xB8, 0xBF, 0xFF, 0x00, 0x00, 0x66, 0x21, 0x43, 0x18}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + sites[0]), {0xE8, 0x89, 0xED, 0xE5, 0xFF}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + sites[1]), {0xE8, 0xF3, 0xF2, 0xE5, 0xFF}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + sites[2]), {0xE8, 0xF5, 0xF1, 0xE5, 0xFF}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + sites[3]), {0xE8, 0x5F, 0xF0, 0xE8, 0xFF}))
        return false;
    if (TiltedPhoques::GetCall<void*>(reinterpret_cast<uint8_t*>(s_imageBase + 0x108D3B1)) != reinterpret_cast<void*>(s_imageBase + 0x105D0E0))
        return false;
    for (const auto site : sites)
        if (TiltedPhoques::GetCall<void*>(reinterpret_cast<uint8_t*>(s_imageBase + site)) != reinterpret_cast<void*>(s_imageBase + 0xEEC1A0))
            return false;
    return true;
}
bool FreshSelectedSource(WorldPass& aPass) noexcept
{
    uint32_t serial{}, form{};
    return !aPass.ObservationOnly && aPass.SourceBinding && SteadyMs() <= aPass.SourceBinding->ExpiresAtSteadyMs && aPass.Identity.HandleReadable && aPass.Identity.Handle &&
           aPass.Identity.Handle != UINT32_MAX && Read(aPass.Identity.HandleAddress, &serial, sizeof(serial)) && serial == aPass.Identity.Handle &&
           ReadAt(reinterpret_cast<void*>(aPass.Identity.OwnedActor), offsetof(TESForm, formID), form) && form == aPass.Identity.BoundActor && WorldIdentityMatches(aPass);
}
bool PresentationSelectedSource(WorldPass& aPass) noexcept
{
    if (!aPass.GraceHold)
    {
        if (!FreshSelectedSource(aPass))
            return false;
        std::lock_guard<std::mutex> guard(s_mutex);
        return DwellAdmissionLocked(aPass, true, false);
    }
    uint32_t serial{}, form{};
    const auto now = SteadyMs();
    const auto flipAge = now - aPass.GracePose.TimeMs;
    if (aPass.GraceFlipHeld && (!std::isfinite(flipAge) || flipAge < 0. || flipAge > BodyPoseCommit::kFlipHoldTimeoutMs))
        return false;
    if (!aPass.CommitWatch || aPass.ObservationOnly || !aPass.SourceBinding || !aPass.Identity.HandleReadable || !aPass.Identity.Handle || aPass.Identity.Handle == UINT32_MAX ||
        !Read(aPass.Identity.HandleAddress, &serial, sizeof(serial)) || serial != aPass.Identity.Handle ||
        !ReadAt(reinterpret_cast<void*>(aPass.Identity.OwnedActor), offsetof(TESForm, formID), form) || form != aPass.Identity.BoundActor || !WorldIdentityMatches(aPass))
        return false;
    std::lock_guard<std::mutex> guard(s_mutex);
    auto& grace = s_slots[aPass.Slot].Grace;
    return grace.Source == aPass.SourceBinding && grace.Window.Observe(GraceKeyOf(*aPass.SourceBinding), SteadyMs(), true) == BodyPoseCommit::GraceAdmission::Hold &&
           DwellAdmissionLocked(aPass, false, true);
}
std::unordered_map<uint32_t, std::shared_ptr<const Binding>> s_active;

bool AcquireSelectedBinding(WorldPass& aPass) noexcept
{
    try
    {
        // One root-admission metadata lock only. No native call runs while held;
        // locked publishers/resetters likewise perform no engine calls.
        std::lock_guard<std::mutex> guard(s_mutex);
        const auto found = s_active.find(aPass.Identity.BoundActor);
        if (found == s_active.end())
            return false;
        const auto& source = *found->second;
        auto& slot = s_slots[aPass.Slot];
        auto& grace = slot.Grace;
        if (aPass.ObservationOnly || source.FormId != slot.Actor.load() || source.RootHint != aPass.Identity.BindingRootHint || source.TreeHint != aPass.Identity.Tree ||
            source.Sample.Generation != aPass.Identity.Generation || source.Sample.After.SourceTick != aPass.Identity.SourceTick || !source.Sample.Pose.HasData ||
            source.Sample.Pose.NoBones || slot.Identity.Revision.load(std::memory_order_acquire) != aPass.IdentityRevision)
            return false;
        const auto now = SteadyMs();
        if (now <= source.ExpiresAtSteadyMs)
        {
            if (!DwellAdmissionLocked(aPass, true, false))
                return false;
            aPass.SourceBinding = found->second;
            aPass.FreshSourceAtAdmission = true;
            return true;
        }
        if (!aPass.CommitWatch || !grace.Source || grace.Window.Observe(GraceKeyOf(source), now, true) != BodyPoseCommit::GraceAdmission::Hold ||
            grace.Source->Sample.Before.SourceTick != source.Sample.Before.SourceTick || grace.Source->Sample.After.SourceTick != source.Sample.After.SourceTick)
            return false;
        if (!DwellAdmissionLocked(aPass, false, true))
            return false;
        aPass.SourceBinding = grace.Source; // the last successful commit's exact inputs, not the latest re-sample
        aPass.GracePose = grace.Pose;
        aPass.GraceGuardDecision = grace.GuardDecision;
        aPass.GraceGuardAssessedMs = grace.GuardAssessedMs;
        aPass.GraceSourceCommit = grace.SourceCommit;
        aPass.GraceFlipHeld = grace.FlipHeld;
        aPass.GraceHold = true;
        return true;
    }
    catch (...)
    {
        return false;
    }
}
bool RetainExpiredBindingLocked(const Binding& acCurrent) noexcept
{
    const size_t cSlot = SlotOf(acCurrent.FormId);
    if (cSlot == kNoSlot)
        return false;
    auto& grace = s_slots[cSlot].Grace;
    return grace.Source && GraceKeyOf(acCurrent) == GraceKeyOf(*grace.Source) && acCurrent.Sample.Before.SourceTick == grace.Source->Sample.Before.SourceTick &&
           acCurrent.Sample.After.SourceTick == grace.Source->Sample.After.SourceTick &&
           grace.Window.Observe(GraceKeyOf(acCurrent), SteadyMs(), true) == BodyPoseCommit::GraceAdmission::Hold;
}
struct SourceLedger
{
    uint64_t Generation{}, NewestTick{};
    double ReceivedAtMs{}, LastNowNetworkMs{}, AnchorNetworkMs{}, AnchorSteadyMs{}, DeadlineSteadyMs{};
};
// Expiration/root replacement must not erase the source watermark or its original receive-time deadline.
std::unordered_map<uint32_t, SourceLedger> s_sources;

bool SourceDeadlineLocked(uint32_t aFormId, const RemotePose::Sample& acSample, double aNowNetworkMs, double aNowSteadyMs, double& aDeadline)
{
    if (acSample.Before.SourceTick > acSample.After.SourceTick || acSample.Before.ReceivedAtMs > acSample.After.ReceivedAtMs)
        return false;
    auto it = s_sources.find(aFormId);
    if (it == s_sources.end())
    {
        if (s_sources.size() >= kMaxSourceLedgers)
            return false; // fail closed instead of evicting a watermark and reviving a held old source
        it = s_sources.emplace(aFormId, SourceLedger{}).first;
    }
    auto& source = it->second;
    if (source.Generation && acSample.Generation < source.Generation)
        return false;
    const bool newGeneration = source.Generation != acSample.Generation;
    const bool newerTick = newGeneration || acSample.After.SourceTick > source.NewestTick;
    if (!newGeneration && (acSample.After.SourceTick < source.NewestTick || aNowNetworkMs < source.LastNowNetworkMs || acSample.After.ReceivedAtMs < source.ReceivedAtMs ||
                           (acSample.After.SourceTick == source.NewestTick && acSample.After.ReceivedAtMs != source.ReceivedAtMs)))
        return false;
    if (newGeneration)
    {
        source = {};
        source.Generation = acSample.Generation;
        source.AnchorNetworkMs = aNowNetworkMs;
        source.AnchorSteadyMs = aNowSteadyMs;
    }
    // Convert clock epochs once per generation. Re-publication with a frozen network poll clock cannot renew age.
    const double deadline = source.AnchorSteadyMs + (std::min(acSample.Before.ReceivedAtMs, acSample.After.ReceivedAtMs) - source.AnchorNetworkMs) + RemotePose::kLiveTimeoutMs;
    source.DeadlineSteadyMs = newerTick ? deadline : std::min(source.DeadlineSteadyMs, deadline);
    source.NewestTick = acSample.After.SourceTick;
    source.ReceivedAtMs = acSample.After.ReceivedAtMs;
    source.LastNowNetworkMs = aNowNetworkMs;
    aDeadline = source.DeadlineSteadyMs;
    return std::isfinite(aDeadline) && aNowSteadyMs <= aDeadline;
}

void RemoveBinding(uint32_t aFormId)
{
    std::lock_guard<std::mutex> guard(s_mutex);
    s_active.erase(aFormId);
    Disarm(aFormId);
    s_activeCount.store(s_active.size());
}

bool ToTransform(const std::array<float, 13>& acRaw, BodyPosePresentation::Transform& aOutput) noexcept
{
    if (!std::all_of(acRaw.begin(), acRaw.end(), [](float aValue) { return std::isfinite(aValue); }) || acRaw[12] <= 0.f)
        return false;
    glm::mat3 rotation{};
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            rotation[column][row] = acRaw[row * 3 + column]; // native NiMatrix3 is row-major
    for (size_t i = 0; i < 3; ++i)
    {
        if (std::abs(glm::dot(rotation[i], rotation[i]) - 1.f) > 0.01f)
            return false;
        for (size_t j = i + 1; j < 3; ++j)
            if (std::abs(glm::dot(rotation[i], rotation[j])) > 0.01f)
                return false;
    }
    if (std::abs(glm::determinant(rotation) - 1.f) > 0.01f)
        return false;
    const auto quaternion = glm::quat_cast(rotation);
    if (!BodyPosePresentation::Unit(quaternion))
        return false;
    aOutput.Rotation = glm::normalize(quaternion);
    aOutput.Translation = {acRaw[9], acRaw[10], acRaw[11]};
    aOutput.Scale = acRaw[12];
    return true;
}

void Disarm(uint32_t aFormId) noexcept
{
    WithdrawDwellLocked(aFormId);
    InvalidateGraceLocked(aFormId);
    DisarmJobIdentity(aFormId);
    WithdrawCommitted(aFormId);
}

constexpr size_t kCommitEntries = 192, kCommitWrites = 2 * kCommitEntries, kCommitWriteSlots = 4, kCommitAncestors = 16;
constexpr float kCommitMaxTravel = 512.f;            // game units a written world may move from its native baseline
constexpr uintptr_t kCommitStackReserve = 64 * 1024; // the value solver keeps ~22 KB of locals
constexpr float kCommitHavokScale = .0142874997109f;
// Child arrays detect attachment changes. Skinned geometries are used to refresh bounds.
constexpr size_t kCommitSubtreeSlots = 4096, kCommitSubtreeObjects = kCommitSubtreeSlots / 2, kCommitSubtreeShapes = 1024;
constexpr size_t kCommitSkinMeshes = 32, kCommitChildScratch = 1024, kCommitClassCache = 64;
constexpr uint32_t kCommitSubtreeVerifyPerPass = 8;
constexpr double kCommitSubtreeRebuildGapMs = 500.; // at most one change-driven rebuild per half second
constexpr int16_t kSubtreeOther = 0, kSubtreeNode = 1, kSubtreeGeometry = 2;
constexpr uint8_t kClassNode = 1, kClassGeometry = 2, kClassSkin = 4, kClassParticles = 8;
// rigid descendants of written linked nodes (attach/weapon nodes and their unskinned geometry), recomputed and
// written after the flat stores. the source guard's ring holds one admitted candidate per source tick.
constexpr size_t kCommitRigid = 256, kCommitRigidParents = 64, kCommitFlipRing = 8;
// finger outcome per hand and pass.
enum class CommitFingers : uint8_t
{
    Off,
    NoData,
    NotFound,
    NotComposed,
    Invalid,
    Applied,
    Count
};
static_assert(
    BodyPoseCommit::kFlipSpine1 == VRPose::kSpine1 && BodyPoseCommit::kFlipNeck == VRPose::kNeck && BodyPoseCommit::kFlipHead == VRPose::kHead &&
        BodyPoseCommit::kFlipPelvis == VRPose::kPelvis,
    "guard wires are the wire bones");
constexpr size_t kRigidRawBytes = BodyPoseCommit::kNodeFlags + sizeof(uint32_t); // one read: vtable .. flags (+00..+F7)
constexpr size_t kRigidHeaderBytes = BodyPoseCommit::kNodeLocal;                 // topology walk: vtable .. collision object (+00..+47)
static_assert(kCommitEntries <= BodyPosePresentation::kMaxNodes && kCommitWrites < 0x7FFF);
enum class CommitNodeKind : int16_t
{
    Root,
    Linked,
    Outside,
    Tree,
    Ancestor
};
enum class CommitReason : uint32_t
{
    Committed,
    NotReady,
    Busy,
    Scope,
    NoSource,
    Expired,
    Identity,
    Stack,
    Tree,
    Capacity,
    Topology,
    Named,
    RootChanged,
    Baseline,
    ActorState,
    Solve,
    NoTargets,
    BaselineChanged,
    ExpiredInPass,
    WriteSlotBusy,
    NonFinite,
    TreeChanged,
    FlipTimeout,
    FlipNoCandidate, // the source guard released the pass (held pose too old / nothing plausible to hold)
    HavokWorld,
    HavokUnreadable
}; // a biped body is in a Havok world / its Havok state could not be read (Havok gate)
struct CommitRecord
{
    uint64_t Sequence{}, SourceBefore{}, SourceAfter{};
    uint32_t PassFrame{}, Actor{}, SkippedInvalid{}, NonFiniteRejected{};
    uintptr_t Root{}, Tree{};
    double SourceDeadlineMs{}, SourceCheckedMs{}, MonotonicMs{}, GraceGuardAssessmentMs{};
    float ScaleSender = -1.f, ScaleNative = -1.f;
    uint32_t RigidParentsChanged{};
    std::array<uint8_t, 2> Fingers{};
    CommitReason Reason = CommitReason::NotReady;
    bool TopologyRebuilt{}, FreshAtCommit{}, FlipHeld{}, RigidEnabled{}, Committed{}, Ticket{}, RootPost{};
    std::array<uint8_t, 2> Ik{};
    uint32_t HavokInWorld{}, HavokUnreadable{};
};
struct CommitSkinMesh
{
    uintptr_t Geometry{}, Skin{};
};
struct CommitShape
{
    uintptr_t Node{}, Array{};
    uint16_t Bound{};
    uint64_t Hash{};
};
constexpr uint32_t kWriting = 0x80000000u;
struct CommitWriteSlot
{
    std::atomic<uint32_t> Readers{}, Committing{};
    std::atomic<uint64_t> Age{};
    std::atomic<bool> Live{};
    uint32_t Written{};
    BodyPoseCommit::AddressTable<1024> Addresses;
};
struct CommitState
{
    // Topology cache, revalidated against the native entry array every pass.
    bool TopologyValid{};
    uint64_t TopologyId{};
    uintptr_t Root{}, RootParent{}, Tree{}, Entries{};
    uint32_t Count{}, Linked{}, RootExcluded{}, AncestorCount{};
    std::array<uintptr_t, kCommitEntries> Node{}, NodeParent{};
    std::array<int16_t, kCommitEntries> FlatParent{};
    std::array<int, kCommitEntries> Wire{}, SolverParent{};
    std::array<int, VRPose::kBoneCount> Bone{};
    std::array<bool, kCommitEntries> Excluded{};
    // The hands' "WEAPON" and "SHIELD" entries while the held-weapon placement is on: it writes them and everything below them.
    std::array<bool, kCommitEntries> LeftToPlacement{};
    uint32_t LeftToPlacementCount{};
    std::array<bool, BodyPosePresentation::kMaxNodes> HipExcluded{};
    std::array<uintptr_t, kCommitAncestors> Ancestors{};
    // Live tracking and the last commit, for the release check after a source stop.
    bool Live{};
    uint64_t LastCommit{};
    uint32_t LastCommitFrame{}, LastCount{};
    // Per-pass scratch (static: no allocation or large stack frame inside the native dispatch).
    std::array<uint8_t, kCommitEntries * BodyPoseCommit::kEntryStride> Raw{}, Raw2{};
    std::array<std::array<uint8_t, 0x80>, kCommitEntries> NodeRaw{}, NodeRaw2{}; // node+30..+B0: parent, local, world
    std::array<BodyPoseCommit::Transform, kCommitEntries> Baseline{}, EntryWorld{}, Target{};
    std::array<bool, kCommitEntries> Selected{}, Refused{}; // Refused (): left native by the flat validation
    std::array<BodyPoseCommit::Write, kCommitWrites> Writes{};
    BodyPoseCommit::FlipPose PendingGracePose{};
    bool PendingGraceValid{}, PendingFlipReached{}, PendingFlipAdvanced{};
    BodyPoseCommit::FlipGuard<kCommitFlipRing> PendingFlip{};
    BodyPoseCommit::FlipDecision PendingFlipDecision{};
    CommitRecord RetryRecord{}; // static scratch: retry introduces no large native stack frame
    BodyPosePresentation::Input In{};
    BodyPosePresentation::Output Out{};
    // actor-subtree membership set, NiNode shapes, skinned geometries, RTTI class cache (by vtable), angles.
    BodyPoseCommit::AddressTable<kCommitSubtreeSlots> Subtree{};
    std::array<uintptr_t, kCommitSubtreeObjects> SubtreeOrder{};
    std::array<BodyPoseCommit::SubtreePending, kCommitSubtreeObjects> SubtreeStack{};
    BodyPoseCommit::SubtreeStats SubtreeStats{};
    std::array<CommitShape, kCommitSubtreeShapes> Shapes{};
    std::array<uintptr_t, kCommitSkinMeshes> Geometries{};
    std::array<uintptr_t, kCommitChildScratch> ChildScratch{};
    uint32_t ShapeCount{}, VerifyCursor{}, GeometryCount{}, SubtreeNodeCount{}, SubtreeForeign{}, RebuildCause{};
    bool SubtreeComplete{}, ShapesTruncated{}, GeometriesTruncated{}, SubtreeChangedPending{};
    double SubtreeRebuildNotBeforeMs{};
    uint32_t SkinMeshCount{};
    std::array<CommitSkinMesh, kCommitSkinMeshes> SkinMeshes{};
    uint64_t WitnessOrdinal{};
    std::array<uintptr_t, kCommitClassCache> ClassVtable{};
    std::array<uint8_t, kCommitClassCache> ClassKnown{}, ClassIs{};
    size_t ClassCount{};
    // rigid descendants (topology part, rebuilt with the subtree set; preorder, parents first).
    uint32_t RigidCount{}, RigidParentCount{};
    bool RigidTruncated{};
    std::array<BodyPoseCommit::RigidItem, kCommitRigid> RigidItems{};
    std::array<uintptr_t, kCommitRigid> RigidObject{}, RigidVtable{}, RigidParent{}, RigidCollision{}; // + collision object
    std::array<uint16_t, kCommitRigid> RigidParentSlot{};                                              // index into RigidParentNode (the child array that must still hold it)
    std::array<bool, kCommitRigid> RigidOutside{};
    std::array<uintptr_t, kCommitRigidParents> RigidParentNode{};
    std::array<int32_t, kCommitRigidParents> RigidParentShape{}; // index into Shapes, -1 unknown
    std::array<bool, kCommitRigidParents> RigidParentOk{};
    BodyPoseCommit::AddressTable<512> LinkedIndex{}; // linked node -> flat entry (written-capable parents)
    BodyPoseCommit::AddressTable<1024> RigidIndex{}; // rigid object -> item
    std::array<uint8_t, kRigidHeaderBytes> RigidHeader{};
    // Per pass.
    std::array<BodyPoseCommit::RigidRead, kCommitRigid> RigidReads{};
    std::array<BodyPoseCommit::RigidResult, kCommitRigid> RigidResults{};
    std::array<uint8_t, kRigidRawBytes> RigidRaw{};
    uint32_t RigidLastCount{};
    uint64_t RigidLastCommit{};
    // source plausibility guard: admitted candidates (one per source tick), bad tick, current episode.
    BodyPoseCommit::FlipGuard<kCommitFlipRing> Flip{};
    // topology part: skeleton root (-1 the tree, an entry, -2 none) and the entries the root-scale pass scales, the
    // finger chains (per hand, thumb first, parent first) and toes. Per pass: native solver transforms before any shape
    // pass, the tree's world (when it is the skeleton root) and Havok scratch (hkp +10..+160). Selected epoch (re-selection).
    // Zero-initialised like the rest of the state; the topology build sets ScaleRoot/ToeEntry (-2/-1 for none) before use.
    int32_t ScaleRoot{};
    std::array<bool, kCommitEntries> ScaleUnder{};
    std::array<std::array<int, BodyPosePresentation::kFingerBonesPerHand>, 2> FingerEntry{};
    std::array<bool, 2> FingersFound{};
    std::array<int32_t, 2> ToeEntry{};
    std::array<BodyPosePresentation::Transform, kCommitEntries> NativeT{};
    std::array<float, 13> TreeWorld{};
    std::array<uint8_t, BodyPoseCommit::kHavokMotionOffset - BodyPoseCommit::kHavokWorldOffset + 1> HavokRaw{};
    uint64_t SelectedEpoch{};
    bool RebindPending{}, RebindDroppedLive{}, RebuildForRebind{};
};
constexpr size_t kNodeRawWorld = BodyPoseCommit::kNodeWorld - BodyPoseCommit::kNodeParent; // world offset inside NodeRaw
static_assert(kNodeRawWorld + BodyPoseCommit::kTransformBytes == 0x80);
bool s_bodyCommitReady{};
std::atomic<uint32_t> s_commitBusy{}, s_commitWriteCurrent{}; // current: slot index + 1, zero none
std::atomic<uint64_t> s_commitTopologyCounter{};
std::array<CommitWriteSlot, kCommitWriteSlots> s_commitWriteSlots{};
// One commit state per slot. Every copy's commit runs inside the one commit section (s_commitBusy), as a single copy's
// did: the held-weapon placement and the add-on presenters it calls are never entered by two copies at once.
std::array<CommitState, kSlots> s_commits;
CommitState* s_commitState{}; // the state of the copy being committed; set inside the commit section only
static_assert(std::atomic<uint8_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
constexpr bool kCommitFlipGuard = true; // Source plausibility guard.
// The rigid pass, and the install-time guards of the native world update its math replicates (InstallBodyCommit).
bool s_commitRigidReady{};
bool CommitRigidAvailable() noexcept
{
    return s_commitRigidReady;
}
// (review R1): collision-owning rigid items are decided per pass (BodyPoseCommit::DecideRigid: written only when
// their native world is bit for bit the engine's compose from the parent, collision_composed; else collision_driven).
// s_commitRigidCollisionReady: the native collision world updates that rule relies on matched at install
bool s_commitRigidCollisionReady{};
constexpr bool kCommitHavokGate = true;
// The five instantiable bhk collision classes of SE 1.7.104 (RTTI .?AV...CollisionObject@@ of the installed image;
// NiCollisionObject and bhkNiCollisionObject are abstract): vtable RVA and the functions at slots 130h (UpdateWorldData,
// called by EEBFCB), 158h and 160h (the arms the class updates 106AFD0 / 108D310 dispatch to), 168h and 178h (106AFD0's
// reset step and its keyframed-or-fixed test). dumpbin of the installed image (SHA 846efccf...402f): slot 160h composes
// the node through EEC1A0 and then pushes that just-composed world into the body or phantom (105D0E0, 109EE70, 1085430);
// slot 158h copies the body's or phantom's transform into the node world through 106B160 (105CE50, 109EE20, 1085350), or
// for the blend classes blends the two (108CDD0, reaching EEC1A0 and 105D0E0 itself); slot 168h zeroes the body's
// velocities (105D420) or does nothing (10855B0); slot 178h tests motion type 4/5 (105D3A0) or returns true (10855A0).
// No arm reads the node world before EEC1A0 rewrote it in the same call.
struct CommitCollisionClass
{
    const char* Name;
    uint32_t Vtable, Update, Slot158, Slot160, Slot168, Slot178;
};
constexpr std::array<CommitCollisionClass, 5> kCommitCollisionClasses{{
    {"bhkCollisionObject", 0x1828388, 0x105D300, 0x105CE50, 0x105D0E0, 0x105D420, 0x105D3A0},
    {"bhkBlendCollisionObject", 0x1A85198, 0x108D310, 0x108CDD0, 0x105D0E0, 0x105D420, 0x105D3A0},
    {"bhkAttachmentCollisionObject", 0x1A826D0, 0x108D310, 0x108CDD0, 0x105D0E0, 0x105D420, 0x105D3A0},
    {"bhkSPCollisionObject", 0x193D278, 0x106AFD0, 0x109EE20, 0x109EE70, 0x10855B0, 0x10855A0},
    {"bhkPCollisionObject", 0x1828518, 0x106AFD0, 0x1085350, 0x1085430, 0x10855B0, 0x10855A0},
}};
// The calls those arms make (opcode, rel32 target), same image.
struct CommitCallSite
{
    uint32_t Site;
    uint8_t Opcode;
    uint32_t Target;
};
constexpr std::array<CommitCallSite, 9> kCommitCollisionCalls{{
    {0x105D373, 0xE9, 0xEEC1A0},  // 105D300: a rigid body whose body+20 bit 6 is set -> the node's own compose only
    {0x105D382, 0xE8, 0x106AFD0}, // 105D300: otherwise the bhkNiCollisionObject update
    {0x106B0D3, 0xE8, 0xEEC1A0},  // 106AFD0: inactive, unsynced or TLS-mode arms -> compose only
    {0x105D13C, 0xE8, 0xEEC1A0},  // 105D0E0 (keyframed/fixed body): compose first, then push it into the body
    {0x109EE91, 0xE8, 0xEEC1A0},  // 109EE70 (simple phantom): compose first, then push
    {0x1085468, 0xE8, 0xEEC1A0},  // 1085430 (phantom): compose first, then push
    {0x105D081, 0xE8, 0x106B160}, // 105CE50 (dynamic body): node world := body transform
    {0x109EE5C, 0xE8, 0x106B160}, // 109EE20: node world := phantom transform
    {0x1085413, 0xE8, 0x106B160}, // 1085350: node world := phantom transform
}};
int CommitCollisionClassOf(uintptr_t aVtable) noexcept
{
    for (size_t i = 0; i < kCommitCollisionClasses.size(); ++i)
        if (aVtable && aVtable == s_imageBase + kCommitCollisionClasses[i].Vtable)
            return static_cast<int>(i);
    return -1;
}
template <class T> T CommitRawAt(const std::array<uint8_t, kCommitEntries * BodyPoseCommit::kEntryStride>& acRaw, size_t aEntry, size_t aOffset) noexcept
{
    T value{};
    std::memcpy(&value, acRaw.data() + aEntry * BodyPoseCommit::kEntryStride + aOffset, sizeof(T));
    return value;
}
void ResetCommitWriteSlot(CommitWriteSlot& aSlot, uint32_t) noexcept
{
    aSlot.Live.store(false, std::memory_order_release);
}
// Reserve the oldest non-current slot; its previous witness is finalized first (reuse).
CommitWriteSlot* ReserveCommitWriteSlot() noexcept
{
    const auto current = s_commitWriteCurrent.load(std::memory_order_acquire);
    std::array<size_t, kCommitWriteSlots> order{};
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(
        order.begin(), order.end(),
        [](size_t aLeft, size_t aRight) { return s_commitWriteSlots[aLeft].Age.load(std::memory_order_relaxed) < s_commitWriteSlots[aRight].Age.load(std::memory_order_relaxed); });
    for (const auto i : order)
    {
        if (i + 1 == current)
            continue;
        auto& slot = s_commitWriteSlots[i];
        uint32_t free{};
        if (!slot.Readers.compare_exchange_strong(free, kWriting, std::memory_order_acquire))
            continue;
        ResetCommitWriteSlot(slot, 1);
        return &slot;
    }
    return nullptr;
}
// Under kWriting: identity, counters and the selected-subtree node table (root, tree, linked nodes,
// ancestors of the root, and the composed nodes outside the flat tree recorded by this pass's descent).
void BeginCommitWriteSlot(CommitWriteSlot& aSlot, uint32_t, const CommitRecord& acRecord, const WorldPass*) noexcept
{
    aSlot.Age.store(acRecord.Sequence, std::memory_order_relaxed);
    aSlot.Written = 0;
    aSlot.Committing.store(0, std::memory_order_relaxed);
    aSlot.Addresses.Clear();
}
void PublishCommitWriteSlot(CommitWriteSlot& aSlot, bool aCommitting) noexcept
{
    aSlot.Committing.store(aCommitting ? 1u : 0u, std::memory_order_relaxed);
    aSlot.Live.store(true, std::memory_order_relaxed);
    aSlot.Readers.store(0, std::memory_order_release);
    s_commitWriteCurrent.store(static_cast<uint32_t>(&aSlot - s_commitWriteSlots.data()) + 1, std::memory_order_release);
}

// ----- actor-subtree membership set, change detection and skin coverage (all under s_commitBusy) -----
// RTTI class test by vtable, cached (vtables are image constants); HasType reads only through ReadProcessMemory.
bool CommitObjectIs(uintptr_t aObject, uintptr_t aVtable, uint8_t aBit) noexcept
{
    auto& c = *s_commitState;
    size_t at = c.ClassCount;
    for (size_t i = 0; i < c.ClassCount; ++i)
        if (c.ClassVtable[i] == aVtable)
        {
            at = i;
            break;
        }
    if (at < c.ClassCount && (c.ClassKnown[at] & aBit))
        return (c.ClassIs[at] & aBit) != 0;
    const char* name = aBit == kClassNode        ? ".?AVNiNode@@"
                       : aBit == kClassGeometry  ? ".?AVBSGeometry@@"
                       : aBit == kClassParticles ? ".?AVNiParticles@@"
                                                 : ".?AVNiSkinInstance@@"; // NiParticles (base of the particle systems)
    const bool is = HasType(reinterpret_cast<void*>(aObject), name);
    if (at == c.ClassCount && c.ClassCount < c.ClassVtable.size())
    {
        c.ClassVtable[at] = aVtable;
        c.ClassKnown[at] = c.ClassIs[at] = 0;
        ++c.ClassCount;
    }
    if (at < c.ClassCount)
    {
        c.ClassKnown[at] = static_cast<uint8_t>(c.ClassKnown[at] | aBit);
        if (is)
            c.ClassIs[at] = static_cast<uint8_t>(c.ClassIs[at] | aBit);
    }
    return is;
}
// NiNode children: NiTArray at +110 (data +118, capacity +120, populated bound +122, as the native scene walk uses).
bool ReadCommitChildren(uintptr_t aNode, uintptr_t& aData, uint16_t& aBound) noexcept
{
    auto& c = *s_commitState;
    std::array<uint8_t, 12> raw{};
    if (!Read(aNode + 0x118, raw.data(), raw.size()))
        return false;
    uint16_t capacity{};
    std::memcpy(&aData, raw.data(), sizeof(aData));
    std::memcpy(&capacity, raw.data() + 8, sizeof(capacity));
    std::memcpy(&aBound, raw.data() + 10, sizeof(aBound));
    if (aBound > capacity || aBound > c.ChildScratch.size() || (aBound && !aData))
        return false;
    return !aBound || Read(aData, c.ChildScratch.data(), aBound * sizeof(uintptr_t));
}
template <class Push> int CommitSubtreeVisit(uintptr_t aObject, Push& aPush) noexcept
{
    auto& c = *s_commitState;
    uintptr_t vtable{};
    if (!ReadAt(reinterpret_cast<void*>(aObject), 0, vtable) || !vtable)
        return -1;
    if (vtable < s_imageBase || vtable - s_imageBase >= s_imageSize)
    {
        ++c.SubtreeForeign;
        return -1;
    } // children not enumerable: incomplete
    if (CommitObjectIs(aObject, vtable, kClassNode))
    {
        ++c.SubtreeNodeCount;
        uintptr_t data{};
        uint16_t bound{};
        if (!ReadCommitChildren(aObject, data, bound))
            return -1;
        if (c.ShapeCount < c.Shapes.size())
            c.Shapes[c.ShapeCount++] = {aObject, data, bound, BodyPoseCommit::ChildHash(c.ChildScratch.data(), bound)};
        else
            c.ShapesTruncated = true;
        for (uint16_t i = 0; i < bound; ++i)
            aPush(c.ChildScratch[i]); // a refused push (depth/stack) is recorded in the stats
        return kSubtreeNode;
    }
    if (CommitObjectIs(aObject, vtable, kClassGeometry))
    {
        if (c.GeometryCount < c.Geometries.size())
            c.Geometries[c.GeometryCount++] = aObject;
        else
            c.GeometriesTruncated = true;
        return kSubtreeGeometry;
    }
    return kSubtreeOther;
}
double BuildCommitSubtree(uintptr_t aRoot) noexcept
{
    auto& c = *s_commitState;
    const double started = SteadyMs();
    c.ShapeCount = c.GeometryCount = c.SubtreeNodeCount = c.SubtreeForeign = 0;
    c.ShapesTruncated = c.GeometriesTruncated = false;
    c.SubtreeStats = BodyPoseCommit::CollectSubtree(
        aRoot, static_cast<uint32_t>(kMaxDepth), c.Subtree, c.SubtreeOrder, c.SubtreeStack,
        [](uintptr_t aObject, uint32_t, auto& aPush) noexcept { return CommitSubtreeVisit(aObject, aPush); });
    c.SubtreeComplete = c.SubtreeStats.Complete() && !c.ShapesTruncated;
    c.VerifyCursor = 0;
    c.SubtreeChangedPending = false;
    c.SubtreeRebuildNotBeforeMs = SteadyMs() + kCommitSubtreeRebuildGapMs;

    return (SteadyMs() - started) * 1000.;
}
// Verify a few NiNode child arrays per pass (round robin). False: the subtree changed since it was built.
uint32_t VerifyCommitSubtreeSlice(bool& aChanged) noexcept
{
    auto& c = *s_commitState;
    aChanged = false;
    const uint32_t n = std::min<uint32_t>(kCommitSubtreeVerifyPerPass, c.ShapeCount);
    for (uint32_t k = 0; k < n; ++k)
    {
        const auto& shape = c.Shapes[c.VerifyCursor];
        c.VerifyCursor = (c.VerifyCursor + 1) % c.ShapeCount;
        uintptr_t data{};
        uint16_t bound{};
        if (!ReadCommitChildren(shape.Node, data, bound) || data != shape.Array || bound != shape.Bound || BodyPoseCommit::ChildHash(c.ChildScratch.data(), bound) != shape.Hash)
        {
            aChanged = true;
            return k + 1;
        }
    }
    return n;
}
void CommitSafeName(uintptr_t aObject, std::array<char, 48>& aOutput) noexcept
{
    aOutput.fill(0);
    uintptr_t name{};
    std::array<char, 48> text{};
    if (!ReadAt(reinterpret_cast<void*>(aObject), 0x10, name) || !name || !Read(name, text.data(), text.size() - 1))
        return;
    for (size_t i = 0; i + 1 < aOutput.size() && text[i]; ++i)
    {
        const char ch = text[i];
        const bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-' || ch == '[' || ch == ']' ||
                           ch == ':' || ch == '.' || ch == '(' || ch == ')' || ch == '#';
        aOutput[i] = plain ? ch : '?'; // JSON-safe: no quote, backslash, control or non-ASCII byte
    }
}
void BuildCommitSkin(uintptr_t, uint32_t) noexcept
{
    auto& c = *s_commitState;
    c.SkinMeshCount = 0;
    for (uint32_t g = 0; g < c.GeometryCount; ++g)
    {
        const auto geometry = c.Geometries[g];
        uintptr_t skin{}, vtable{};
        if (!ReadAt(reinterpret_cast<void*>(geometry), 0x130, skin) || !skin)
            continue;
        if (!ReadAt(reinterpret_cast<void*>(skin), 0, vtable) || !vtable || !CommitObjectIs(skin, vtable, kClassSkin))
            continue;
        if (c.SkinMeshCount < c.SkinMeshes.size())
        {
            auto& mesh = c.SkinMeshes[c.SkinMeshCount++];
            mesh.Geometry = geometry;
            mesh.Skin = skin;
        }
    }
}
// every object of the actor subtree that hangs (directly or through other such objects) under a written-capable
// linked node and is not itself a flat entry: the rigid attachments the engine composed from the PRE-commit worlds.
// Read-only diagnostics: no engine write and no logging/allocation inside native scopes.
void BuildCommitRigid(const WorldPass& acPass, uint64_t aCommit) noexcept
{
    auto& c = *s_commitState;
    c.RigidCount = c.RigidParentCount = 0;
    c.RigidTruncated = false;
    c.RigidLastCount = 0; // new topology: earlier rigid addresses are not checked any more
    c.RigidOutside.fill(false);
    c.LinkedIndex.Clear();
    c.RigidIndex.Clear();
    for (uint32_t i = 0; i < c.Count; ++i)
        if (c.Node[i] && !c.Excluded[i])
            c.LinkedIndex.Insert(c.Node[i], static_cast<int16_t>(i));
    std::array<uint8_t, kCommitRigid> depth{};
    for (size_t k = 0; k < c.SubtreeStats.Objects && k < c.SubtreeOrder.size(); ++k)
    {
        const uintptr_t object = c.SubtreeOrder[k];
        if (object == c.Root || object == c.Tree || c.LinkedIndex.Find(object) >= 0)
            continue;
        if (!Read(object, c.RigidHeader.data(), c.RigidHeader.size()))
            continue; // vtable .. collision (+00..+47)
        uintptr_t vtable{}, parent{}, collision{};
        std::memcpy(&vtable, c.RigidHeader.data(), sizeof(vtable));
        std::memcpy(&parent, c.RigidHeader.data() + BodyPoseCommit::kNodeParent, sizeof(parent));
        std::memcpy(&collision, c.RigidHeader.data() + BodyPoseCommit::kNodeCollision, sizeof(collision));
        const int entry = c.LinkedIndex.Find(parent), rigid = c.RigidIndex.Find(parent);
        if (entry < 0 && rigid < 0)
            continue; // not under a written-capable node: its native world stays right
        if (c.RigidCount == kCommitRigid)
        {
            c.RigidTruncated = true;
            continue;
        }
        const uint32_t n = c.RigidCount;
        auto& item = c.RigidItems[n];
        item = {};
        item.ParentEntry = entry;
        item.ParentRigid = entry >= 0 ? -1 : rigid;
        const int kind = c.Subtree.Find(object);
        using BodyPoseCommit::RigidSkip;
        // the class's world update (vtable slot 180h) must be the plain NiAVObject one, EEBF40, which ComposeWorld
        // replicates (vtables read from the installed image: every NiNode/BSGeometry class used here has it; NiBillboardNode,
        // BSValueNode and NiParticleSystem have their own). Refused statically as own_update; not_composed stays the runtime net.
        const bool imageClass = vtable && vtable >= s_imageBase && vtable - s_imageBase < s_imageSize;
        uintptr_t update{};
        const bool plainUpdate = imageClass && ReadAt(reinterpret_cast<void*>(vtable), BodyPoseCommit::kNodeUpdateWorldSlot, update) && update == s_imageBase + 0xEEBF40;
        if (kind == kSubtreeGeometry)
        {
            uintptr_t skin{};
            if (!imageClass)
                item.Static = RigidSkip::OtherClass;
            else if (CommitObjectIs(object, vtable, kClassParticles))
                item.Static = RigidSkip::Particles;
            else if (!plainUpdate)
                item.Static = RigidSkip::OwnUpdate;
            else if (!ReadAt(reinterpret_cast<void*>(object), BodyPoseCommit::kGeometrySkin, skin))
                item.Static = RigidSkip::Unverified;
            else if (skin)
                item.Static = RigidSkip::Skinned;
        }
        else if (kind == kSubtreeNode)
        {
            if (!imageClass)
                item.Static = RigidSkip::OtherClass;
            else if (!plainUpdate)
                item.Static = RigidSkip::OwnUpdate;
        }
        else
            item.Static = RigidSkip::OtherClass;
        // (review R1): a collision object no longer refuses the item here; each pass decides (DecideRigid: written
        // only on an exact compose, collision_composed, else collision_driven). Static collision remains for a class
        // whose world update was not analysed, or when those guards failed. Its state is read once, for the record.
        CollisionBodyChain chain{};
        int collisionClass = -1;
        if (collision)
        {
            chain = ReadCollisionBodyChain(object, true);
            collisionClass = chain.CollisionTableReadable ? CommitCollisionClassOf(chain.CollisionTable) : -1;
            if (item.Static == RigidSkip::None && (collisionClass < 0 || !s_commitRigidCollisionReady))
                item.Static = RigidSkip::Collision;
        }
        uint32_t slot = c.RigidParentCount;
        for (uint32_t p = 0; p < c.RigidParentCount; ++p)
            if (c.RigidParentNode[p] == parent)
            {
                slot = p;
                break;
            }
        if (slot == c.RigidParentCount)
        {
            if (c.RigidParentCount < kCommitRigidParents)
            {
                c.RigidParentNode[slot] = parent;
                c.RigidParentShape[slot] = -1;
                for (uint32_t s = 0; s < c.ShapeCount; ++s)
                    if (c.Shapes[s].Node == parent)
                    {
                        c.RigidParentShape[slot] = static_cast<int32_t>(s);
                        break;
                    }
                ++c.RigidParentCount;
            }
            else
                slot = kCommitRigidParents;
        }
        // The recorded child array of the parent must hold this object: per pass, an unchanged array (pointer, bound,
        // hash of the child pointers) then proves the object is still attached to a live parent.
        uintptr_t childData{};
        uint16_t childBound{};
        const bool held = slot < kCommitRigidParents && c.RigidParentShape[slot] >= 0 && ReadCommitChildren(parent, childData, childBound) &&
                          std::find(c.ChildScratch.begin(), c.ChildScratch.begin() + childBound, object) != c.ChildScratch.begin() + childBound;
        if (!held)
        {
            if (item.Static == RigidSkip::None)
                item.Static = RigidSkip::Unverified; // the child array that proves attachment is unknown
            if (slot >= kCommitRigidParents)
                slot = 0;
        }
        c.RigidObject[n] = object;
        c.RigidVtable[n] = vtable;
        c.RigidParent[n] = parent;
        c.RigidCollision[n] = collision;
        c.RigidParentSlot[n] = static_cast<uint16_t>(slot);
        depth[n] = static_cast<uint8_t>(entry >= 0 ? 1 : std::min<int>(depth[static_cast<size_t>(rigid)] + 1, 255));
        c.RigidIndex.Insert(object, static_cast<int16_t>(n));
        ++c.RigidCount;
    }
    // The nodes the qualified native descent composed in this pass that are not root, tree, linked or ancestors
    // (BeginCommitWriteSlot counts the same set per pass as outsideFlatComposed).
    const auto recorded = std::min<size_t>(acPass.CommitDescentCalls, acPass.CommitDescentNodes.size());
    for (size_t i = 0; i < recorded; ++i)
    {
        const auto node = acPass.CommitDescentNodes[i];
        if (!node || node == c.Root || node == c.Tree || c.LinkedIndex.Find(node) >= 0)
            continue;
        if (std::find(c.Ancestors.begin(), c.Ancestors.begin() + c.AncestorCount, node) != c.Ancestors.begin() + c.AncestorCount)
            continue;
        bool repeated{};
        for (size_t j = 0; j < i && !repeated; ++j)
            repeated = acPass.CommitDescentNodes[j] == node;
        if (repeated)
            continue;
        const int rigid = c.RigidIndex.Find(node);
        if (rigid >= 0)
            c.RigidOutside[static_cast<size_t>(rigid)] = true;
    }
}

bool NodeName(uintptr_t aAddress, std::array<char, 64>& aText) noexcept
{
    return aAddress && Read(aAddress, aText.data(), aText.size()) && std::find(aText.begin(), aText.end(), '\0') != aText.end();
}
bool IsWeaponAttachNode(uintptr_t aNode) noexcept
{
    uintptr_t name{};
    std::array<char, 64> text{};
    return aNode && ReadAt(reinterpret_cast<void*>(aNode), 0x10, name) && NodeName(name, text) &&
           (std::strcmp(text.data(), "WEAPON") == 0 || std::strcmp(text.data(), "SHIELD") == 0);
}
bool RebuildCommitTopology(const WorldPass& acPass, uintptr_t aRoot, uintptr_t aTree, uintptr_t aEntries, uint32_t aCount, CommitRecord& aRecord, CommitReason& aReason) noexcept
{
    auto& c = *s_commitState;
    c.TopologyValid = false;
    c.Linked = 0;
    c.Bone.fill(-1);
    aReason = CommitReason::Topology;
    for (uint32_t i = 0; i < aCount; ++i)
    {
        c.Node[i] = CommitRawAt<uintptr_t>(c.Raw, i, BodyPoseCommit::kEntryNode);
        c.FlatParent[i] = CommitRawAt<int16_t>(c.Raw, i, BodyPoseCommit::kEntryParent);
        c.Wire[i] = -1;
        c.NodeParent[i] = 0;
        c.SolverParent[i] = c.FlatParent[i] < 0 ? -1 : c.FlatParent[i]; // EF4F95: any negative index means the tree's own world
        if (c.FlatParent[i] >= 0 && static_cast<uint32_t>(c.FlatParent[i]) >= aCount)
        {
            return false;
        }
        if (!c.Node[i])
            continue;
        ++c.Linked;
        for (uint32_t j = 0; j < i; ++j)
            if (c.Node[j] == c.Node[i])
            {
                return false;
            } // aliased node identity
        if (!UnderRoot(reinterpret_cast<void*>(c.Node[i]), reinterpret_cast<void*>(aRoot)) ||
            !ReadAt(reinterpret_cast<void*>(c.Node[i]), BodyPoseCommit::kNodeParent, c.NodeParent[i]))
        {
            return false;
        }
        c.Wire[i] = WireIndex(reinterpret_cast<void*>(c.Node[i]));
        if (c.Wire[i] >= 0)
        {
            if (c.Bone[c.Wire[i]] != -1)
            {
                aReason = CommitReason::Named;
                return false;
            }
            c.Bone[c.Wire[i]] = static_cast<int>(i);
        }
    }
    // Linked parents follow the actual node hierarchy to the nearest linked ancestor (FillCandidate semantics).
    for (uint32_t i = 0; i < aCount; ++i)
    {
        if (!c.Node[i])
            continue;
        c.SolverParent[i] = -1;
        if (c.Node[i] == aRoot)
            continue;
        uintptr_t parent = c.NodeParent[i];
        bool resolved{};
        std::array<uintptr_t, kMaxDepth> seen{};
        for (size_t depth = 0; parent && depth < seen.size(); ++depth)
        {
            if (std::find(seen.begin(), seen.begin() + depth, parent) != seen.begin() + depth)
                break;
            seen[depth] = parent;
            for (uint32_t j = 0; j < aCount; ++j)
                if (c.Node[j] == parent)
                {
                    c.SolverParent[i] = static_cast<int>(j);
                    resolved = true;
                    break;
                }
            if (resolved || parent == aRoot)
            {
                resolved = true;
                break;
            }
            if (!ReadAt(reinterpret_cast<void*>(parent), BodyPoseCommit::kNodeParent, parent))
                break;
        }
        if (!resolved || c.SolverParent[i] == static_cast<int>(i))
        {
            return false;
        }
    }
    for (const auto entry : c.Bone)
        if (entry == -1)
        {
            aReason = CommitReason::Named;
            return false;
        } // the controlled candidate needs all 19 named targets
    c.RootExcluded = BodyPoseCommit::MarkRootExcluded(c.Node, aCount, aRoot, c.Excluded);
    // The held-weapon placement (VRBodySync::PlaceHeldWeapons) owns the hands' attach nodes: it composes them
    // and everything below them after this commit, on both sides. Left unselected here, their rigid items are skipped as
    // parent_unwritten, so nothing this commit stores is overwritten the same frame. Names are read once per topology.
    c.LeftToPlacement.fill(false);
    c.LeftToPlacementCount = 0;
    for (uint32_t i = 0; i < aCount; ++i)
        if (!c.Excluded[i] && IsWeaponAttachNode(c.Node[i]))
        {
            c.LeftToPlacement[i] = true;
            ++c.LeftToPlacementCount;
        }
    c.HipExcluded.fill(false);
    for (uint32_t i = 0; i < aCount; ++i)
        c.HipExcluded[i] = c.Excluded[i];
    c.AncestorCount = 0;
    c.RootParent = 0;
    if (!ReadAt(reinterpret_cast<void*>(aRoot), BodyPoseCommit::kNodeParent, c.RootParent))
        return false;
    for (uintptr_t up = c.RootParent; up && c.AncestorCount < c.Ancestors.size();)
    {
        c.Ancestors[c.AncestorCount++] = up;
        if (!ReadAt(reinterpret_cast<void*>(up), BodyPoseCommit::kNodeParent, up))
            break;
    }
    // the skeleton root "NPC Root [Root]" (the sender's RootScale is its world scale), found by name as upstream
    // FindSkeletonRoot does. On SE 1.7.104 the flat tree is that node itself (the 94 entries are skeleton.nif's NPC Root
    // subtree without it, in depth-first order; negative flat parents compose from tree+7C, EF4FAF); a linked entry of that
    // name is the fallback. Scaled entries: every non-excluded entry below it by flat parent; all or nothing: if a linked
    // one is not also below it in the node hierarchy, no entry is scaled (never part of a body). Finger chains by structure
    // (BodyPoseCommit::FindFingerChains), each bone flat-only or linked directly under its flat parent's node; toes: the
    // only flat child of each foot. None found: that part stays off.
    std::array<char, 48> treeName{};
    CommitSafeName(aTree, treeName);
    c.ScaleRoot = -2;
    c.ScaleUnder.fill(false);
    if (std::strcmp(treeName.data(), "NPC Root [Root]") == 0)
        c.ScaleRoot = -1;
    else
        for (uint32_t i = 0; i < aCount && c.ScaleRoot == -2; ++i)
        {
            if (!c.Node[i] || c.Excluded[i])
                continue;
            std::array<char, 48> name{};
            CommitSafeName(c.Node[i], name);
            if (std::strcmp(name.data(), "NPC Root [Root]") == 0)
                c.ScaleRoot = static_cast<int32_t>(i);
        }
    uint32_t scaleEntries{}, scaleNotUnder{};
    const int32_t scaleNamed = c.ScaleRoot;
    if (c.ScaleRoot != -2)
    {
        const uintptr_t scaleNode = c.ScaleRoot == -1 ? aTree : c.Node[static_cast<size_t>(c.ScaleRoot)];
        for (uint32_t i = 0; i < aCount; ++i)
        {
            if (c.Excluded[i])
                continue;
            bool under = c.ScaleRoot == -1 || static_cast<int32_t>(i) == c.ScaleRoot;
            int32_t up = c.FlatParent[i];
            for (uint32_t depth = 0; !under && up >= 0 && static_cast<uint32_t>(up) < aCount && depth <= aCount; ++depth)
            {
                under = up == c.ScaleRoot;
                up = c.FlatParent[static_cast<size_t>(up)];
            }
            if (under && c.Node[i] && c.Node[i] != scaleNode && !UnderRoot(reinterpret_cast<void*>(c.Node[i]), reinterpret_cast<void*>(scaleNode)))
                ++scaleNotUnder;
            c.ScaleUnder[i] = under;
            scaleEntries += under ? 1u : 0u;
        }
        if (scaleNotUnder)
        {
            c.ScaleRoot = -2;
            c.ScaleUnder.fill(false);
            scaleEntries = 0;
        }
    }
    constexpr std::array<int, 2> fingerHands{VRPose::kLeftHand, VRPose::kRightHand}, feet{VRPose::kLeftFoot, VRPose::kRightFoot};
    for (size_t h = 0; h < 2; ++h)
    {
        auto& chain = c.FingerEntry[h];
        bool found = BodyPoseCommit::FindFingerChains(c.FlatParent, aCount, c.Bone[fingerHands[h]], chain);
        for (size_t k = 0; found && k < chain.size(); ++k)
        {
            const auto e = static_cast<size_t>(chain[k]);
            const int16_t p = c.FlatParent[e];
            found = !c.Excluded[e] && c.Wire[e] < 0 && p >= 0 && c.SolverParent[e] == p &&
                    (!c.Node[e] || (c.Node[static_cast<size_t>(p)] && c.NodeParent[e] == c.Node[static_cast<size_t>(p)]));
        }
        if (!found)
            chain.fill(-1);
        c.FingersFound[h] = found;
        c.ToeEntry[h] = BodyPoseCommit::UniqueChild(c.FlatParent, aCount, c.Bone[feet[h]]);
    }
    uint32_t collisionEntries{};
    for (uint32_t i = 0; i < aCount; ++i)
    {
        uintptr_t collision{};
        if (c.Node[i] && ReadAt(reinterpret_cast<void*>(c.Node[i]), BodyPoseCommit::kNodeCollision, collision) && collision)
            ++collisionEntries;
    }
    c.RebuildForRebind = false;
    c.Root = aRoot;
    c.Tree = aTree;
    c.Entries = aEntries;
    c.Count = aCount;
    c.TopologyId = s_commitTopologyCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    c.TopologyValid = true;
    c.WitnessOrdinal = 0;
    BuildCommitSubtree(aRoot);         // every object under the actor root, ~1 ms once per topology
    BuildCommitSkin(aEntries, aCount); // skin coverage of this topology, from the skin instances' bone lists
    if (CommitRigidAvailable())
        BuildCommitRigid(acPass, aRecord.Sequence); // rigid descendants of the written linked nodes, outside-flat names
    else
        c.RigidCount = c.RigidParentCount = c.RigidLastCount = 0; // rigid pass off by its guards: no rigid record
    aReason = CommitReason::Committed;
    return true;
}
bool CommitTopologyMatches(uintptr_t aRoot, uintptr_t aTree, uintptr_t aEntries, uint32_t aCount) noexcept
{
    const auto& c = *s_commitState;
    if (!c.TopologyValid || c.Root != aRoot || c.Tree != aTree || c.Entries != aEntries || c.Count != aCount)
        return false;
    for (uint32_t i = 0; i < aCount; ++i)
        if (CommitRawAt<uintptr_t>(c.Raw, i, BodyPoseCommit::kEntryNode) != c.Node[i] || CommitRawAt<int16_t>(c.Raw, i, BodyPoseCommit::kEntryParent) != c.FlatParent[i])
            return false;
    uintptr_t up{};
    return ReadAt(reinterpret_cast<void*>(aRoot), BodyPoseCommit::kNodeParent, up) && up == c.RootParent;
}

// after the flat stores of a committed pass, recompute every rigid descendant as committed parent world x native
// local and store its world (+7C) only. Each item is re-validated in this pass before its store: its vtable and parent
// pointer are the recorded ones, its parent's child array is unchanged (so the parent, itself a linked node re-read from
// the entry array this pass or an item validated earlier in this pass, still holds it), and its native world is exactly
// what the engine composed from the parent's pre-commit world and its local. Locals, Havok, the root: never written.
// an item owning a collision object (still the recorded one) needs that compose bit for bit (collision_composed);
// the collision object, its body and its Havok state are never read through or written, only the node's world +7C.
void WriteCommitRigid(const WorldPass& acPass, CommitRecord& aRecord) noexcept
{
    auto& c = *s_commitState;
    using BodyPoseCommit::RigidSkip;

    c.RigidLastCount = 0;
    if (!c.RigidCount)
        return;
    for (uint32_t p = 0; p < c.RigidParentCount; ++p)
    {
        c.RigidParentOk[p] = false;
        const int32_t s = c.RigidParentShape[p];
        if (s < 0 || static_cast<uint32_t>(s) >= c.ShapeCount)
            continue;
        const auto& shape = c.Shapes[static_cast<size_t>(s)];
        uintptr_t data{};
        uint16_t bound{};
        c.RigidParentOk[p] = shape.Node == c.RigidParentNode[p] && ReadCommitChildren(shape.Node, data, bound) && data == shape.Array && bound == shape.Bound &&
                             BodyPoseCommit::ChildHash(c.ChildScratch.data(), bound) == shape.Hash;
        if (!c.RigidParentOk[p])
            ++aRecord.RigidParentsChanged;
    }
    if (aRecord.RigidParentsChanged && !c.SubtreeChangedPending)
    {
        // An attach/detach under a written node: refuse that parent's items now and rebuild like a detected subtree change.
        c.SubtreeChangedPending = true;
        c.SubtreeComplete = false;
    }
    for (uint32_t n = 0; n < c.RigidCount; ++n)
    {
        auto& read = c.RigidReads[n];
        read = {};
        const auto& item = c.RigidItems[n];
        if (item.Static != RigidSkip::None)
            continue;
        if (item.ParentEntry >= 0 && !c.Selected[static_cast<size_t>(item.ParentEntry)])
            continue; // parent left native: nothing to do
        read.Readable = Read(c.RigidObject[n], c.RigidRaw.data(), c.RigidRaw.size());
        if (!read.Readable)
            continue;
        uintptr_t vtable{}, parent{}, collision{};
        std::memcpy(&vtable, c.RigidRaw.data(), sizeof(vtable));
        std::memcpy(&parent, c.RigidRaw.data() + BodyPoseCommit::kNodeParent, sizeof(parent));
        std::memcpy(&collision, c.RigidRaw.data() + BodyPoseCommit::kNodeCollision, sizeof(collision));
        std::memcpy(read.Local.data(), c.RigidRaw.data() + BodyPoseCommit::kNodeLocal, BodyPoseCommit::kTransformBytes);
        std::memcpy(read.World.data(), c.RigidRaw.data() + BodyPoseCommit::kNodeWorld, BodyPoseCommit::kTransformBytes);
        std::memcpy(&read.Flags, c.RigidRaw.data() + BodyPoseCommit::kNodeFlags, sizeof(read.Flags));
        // the collision object must still be the one recorded at the build (attached or replaced since: changed).
        bool attached = c.RigidParentSlot[n] < c.RigidParentCount && c.RigidParentOk[c.RigidParentSlot[n]];
        if (!attached && c.RigidParentSlot[n] < c.RigidParentCount && parent == c.RigidParent[n])
        {
            // The parent's children changed (an arrow attached or let go): an item its current children still hold is still attached,
            // the proof the build uses, so it keeps following the body instead of waiting for the rebuild at the flat pose.
            uintptr_t childData{};
            uint16_t childBound{};
            attached = ReadCommitChildren(parent, childData, childBound) &&
                       std::find(c.ChildScratch.begin(), c.ChildScratch.begin() + childBound, c.RigidObject[n]) != c.ChildScratch.begin() + childBound;
        }
        read.Matches = vtable == c.RigidVtable[n] && parent == c.RigidParent[n] && collision == c.RigidCollision[n] && attached;
        read.Collision = collision != 0;
    }
    BodyPoseCommit::SolveRigid(c.RigidItems, c.RigidCount, c.RigidReads, c.Selected, c.Refused, c.Target, c.Baseline, c.Count, c.RigidResults, kCommitMaxTravel);
    uint32_t refused{};

    for (uint32_t n = 0; n < c.RigidCount; ++n)
    {
        const auto& r = c.RigidResults[n];

        if (!r.Valid)
        {

            refused += r.Reason == RigidSkip::Unreadable || r.Reason == RigidSkip::Changed || r.Reason == RigidSkip::NotComposed || r.Reason == RigidSkip::Invalid ? 1u : 0u;
            continue;
        }

        if (!r.Store)
        {
            continue;
        }
        if (!BodyPoseCommit::Finite(r.Target))
        {
            continue;
        } // unreachable: validated
        std::memcpy(reinterpret_cast<void*>(c.RigidObject[n] + BodyPoseCommit::kNodeWorld), r.Target.data(), BodyPoseCommit::kTransformBytes);
        ++c.RigidLastCount;
    }
    c.RigidLastCommit = aRecord.Sequence;
}
// Clear rigid store bookkeeping after the native pass resumes control.
void ReleaseCommitRigid(CommitRecord&) noexcept
{
    s_commitState->RigidLastCount = 0;
}
// Read Havok ownership directly. Actor state can miss a ragdoll started by Papyrus.
void CommitHavokScan(CommitRecord& aRecord, uint32_t aCount) noexcept
{
    auto& c = *s_commitState;
    using BodyPoseCommit::HavokState;
    for (uint32_t i = 0; i < aCount; ++i)
    {
        if (!c.Node[i] || c.Excluded[i])
            continue;
        BodyPoseCommit::HavokRead read{};
        std::memcpy(&read.Collision, c.NodeRaw[i].data() + (BodyPoseCommit::kNodeCollision - BodyPoseCommit::kNodeParent), sizeof(uintptr_t));
        if (read.Collision)
        {
            read.BodyRead = Read(read.Collision + BodyPoseCommit::kHavokBodyOffset, &read.Body, sizeof(read.Body));
            if (read.BodyRead && read.Body)
            {
                read.ObjectRead = Read(read.Body + BodyPoseCommit::kHavokObjectOffset, &read.Object, sizeof(read.Object));
                if (read.ObjectRead && read.Object)
                {
                    read.StateRead = Read(read.Object + BodyPoseCommit::kHavokWorldOffset, c.HavokRaw.data(), c.HavokRaw.size());
                    if (read.StateRead)
                    {
                        std::memcpy(&read.World, c.HavokRaw.data(), sizeof(read.World));
                        read.Motion = c.HavokRaw[BodyPoseCommit::kHavokMotionOffset - BodyPoseCommit::kHavokWorldOffset];
                    }
                }
            }
        }
        const auto state = BodyPoseCommit::ClassifyHavok(read);
        if (state == HavokState::NoBody)
            continue;

        aRecord.HavokInWorld += state == HavokState::InWorld ? 1u : 0u;

        aRecord.HavokUnreadable += state == HavokState::Unreadable ? 1u : 0u;
    }
}
// A new copy in the slot (Publish, Forget) starts its commit state over. The previous copy's passes are no longer
// admitted (the slot's FormID changed) and its last committed worlds were recomposed by its own native traversal, so
// nothing of it is kept: no topology (the next pass rebuilds from scratch, cause selected_rebind), no live window (its
// release check cannot be taken on another actor), no rigid entry witness, no flip-guard ring. Under s_commitBusy.
void SyncCommitSelected(BodySlot& aSlot, CommitState& aState) noexcept
{
    const uint64_t epoch = aSlot.Epoch.load(std::memory_order_acquire);
    if (epoch == aState.SelectedEpoch)
        return;
    aState.SelectedEpoch = epoch;
    aState.RebindPending = true;
    aState.RebindDroppedLive = aState.RebindDroppedLive || aState.Live;
    aState.RebuildForRebind = true;
    aState.TopologyValid = false;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        WithdrawDwellLocked(aSlot);
    }
    aState.Live = false;
    aState.RigidLastCount = 0;
    aState.SubtreeChangedPending = false;
    aState.Flip.Reset();
}
void SetCommitFlipState(CommitRecord& aRecord, const BodyPoseCommit::FlipDecision& acDecision) noexcept
{
    aRecord.FlipHeld = acDecision.Hold;
}
// One externally visible pass, one guard state/counter update. A discarded freshness-only build attempt
// must not advance the guard ring, replay events or leak statistics before its cached-input rebuild.
void FinishCommitFlip(CommitRecord& aRecord) noexcept
{
    auto& c = *s_commitState;
    if (!c.PendingFlipReached)
        return;
    if (c.PendingFlipAdvanced)
        c.Flip = c.PendingFlip;
    aRecord.FlipHeld = c.PendingFlipDecision.Hold;
}
// After a commit's stores (same pass, same store lock): the native update left each world bound (+E4) around the native
// pose, so the engine culled a posed part by it (a raised hand's mesh vanished in a close-up). Grow the bound of every
// skinned geometry of the copy (the topology's skin meshes, still attached to the same skin instance) and of each node
// from it up to and including the actor root, to the union with a sphere around the written bone worlds plus
// BodyPoseBounds::kWrittenPaddingUnits. A union only grows, so the stored bound never culls more than native; a bound
// that already holds the sphere is left untouched (no store). World
// bounds only: no transform, flag or Havok field is written. No release path: the next native traversal recomputes the
// bounds from its own pose [S: "normal descent already aggregated worldBound+E4" (post-root design review), and a
// native body drawn where it moves; not re-read in the image], so a grown bound lasts until then and over-draws only.
// Nothing above the actor root is written: those scene nodes are shared and other jobs may write them, and the culling
// seen was per geometry (the body mesh's forearm drew in the same close-ups).
uint32_t RefreshCommitBounds(size_t aWritten) noexcept
{
    auto& c = *s_commitState; // caller holds s_commitBusy and the presentation store lock
    BodyPoseBounds::PointBox points{};
    for (size_t w = 0; w < aWritten; ++w)
    {
        const auto& target = c.Target[c.Writes[w].Entry];
        points.Add(target[9], target[10], target[11]);
    }
    BodyPoseBounds::Sphere posed{};
    if (!c.TopologyValid || !points.Enclosing(BodyPoseBounds::kWrittenPaddingUnits, posed))
        return 0;
    std::array<uintptr_t, kCommitSkinMeshes + kMaxDepth> visited{};
    size_t visitedCount{};
    uint32_t stores{};
    const auto grow = [&](uintptr_t aObject) noexcept
    {
        if (std::find(visited.begin(), visited.begin() + visitedCount, aObject) != visited.begin() + visitedCount)
            return;
        if (visitedCount == visited.size())
            return;
        visited[visitedCount++] = aObject;
        BodyPoseBounds::Sphere native{}, grown{};
        if (!Read(aObject + BodyPoseBounds::kNodeWorldBound, native.data(), sizeof(native)) || !BodyPoseBounds::Union(native, posed, grown) ||
            BodyPoseBounds::SameBits(grown, native))
            return; // unreadable, non-finite or empty (radius <= 0): native kept
        std::memcpy(reinterpret_cast<void*>(aObject + BodyPoseBounds::kNodeWorldBound), grown.data(), sizeof(grown));
        ++stores;
    };
    for (uint32_t m = 0; m < c.SkinMeshCount; ++m)
    {
        const auto& mesh = c.SkinMeshes[m];
        uintptr_t attached{};
        if (!ReadAt(reinterpret_cast<void*>(mesh.Geometry), BodyPoseCommit::kGeometrySkin, attached) || attached != mesh.Skin)
            continue;
        std::array<uintptr_t, kMaxDepth> path{};
        size_t length{};
        bool reached{};
        for (uintptr_t at = mesh.Geometry; at && length < path.size();)
        {
            path[length++] = at;
            if (at == c.Root)
            {
                reached = true;
                break;
            }
            if (!ReadAt(reinterpret_cast<void*>(at), BodyPoseCommit::kNodeParent, at))
                break;
        }
        if (!reached)
            continue; // detached or moved since the topology: not this copy's any more
        for (size_t k = 0; k < length; ++k)
            grow(path[k]);
    }
    return stores;
}
CommitReason CommitBuildAndStore(WorldPass& aPass, CommitRecord& aRecord) noexcept
{
    auto& c = *s_commitState;
    c.PendingFlipReached = c.PendingFlipAdvanced = false;
    // Freshness first, so a fresh frame that fails any later gate is visible as a missing commit.
    aRecord.FreshAtCommit = !aPass.ObservationOnly && aPass.SourceBinding && FreshSelectedSource(aPass);
    aRecord.RigidEnabled = CommitRigidAvailable();
    if (!s_bodyCommitReady)
        return CommitReason::NotReady;
    if (!aRecord.Ticket || !aRecord.RootPost)
        return CommitReason::Scope;
    // Grace cannot turn a borrowed tree address into lifetime authority: the exact tree must have
    // executed in THIS qualified retained-root traversal, with stable entry-array endpoints.
    if (aPass.GraceHold && (!aPass.CommitTreeQualified || aPass.CommitTreeRejected))
        return CommitReason::Scope;
    if (aPass.ObservationOnly || !aPass.SourceBinding)
        return CommitReason::NoSource;
    if (!PresentationSelectedSource(aPass))
    {
        if (aPass.DwellBlocked)
        {
            aRecord.FreshAtCommit = false;
            return CommitReason::NoSource;
        }
        return SteadyMs() > aPass.SourceBinding->ExpiresAtSteadyMs ? CommitReason::Expired : CommitReason::Identity;
    }
    // A pass admitted for the slot's previous copy cannot store after it changed hands.
    auto& bodySlot = s_slots[aPass.Slot];
    if (aPass.Identity.BoundActor != bodySlot.Actor.load(std::memory_order_acquire))
        return CommitReason::Identity;
    ULONG_PTR low{}, high{};
    GetCurrentThreadStackLimits(&low, &high);
    const auto here = reinterpret_cast<uintptr_t>(&low);

    if (here <= low || here - low < kCommitStackReserve)
        return CommitReason::Stack;
    const auto& binding = *aPass.SourceBinding;
    const uintptr_t root = aPass.Identity.BindingRootHint, tree = aPass.Identity.Tree;
    if (!root || !tree || binding.TreeHint != tree || binding.RootHint != root || !IsTree(reinterpret_cast<void*>(tree)))
        return CommitReason::Tree;
    uint32_t count{};
    uintptr_t entries{};
    if (!ReadAt(reinterpret_cast<void*>(tree), BodyPoseCommit::kTreeCount, count) || !ReadAt(reinterpret_cast<void*>(tree), BodyPoseCommit::kTreeEntries, entries))
        return CommitReason::Tree;

    if (aPass.GraceHold && (count != aPass.CommitTreeCount || entries != aPass.CommitTreeEntries))
        return CommitReason::TreeChanged;
    if (!count || !entries || count > kCommitEntries || entries > UINTPTR_MAX - count * BodyPoseCommit::kEntryStride)
        return CommitReason::Capacity;
    if (!Read(entries, c.Raw.data(), count * BodyPoseCommit::kEntryStride))
        return CommitReason::Tree;
    uint32_t life{};
    if (!ReadAt(reinterpret_cast<void*>(aPass.Identity.OwnedActor), offsetof(Actor, actorState) + offsetof(ActorState, flags1), life) || (life & (0x1E00000u | 0xE000000u)))
        return CommitReason::ActorState;
    const bool flatMatches = CommitTopologyMatches(root, tree, entries, count);
    if (aPass.GraceHold && (!flatMatches || c.SubtreeChangedPending || !c.Live))
        return CommitReason::Topology;
    const bool subtreeDue = c.SubtreeChangedPending && SteadyMs() >= c.SubtreeRebuildNotBeforeMs; // attach/detach seen earlier
    if (!flatMatches || subtreeDue)
    {
        // Observing replacement invalidates old committed eligibility BEFORE the fresh-path rebuild
        // updates the topology cache. A later expiry cannot hide this mismatch and retry the old pose.
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            InvalidateGraceLocked(aRecord.Actor);
        }
        aRecord.TopologyRebuilt = true;
        c.RebuildCause = c.RebuildForRebind ? 3u : !c.TopologyId ? 0u : !flatMatches ? 1u : 2u; // 3 selected_rebind
        CommitReason reason{};
        if (!UnderRoot(reinterpret_cast<void*>(tree), reinterpret_cast<void*>(root)))
            return CommitReason::Tree;
        if (!RebuildCommitTopology(aPass, root, tree, entries, count, aRecord, reason))
            return reason;
    }

    // The native baseline this pass just produced: node+7C for linked entries, entry+34 otherwise.
    for (uint32_t i = 0; i < count; ++i)
    {
        std::memcpy(c.EntryWorld[i].data(), c.Raw.data() + i * BodyPoseCommit::kEntryStride + BodyPoseCommit::kEntryWorld, BodyPoseCommit::kTransformBytes);
        if (!c.Node[i])
        {
            c.Baseline[i] = c.EntryWorld[i];
            continue;
        }
        uintptr_t parent{};
        if (!Read(c.Node[i] + BodyPoseCommit::kNodeParent, c.NodeRaw[i].data(), c.NodeRaw[i].size()))
        {
            ;
            return CommitReason::Topology;
        }
        std::memcpy(&parent, c.NodeRaw[i].data(), sizeof(parent));
        if (parent != c.NodeParent[i])
        {
            ;
            return CommitReason::Topology;
        }
        std::memcpy(c.Baseline[i].data(), c.NodeRaw[i].data() + kNodeRawWorld, BodyPoseCommit::kTransformBytes);
    }
    // Havok gate , before anything is solved or stored: a biped body in a Havok world, or one whose state
    // cannot be read, leaves this pass native (release check and expiry follow as for a source stop). Counted always,
    CommitHavokScan(aRecord, count);
    if (kCommitHavokGate && aRecord.HavokInWorld)
        return CommitReason::HavokWorld;
    if (kCommitHavokGate && aRecord.HavokUnreadable)
        return CommitReason::HavokUnreadable;
    std::array<float, 13> rootRaw{};
    BodyPosePresentation::Transform rootTransform;
    if (!Read(root + BodyPoseCommit::kNodeWorld, rootRaw.data(), sizeof(rootRaw)) || std::memcmp(rootRaw.data(), aPass.RootPostWorldValue.data(), sizeof(rootRaw)) != 0 ||
        !ToTransform(rootRaw, rootTransform))
        return CommitReason::RootChanged; // the actor root must still hold the world its own native update produced
    auto& in = c.In;
    in.Count = count;
    in.Parent.fill(BodyPosePresentation::kMissing);
    in.Bone.fill(BodyPosePresentation::kMissing);
    in.Rotations.fill(glm::quat{1.f, 0.f, 0.f, 0.f});
    in.RootRotation = rootTransform.Rotation;
    in.RootTranslation = rootTransform.Translation;
    in.HasData = binding.Sample.Pose.HasData;
    in.NoBones = binding.Sample.Pose.NoBones;
    in.HasLegs = binding.Sample.Pose.HasLegs;
    in.HasHips = binding.Sample.Pose.HasHips;
    in.HipOffset = {binding.Sample.Pose.HipOffset[0], binding.Sample.Pose.HipOffset[1], binding.Sample.Pose.HipOffset[2]};
    for (size_t wire = 0; wire < VRPose::kBoneCount; ++wire)
    {
        in.Rotations[wire] = static_cast<glm::quat>(binding.Sample.Pose.Bones[wire]);
        in.Bone[wire] = c.Bone[wire];
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!ToTransform(c.Baseline[i], in.Baseline[i]))
        {
            ;
            return CommitReason::Baseline;
        }
        in.Parent[i] = c.SolverParent[i];
        c.NativeT[i] = in.Baseline[i]; // native solver values, before the root-scale pass changes the input
    }
    c.PendingGracePose = BodyPoseCommit::FlipPoseFrom(in, aRecord.MonotonicMs, binding.Sample.Generation, binding.Sample.Before.SourceTick, binding.Sample.After.SourceTick);
    c.PendingGraceValid = aPass.CommitTreeQualified && !aPass.CommitTreeRejected && count == aPass.CommitTreeCount && entries == aPass.CommitTreeEntries &&
                          GraceSourcePoseValid(binding.Sample.Pose) && BodyPoseCommit::ValidGracePose(c.PendingGracePose);
    if (aPass.GraceHold)
    {
        BodyPoseCommit::ApplyFlipPose(aPass.GracePose, in);
        c.PendingGracePose = aPass.GracePose;
        if (kCommitFlipGuard)
        {
            aRecord.GraceGuardAssessmentMs = SteadyMs(); // invocation start can precede a newer committed cache after preemption
            if (!BodyPoseCommit::ReuseFlipDecision(
                    aPass.GraceGuardDecision, aPass.GraceGuardAssessedMs, aRecord.GraceGuardAssessmentMs, aPass.GracePose.TimeMs, c.PendingFlipDecision))
                return CommitReason::ExpiredInPass;
            c.PendingFlipReached = true; // evidence/statistics only: the ring and one-shot events stay unchanged

            SetCommitFlipState(aRecord, c.PendingFlipDecision);
        }
        aRecord.FlipHeld = aPass.GraceFlipHeld; // assign only after successful evidence reuse; refusals remain unchecked and unheld
    }
    if (!aPass.GraceHold && kCommitFlipGuard)
    {
        const auto candidate = BodyPoseCommit::FlipPoseFrom(in, aRecord.MonotonicMs, binding.Sample.Generation, binding.Sample.Before.SourceTick, binding.Sample.After.SourceTick);
        BodyPoseCommit::FlipPose use{};
        c.PendingFlip = c.Flip; // transactional until the final chosen attempt returns
        const auto d = c.PendingFlip.Step(candidate, use, BodyPoseCommit::kFlipImplausibleDeg, BodyPoseCommit::kFlipHoldTimeoutMs);
        c.PendingFlipDecision = d;
        c.PendingFlipReached = c.PendingFlipAdvanced = true;
        SetCommitFlipState(aRecord, d);
        if (d.Release)
            return d.Timeout ? CommitReason::FlipTimeout : CommitReason::FlipNoCandidate;
        if (d.Hold)
        {
            BodyPoseCommit::ApplyFlipPose(use, in);
            c.PendingGracePose = use;
        }
    }
    c.PendingGracePose = BodyPoseCommit::FlipPoseFrom(in, c.PendingGracePose.TimeMs, c.PendingGracePose.Generation, c.PendingGracePose.BeforeTick, c.PendingGracePose.AfterTick);
    c.PendingGraceValid = c.PendingGraceValid && BodyPoseCommit::ValidGracePose(c.PendingGracePose);
    // shape passes, in order: body size as world values, Solve (hip shift and the 19 rotations, as before), hand
    // value pass on the solver's input or output; the validation and stores below are the same for every written value.
    const auto& pose = binding.Sample.Pose;
    BodyPosePresentation::ScaleDecision scale{};

    if (pose.HasScale && c.ScaleRoot != -2)
    {
        // Body size (owner decision D4): ratio = sender skeleton-root world scale / this copy's skeleton-root world scale;
        // every scaled entry's native world is scaled about the skeleton root's world position before Solve (so the hip
        // delta is taken against the scaled pelvis and the legs swing at the sender's length). World values only: no local
        // or persistent scale is written; the next native traversal rebuilds the native scale.
        BodyPoseCommit::Transform skeleton{};
        if (c.ScaleRoot == -1)
        {
            if (!Read(tree + BodyPoseCommit::kNodeWorld, c.TreeWorld.data(), sizeof(c.TreeWorld)))
                return CommitReason::Tree; // re-checked before the stores
            std::memcpy(skeleton.data(), c.TreeWorld.data(), sizeof(skeleton));
        }
        else
            skeleton = c.Baseline[static_cast<size_t>(c.ScaleRoot)];

        aRecord.ScaleSender = static_cast<float>(pose.RootScale) / 1000.f;
        aRecord.ScaleNative = skeleton[12];
        scale = BodyPosePresentation::RootScaleRatio(aRecord.ScaleSender, aRecord.ScaleNative);
        if (scale.Applied && !BodyPosePresentation::ApplyRootScale(in, c.ScaleUnder, glm::vec3{skeleton[9], skeleton[10], skeleton[11]}, scale.Ratio))
            scale = BodyPosePresentation::ScaleDecision{1.f, false, false, false, true};
    }
    if (!BodyPosePresentation::Solve(in, c.Out, &c.HipExcluded))
        return CommitReason::Solve;

    bool shaped = scale.Applied;

    if (pose.HasHandCheck)
    {
        // the sender's hand offsets, with or without the correction), then upstream ReachHands' fine-tuning (15-unit cap).
        // Not on a pass the flip guard HELD: its offsets belong to the replaced sample, not to the held rotations.
        constexpr std::array<std::array<int, 3>, 2> arms{
            {{VRPose::kLeftUpperArm, VRPose::kLeftForearm, VRPose::kLeftHand}, {VRPose::kRightUpperArm, VRPose::kRightForearm, VRPose::kRightHand}}};

        for (size_t side = 0; side < arms.size(); ++side)
        {
            const float* offset = side ? pose.RightHandOffset : pose.LeftHandOffset;

            const glm::vec3 target = in.RootTranslation + in.RootRotation * glm::vec3{offset[0], offset[1], offset[2]};
            const auto hand = static_cast<size_t>(c.Bone[static_cast<size_t>(arms[side][2])]);

            if (aRecord.FlipHeld)
                continue;
            // A hand beyond the copy's straight arm: the shoulder comes towards it first, or the arm stops short of it.
            const float turn = HandReach::TurnClavicle(
                c.Out, in.Parent, c.Bone[side ? VRPose::kRightClavicle : VRPose::kLeftClavicle], c.Bone[static_cast<size_t>(arms[side][0])],
                c.Bone[static_cast<size_t>(arms[side][1])], static_cast<int>(hand), target);
            if (turn > 0.f)
                shaped = true;
            const auto r = BodyPosePresentation::ReachHand(
                c.Out, in.Parent, in.RootRotation, c.Bone[static_cast<size_t>(arms[side][0])], c.Bone[static_cast<size_t>(arms[side][1])], static_cast<int>(hand), target,
                glm::normalize(in.RootRotation * in.Rotations[static_cast<size_t>(arms[side][2])]));
            aRecord.Ik[side] = static_cast<uint8_t>(r.Outcome);

            shaped = shaped || r.Outcome == BodyPosePresentation::Reach::Applied || r.Outcome == BodyPosePresentation::Reach::Degenerate;
        }
    }

    {
        // Fingers: the 30 sent parent-relative rotations below each posed hand, each bone keeping its own native local
        // translation (flat-only: entry+00, the operand EF4FBB composes; linked: node+48) and world scale. The locals are
        // used only when this pass's native finger worlds are the engine's compose of them (BodyPoseCommit::ComposesFrom).
        constexpr std::array<int, 2> hands{VRPose::kLeftHand, VRPose::kRightHand};
        for (size_t h = 0; h < hands.size(); ++h)
        {
            auto& outcome = aRecord.Fingers[h];
            if (!pose.HasFingers)
            {
                outcome = static_cast<uint8_t>(CommitFingers::NoData);
                continue;
            }
            if (!c.FingersFound[h])
            {
                outcome = static_cast<uint8_t>(CommitFingers::NotFound);
                continue;
            }
            std::array<glm::vec3, BodyPosePresentation::kFingerBonesPerHand> local{};
            std::array<glm::quat, BodyPosePresentation::kFingerBonesPerHand> rotation{};
            bool composed = true;
            for (size_t k = 0; k < BodyPosePresentation::kFingerBonesPerHand && composed; ++k)
            {
                const auto e = static_cast<size_t>(c.FingerEntry[h][k]);
                const auto p = static_cast<size_t>(c.FlatParent[e]);
                BodyPoseCommit::Transform own{};
                if (c.Node[e])
                    std::memcpy(own.data(), c.NodeRaw[e].data() + (BodyPoseCommit::kNodeLocal - BodyPoseCommit::kNodeParent), sizeof(own));
                else
                    std::memcpy(own.data(), c.Raw.data() + e * BodyPoseCommit::kEntryStride, sizeof(own));
                bool exact{};
                composed = BodyPoseCommit::ComposesFrom(c.Node[e] ? c.Baseline[e] : c.EntryWorld[e], c.Node[e] ? c.Baseline[p] : c.EntryWorld[p], own, exact);

                local[k] = {own[9], own[10], own[11]};
                rotation[k] = static_cast<glm::quat>(pose.Fingers[h * BodyPosePresentation::kFingerBonesPerHand + k]);
            }
            if (!composed)
                outcome = static_cast<uint8_t>(CommitFingers::NotComposed);
            else if (!BodyPosePresentation::ApplyFingers(c.Out, c.Bone[static_cast<size_t>(hands[h])], c.FingerEntry[h], local, rotation))
                outcome = static_cast<uint8_t>(CommitFingers::Invalid);
            else
            {
                outcome = static_cast<uint8_t>(CommitFingers::Applied);
                shaped = true;
            }
        }
    }
    if (shaped && !BodyPosePresentation::ValidOutput(c.Out))
        return CommitReason::Solve;

    c.Selected.fill(false);
    c.Refused.fill(false);
    for (uint32_t i = 0; i < count; ++i)
    {
        c.Target[i] = c.Baseline[i];
        if (c.Excluded[i])
            continue; // actor root: native world, no hip delta, never written
        if (c.LeftToPlacement[i])
            continue; // a hand's attach node: the held-weapon placement writes it after this commit

        const auto& solved = c.Out.world[i];
        const auto& native = c.NativeT[i]; // the native values (in.baseline is scaled when the root-scale pass ran)
        // a scaled entry is never "untouched" (its translation and scale differ from native); its scale bits are the
        // native scale x the ratio, one float multiply, the same value ApplyRootScale put into the solver input.
        const bool scaledEntry = scale.Applied && c.ScaleUnder[i];
        if (!scaledEntry && solved.Rotation == native.Rotation && solved.Translation == native.Translation)
        {
            continue;
        } // untouched: exact native bits
        const float expectedScale = scaledEntry ? c.Baseline[i][12] * scale.Ratio : c.Baseline[i][12];
        auto target = c.Baseline[i]; // scale bits are never posed, except by the root-scale pass (expectedScale)
        target[12] = expectedScale;
        BodyPoseCommit::PackRotation(solved.Rotation, target); // native NiMatrix3 is row-major (same arithmetic as 's inline loop)
        target[9] = solved.Translation.x;
        target[10] = solved.Translation.y;
        target[11] = solved.Translation.z;
        if (!BodyPoseCommit::Finite(target))
        {
            ++aRecord.NonFiniteRejected;
            ++aRecord.SkippedInvalid;
            c.Refused[i] = true;
            continue;
        }
        if (!BodyPoseCommit::ValidTargetScale(target, c.Baseline[i], expectedScale, kCommitMaxTravel))
        {
            ++aRecord.SkippedInvalid;
            c.Refused[i] = true;
            continue;
        }
        if (BodyPoseCommit::SameBits(target, c.Baseline[i]))
        {
            continue;
        }
        c.Target[i] = target;
        c.Selected[i] = true;
    }
    if (aPass.GraceHold && aRecord.SkippedInvalid)
        return aRecord.NonFiniteRejected ? CommitReason::NonFinite : CommitReason::Solve;

    size_t written{};
    if (!BodyPoseCommit::SelectWrites(entries, c.Node, count, c.Selected, c.Writes, written))
        return CommitReason::Capacity;
    if (!written)
        return CommitReason::NoTargets;

    // Nothing may have replaced the native baseline, topology or root since they were read.
    if (!Read(entries, c.Raw2.data(), count * BodyPoseCommit::kEntryStride))
        return CommitReason::BaselineChanged;
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t at = i * BodyPoseCommit::kEntryStride + BodyPoseCommit::kEntryWorld;
        if (std::memcmp(c.Raw.data() + at, c.Raw2.data() + at, BodyPoseCommit::kEntryNode + sizeof(uintptr_t) - BodyPoseCommit::kEntryWorld) != 0)
        {
            ;
            return CommitReason::BaselineChanged;
        }
        if (!c.Node[i])
            continue;
        if (!Read(c.Node[i] + BodyPoseCommit::kNodeParent, c.NodeRaw2[i].data(), c.NodeRaw2[i].size()) ||
            std::memcmp(c.NodeRaw[i].data(), c.NodeRaw2[i].data(), sizeof(uintptr_t)) != 0 ||
            std::memcmp(c.NodeRaw[i].data() + kNodeRawWorld, c.NodeRaw2[i].data() + kNodeRawWorld, BodyPoseCommit::kTransformBytes) != 0)
        {
            ;
            return CommitReason::BaselineChanged;
        }
    }
    std::array<float, 13> rootAgain{};
    if (!Read(root + BodyPoseCommit::kNodeWorld, rootAgain.data(), sizeof(rootAgain)) || std::memcmp(rootAgain.data(), rootRaw.data(), sizeof(rootRaw)) != 0)
        return CommitReason::BaselineChanged;
    if (scale.Applied && c.ScaleRoot == -1)
    {
        // the skeleton root's world the scale pivot and ratio came from (the tree's own world) is still the same.
        std::array<float, 13> treeAgain{};
        if (!Read(tree + BodyPoseCommit::kNodeWorld, treeAgain.data(), sizeof(treeAgain)) || std::memcmp(treeAgain.data(), c.TreeWorld.data(), sizeof(treeAgain)) != 0)
            return CommitReason::BaselineChanged;
    }
    for (size_t w = 0; w < written; ++w)
        if (!BodyPoseCommit::Finite(c.Target[c.Writes[w].Entry]))
            return CommitReason::NonFinite; // unreachable by construction; never store
    auto* slot = ReserveCommitWriteSlot();
    if (!slot)
        return CommitReason::WriteSlotBusy;
    const uint64_t witnessOrdinal = c.WitnessOrdinal;
    const auto abandon = [slot, witnessOrdinal](CommitReason aReason) noexcept
    {
        if (aReason == CommitReason::ExpiredInPass || aReason == CommitReason::NoSource)
        {
            s_commitState->WitnessOrdinal = witnessOrdinal;
        }
        slot->Readers.store(0, std::memory_order_release);
        return aReason; // never published
    };
    BeginCommitWriteSlot(*slot, 1, aRecord, &aPass);
    for (size_t w = 0; w < written; ++w)
    {
        const auto& write = c.Writes[w];
        if (!slot->Addresses.Insert(write.Address, static_cast<int16_t>(w)))
        {

            return abandon(CommitReason::Topology);
        }
    }
    slot->Written = static_cast<uint32_t>(written);
    // Check source admission and the tree's entry array again after reserving the write slot.
    aRecord.SourceCheckedMs = SteadyMs();
    aRecord.FreshAtCommit = FreshSelectedSource(aPass); // truthful receive freshness, including on a successful grace commit
    if (!PresentationSelectedSource(aPass))
    {
        if (aPass.DwellBlocked)
        {
            aRecord.FreshAtCommit = false;
            return abandon(CommitReason::NoSource);
        }
        return abandon(CommitReason::ExpiredInPass);
    }
    uint32_t countNow{};
    uintptr_t entriesNow{};
    if (!IsTree(reinterpret_cast<void*>(tree)) || !ReadAt(reinterpret_cast<void*>(tree), BodyPoseCommit::kTreeCount, countNow) ||
        !ReadAt(reinterpret_cast<void*>(tree), BodyPoseCommit::kTreeEntries, entriesNow) || countNow != count || entriesNow != entries)
        return abandon(CommitReason::TreeChanged);
    aRecord.RigidEnabled = CommitRigidAvailable();
    // Serialize final admission and ALL body-commit flat/rigid stores with lifecycle publication.
    // The rigid closure is bounded RPM/value math/plain stores only: no engine callback or registry re-entry.
    std::unique_lock<std::mutex> presentationStore(s_mutex);
    const double sourceChecked = SteadyMs();
    aRecord.SourceCheckedMs = sourceChecked;
    aRecord.FreshAtCommit = sourceChecked <= binding.ExpiresAtSteadyMs;
    if (bodySlot.Identity.Revision.load(std::memory_order_acquire) != aPass.IdentityRevision)
        return abandon(CommitReason::Identity);
    aRecord.RigidEnabled = CommitRigidAvailable();
    if ((!aPass.GraceHold && !aRecord.FreshAtCommit) || (aPass.GraceHold && sourceChecked > binding.ExpiresAtSteadyMs + BodyPoseCommit::kPresentationGraceMs))
        return abandon(CommitReason::ExpiredInPass);
    if (aPass.GraceHold &&
        (bodySlot.Grace.Source != aPass.SourceBinding || bodySlot.Grace.Window.Observe(GraceKeyOf(binding), sourceChecked, true) != BodyPoseCommit::GraceAdmission::Hold ||
         (aPass.GraceFlipHeld && sourceChecked - aPass.GracePose.TimeMs > BodyPoseCommit::kFlipHoldTimeoutMs)))
        return abandon(CommitReason::ExpiredInPass); // Invalidation during lock acquisition cannot revive a hold
    if (!DwellAdmissionLocked(aPass, aRecord.FreshAtCommit, aPass.GraceHold))
    {
        aRecord.FreshAtCommit = false;
        return abandon(CommitReason::NoSource);
    }
    const uint64_t dwellTransition = bodySlot.Dwell.Transition;

    PublishCommitWriteSlot(*slot, true);
    // The commit: plain stores of validated 52-byte values, inside this native root scope only.
    for (size_t w = 0; w < written; ++w)
    {
        const auto& value = c.Target[c.Writes[w].Entry];
        std::memcpy(reinterpret_cast<void*>(c.Writes[w].Address), value.data(), BodyPoseCommit::kTransformBytes);
    }
    slot->Committing.store(0, std::memory_order_release);
    bodySlot.Dwell.Commit(bodySlot.Dwell.LastNowMs, bodySlot.Dwell.LastPresentFrame, aRecord.FreshAtCommit, aPass.GraceHold, dwellTransition);
    bodySlot.DwellActor = aRecord.Actor;

    if (aRecord.RigidEnabled)
        WriteCommitRigid(aPass, aRecord); // rigid descendants from the committed parent worlds, after every flat store
    else if (c.Live && c.RigidLastCount && c.RigidLastCommit == c.LastCommit)
        ReleaseCommitRigid(aRecord); // rigid just switched off
    RefreshCommitBounds(written);    // after every bone store of this pass: bounds only, never bones
    presentationStore.unlock();      // every presentation store is complete before lifecycle can publish
    return CommitReason::Committed;
}
void CopyCommitSource(const WorldPass& acPass, CommitRecord& aRecord) noexcept
{

    if (acPass.SourceBinding)
    {
        const auto& source = *acPass.SourceBinding;

        aRecord.SourceBefore = source.Sample.Before.SourceTick;
        aRecord.SourceAfter = source.Sample.After.SourceTick;
        aRecord.SourceDeadlineMs = source.ExpiresAtSteadyMs;
    }
}
void CaptureCommitDwell(BodySlot& aSlot) noexcept
{
    std::lock_guard<std::mutex> guard(s_mutex);
    const auto now = SteadyMs();
    aSlot.Dwell.Observe(now, s_presentFrame.load(std::memory_order_acquire));
}
// Preserve the pre-traversal rebind synchronization; no survival readback is needed for presentation.
void CommitPreRoot() noexcept
{
    auto* pass = CurrentWorldPass();
    if (!pass || !pass->CommitWatch || !s_bodyCommitReady)
        return;
    uint32_t idle{};
    if (!s_commitBusy.compare_exchange_strong(idle, 1, std::memory_order_acquire))
        return;
    struct Release
    {
        ~Release() { s_commitBusy.store(0, std::memory_order_release); }
    } release;
    if (s_slots[pass->Slot].Actor.load(std::memory_order_acquire) == pass->Identity.BoundActor)
        SyncCommitSelected(s_slots[pass->Slot], s_commits[pass->Slot]);
}
// Another copy's commit, on another of the game's job threads, holds the section for a short while: wait for it, never
// longer than kCommitSectionWait, so no engine lock either thread holds can stall the other for good.
constexpr auto kCommitSectionWait = std::chrono::milliseconds(1);
bool EnterCommitSection() noexcept
{
    const auto cUntil = Clock::now() + kCommitSectionWait;
    for (;;)
    {
        uint32_t idle{};
        if (s_commitBusy.compare_exchange_weak(idle, 1, std::memory_order_acquire))
            return true;
        if (Clock::now() >= cUntil)
            return false;
        _mm_pause();
    }
}
// True when this pass committed its copy.
bool CommitPostRoot() noexcept
{
    auto* pass = CurrentWorldPass();
    if (!pass || !pass->CommitWatch)
        return false;
    // A pass for a copy whose slot changed hands since admission leaves the new copy's state alone.
    auto& slot = s_slots[pass->Slot];
    if (slot.Actor.load(std::memory_order_acquire) != pass->Identity.BoundActor)
        return false;
    const double started = SteadyMs();
    CommitRecord rec{};
    rec.MonotonicMs = started;
    rec.Ticket = pass->CommitRootTicket;
    rec.RootPost = pass->RootPostWorldObserved && pass->RootPostWorldReadable;
    rec.Sequence = s_jobSequence.fetch_add(1, std::memory_order_relaxed) + 1;

    rec.PassFrame = pass->Identity.Frame;

    rec.Actor = pass->Identity.BoundActor;

    rec.Root = pass->Identity.BindingRootHint;
    rec.Tree = pass->Identity.Tree;

    CopyCommitSource(*pass, rec);

    if (!EnterCommitSection())
    {
        rec.Reason = CommitReason::Busy;
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            WithdrawDwellLocked(rec.Actor);
            InvalidateGraceLocked(rec.Actor);
        }
        CaptureCommitDwell(slot);
        return false;
    }
    struct Release
    {
        ~Release()
        {
            s_commitState = nullptr;
            s_commitBusy.store(0, std::memory_order_release);
        }
    } release;
    s_commitState = &s_commits[pass->Slot];
    auto& c = *s_commitState;
    SyncCommitSelected(slot, c); // a new copy in the slot since the last pass starts the state over (no-op otherwise)

    c.RebindPending = c.RebindDroppedLive = false;

    const bool rigidPending = c.RigidLastCount && c.RigidLastCommit == c.LastCommit; // the last commit's rigid stores
    c.RetryRecord = rec;
    rec.Reason = CommitBuildAndStore(*pass, rec);
    if (pass->SourceBinding &&
        BodyPoseCommit::CanRetryWithGrace(
            rec.Reason == CommitReason::Expired || rec.Reason == CommitReason::ExpiredInPass, pass->GraceHold, SteadyMs(), pass->SourceBinding->ExpiresAtSteadyMs) &&
        WorldIdentityMatches(*pass) && AcquireSelectedBinding(*pass) && pass->GraceHold && PresentationSelectedSource(*pass))
    {
        // The first attempt stored nothing. Preserve a real topology rebuild, discard all tentative
        // per-pass solve/guard fields and rebuild from the last successful immutable input pin only.
        const bool topologyRebuilt = rec.TopologyRebuilt;
        rec = c.RetryRecord;
        rec.TopologyRebuilt = topologyRebuilt;
        CopyCommitSource(*pass, rec);
        rec.Reason = CommitBuildAndStore(*pass, rec); // at most one retry, no new blend or deadline
    }
    FinishCommitFlip(rec);
    rec.Committed = rec.Reason == CommitReason::Committed;

    // The weapons and the arrow where the owner holds them, from the hands just committed; not once the pass is switched off or stale.
    // Inside the commit section (s_commitBusy), committed passes only. A rebuilt or changed tree has the weapon nodes looked up afresh.
    if (rec.Committed && pass->SourceBinding && slot.Identity.Revision.load(std::memory_order_acquire) == pass->IdentityRevision)
        VRBodySync::PlaceHeldWeapons(rec.Actor, reinterpret_cast<void*>(rec.Root), pass->SourceBinding->Sample.Pose, rec.TopologyRebuilt || c.SubtreeChangedPending);
    else
        VRBodySync::ReleaseHeldWeapons(rec.Actor);

    if (rec.Committed)
    {
        c.Live = true;
        c.LastCommit = rec.Sequence;
        c.LastCommitFrame = rec.PassFrame;
        // Only a pass that put the hands on the sender's: a commit that left them where the arm rotations alone put
        // them (no hand offsets, hand IK off or capped, flip guard holding) does not count.
        constexpr auto cApplied = static_cast<uint8_t>(BodyPosePresentation::Reach::Applied);
        const uint32_t cSides = (rec.Ik[0] == cApplied ? 1u : 0u) | (rec.Ik[1] == cApplied ? 2u : 0u);
        if (slot.Actor.load(std::memory_order_acquire) == rec.Actor)
        {
            slot.PosedHands.store(cSides ? PosedHandsWord(rec.Actor, EngineFrame(), cSides) : 0, std::memory_order_release);
            slot.CommittedBody.store(RemoteBodyPoser::CommitWord(rec.Actor, EngineFrame()), std::memory_order_release);
        }

        std::lock_guard<std::mutex> guard(s_mutex);
        // Held commits never arm or renew. A completed store cannot undo lifecycle invalidation.
        auto& grace = slot.Grace;
        if (!pass->GraceHold && pass->SourceBinding && slot.Identity.Revision.load(std::memory_order_acquire) == pass->IdentityRevision &&
            grace.Window.CommittedFreshCompletion(
                GraceKeyOf(*pass->SourceBinding), rec.SourceBefore, rec.SourceAfter, rec.SourceDeadlineMs, rec.SourceCheckedMs, SteadyMs(),
                rec.FreshAtCommit && c.PendingGraceValid && !rec.SkippedInvalid && !rec.NonFiniteRejected && !c.SubtreeChangedPending))
        {
            grace.Source = pass->SourceBinding;
            grace.Pose = c.PendingGracePose;
            grace.FlipHeld = rec.FlipHeld;
            grace.GuardDecision = c.PendingFlipDecision;
            grace.GuardAssessedMs = rec.MonotonicMs;
            grace.SourceCommit = rec.Sequence;
            grace.LastNetworkMs = pass->SourceBinding->PublishedNetworkMs;
        }
        else if (!pass->GraceHold)
            InvalidateGraceLocked(rec.Actor);
    }
    else
    {
        if (c.Live && rigidPending)
            ReleaseCommitRigid(rec); // the rigid worlds must no longer hold the stale committed values
        c.Live = false;
        WithdrawCommitted(rec.Actor); // frame end poses the copy in this same frame
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            WithdrawDwellLocked(rec.Actor);
            InvalidateGraceLocked(rec.Actor);
        }
    }
    if (c.TopologyValid && !rec.TopologyRebuilt && !c.SubtreeChangedPending && rec.Root == c.Root)
    {
        // Invalidate the current topology when an attachment changes.
        bool changed{};
        VerifyCommitSubtreeSlice(changed);
        if (changed)
        {
            c.SubtreeChangedPending = true;
            c.SubtreeComplete = false;

            {
                std::lock_guard<std::mutex> guard(s_mutex);
                InvalidateGraceLocked(rec.Actor);
            }
        }
    }
    CaptureCommitDwell(slot);
    return rec.Committed;
}
bool InstallCommitJobHooks()
{
    auto* worker = reinterpret_cast<uint8_t*>(s_imageBase + 0x784240);
    auto* lookup = reinterpret_cast<uint8_t*>(s_imageBase + 0x78426D);
    auto* get3D = reinterpret_cast<uint8_t*>(s_imageBase + 0x78435B);
    auto* dispatch = reinterpret_cast<uint8_t*>(s_imageBase + 0x6AFF4F);
    const auto fits = [](const uint8_t* apSite, const auto aHook) noexcept
    {
        return CallReaches(apSite, reinterpret_cast<uintptr_t>(aHook));
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!s_engineFrame || !Read(s_imageBase, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || !Read(s_imageBase + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage < 0x33363F8 ||
        !PatchCheck::Is(worker, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50}) ||
        !PatchCheck::Is(lookup, {0xE8, 0x4E, 0x34, 0xAE, 0xFF, 0x90, 0x41, 0x8D, 0x6C, 0x24, 0xFF}) ||
        !PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0x2676C0), {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x2677B0), {0x48, 0x83, 0x3F, 0x00}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x2677BE), {0x0F, 0x95, 0xC0, 0x48, 0x83, 0xC4, 0x20, 0x41, 0x5F, 0x5F, 0x5E, 0xC3}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x784355), {0x48, 0x8B, 0x03, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0x80, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xF8}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x78436E), {0xF0, 0xFF, 0x40, 0x08}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x784453), {0xF0, 0x0F, 0xC1, 0x47, 0x08}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x6AFF38), {0x4C, 0x8B, 0x08, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xC8, 0xF3, 0x0F, 0x11,
                                                                             0x44, 0x24, 0x38, 0xC7, 0x44, 0x24, 0x3C, 0x01, 0x20, 0x00, 0x00, 0x41,
                                                                             0xFF, 0x91, 0x60, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3}) ||
        TiltedPhoques::GetCall<JobLookup*>(lookup) != reinterpret_cast<JobLookup*>(s_imageBase + 0x2676C0) || !fits(lookup, &HookJobLookup) || !fits(get3D, &HookJobGet3D) ||
        !fits(dispatch, &HookJobDispatch))
    {
        spdlog::debug("SEBodyCommit: exact SE1.7.104 job guards failed; job hooks not installed");
        return false;
    }
    s_imageSize = nt.OptionalHeader.SizeOfImage;
    s_jobWorker = reinterpret_cast<JobWorker*>(worker);
    TP_HOOK_IMMEDIATE(&s_jobWorker, &HookJobWorker);
    if (s_jobWorker == reinterpret_cast<JobWorker*>(worker) || !EntryReachesHook(reinterpret_cast<uintptr_t>(worker), reinterpret_cast<uintptr_t>(&HookJobWorker)))
    {
        spdlog::debug("SEBodyCommit: worker hook not in place; job hooks off");
        return false;
    }
    s_jobLookup = reinterpret_cast<JobLookup*>(s_imageBase + 0x2676C0);
    const bool written = WriteJobCall(lookup, 5, reinterpret_cast<uintptr_t>(&HookJobLookup)) && WriteJobCall(get3D, 6, reinterpret_cast<uintptr_t>(&HookJobGet3D)) &&
                         WriteJobCall(dispatch, 7, reinterpret_cast<uintptr_t>(&HookJobDispatch));
    const bool verified = written && JobCallMatches(lookup, 5, reinterpret_cast<uintptr_t>(&HookJobLookup)) &&
                          JobCallMatches(get3D, 6, reinterpret_cast<uintptr_t>(&HookJobGet3D)) && JobCallMatches(dispatch, 7, reinterpret_cast<uintptr_t>(&HookJobDispatch));
    s_jobReady.store(verified, std::memory_order_release);
    return verified;
}
// the descent hooks the commit needs (normal entry EE24D0, world call EE2526, child calls EE258C/EE261C) when
bool InstallCommitDescentHooks()
{
    auto* entry = reinterpret_cast<uint8_t*>(s_imageBase + 0xEE24D0);
    auto* world = reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2526);
    auto* childA = reinterpret_cast<uint8_t*>(s_imageBase + 0xEE258C);
    auto* childB = reinterpret_cast<uint8_t*>(s_imageBase + 0xEE261C);
    const auto fits = [](const uint8_t* apSite, const auto aHook) noexcept
    {
        return CallReaches(apSite, reinterpret_cast<uintptr_t>(aHook));
    };
    if (!s_jobReady.load() || !s_commitPassReady.load() ||
        !PatchCheck::Is(entry, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEE24E8), {0xF6, 0x42, 0x04, 0x01, 0x41, 0x8B, 0xF0, 0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xF9}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2500), {0x48, 0x8B, 0x03, 0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0x38, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2518), {0x48, 0x8B, 0x07, 0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCF, 0x4C,
                                                                             0x89, 0x74, 0x24, 0x40, 0xFF, 0x90, 0x80, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2580), {0x48, 0x8B, 0x06, 0x45, 0x8B, 0xC4, 0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCE, 0xFF, 0x90, 0x60, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2610), {0x48, 0x8B, 0x03, 0x45, 0x8B, 0xC4, 0x48, 0x8B, 0xD5, 0x48, 0x8B, 0xCB, 0xFF, 0x90, 0x60, 0x01, 0x00, 0x00}) ||
        !PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEF4D10), {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0xB2, 0xD7, 0xFE, 0xFF,
                                                                             0x48, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, 0x25, 0x02, 0x00, 0x00}) ||
        TiltedPhoques::GetCall<DescentCall*>(reinterpret_cast<uint8_t*>(s_imageBase + 0xEF4D19)) != reinterpret_cast<DescentCall*>(entry) || !fits(world, &HookDescentWorld) ||
        !fits(childA, &HookDescentChildA) || !fits(childB, &HookDescentChildB))
    {
        spdlog::debug("SEBodyCommit: exact descent guards failed; descent hooks not installed");
        return false;
    }
    s_descentOriginal = reinterpret_cast<DescentCall*>(entry);
    TP_HOOK_IMMEDIATE(&s_descentOriginal, &HookDescentEntry);
    if (s_descentOriginal == reinterpret_cast<DescentCall*>(entry) || !EntryReachesHook(reinterpret_cast<uintptr_t>(entry), reinterpret_cast<uintptr_t>(&HookDescentEntry)))
    {
        spdlog::debug("SEBodyCommit: descent hook not in place; descent hooks off");
        return false;
    }
    const bool written = WriteJobCall(world, 6, reinterpret_cast<uintptr_t>(&HookDescentWorld)) && WriteJobCall(childA, 6, reinterpret_cast<uintptr_t>(&HookDescentChildA)) &&
                         WriteJobCall(childB, 6, reinterpret_cast<uintptr_t>(&HookDescentChildB));
    const bool verified = written && JobCallMatches(world, 6, reinterpret_cast<uintptr_t>(&HookDescentWorld)) &&
                          JobCallMatches(childA, 6, reinterpret_cast<uintptr_t>(&HookDescentChildA)) && JobCallMatches(childB, 6, reinterpret_cast<uintptr_t>(&HookDescentChildB));
    s_descentReady.store(verified, std::memory_order_release);
    return verified;
}

void InstallBodyCommit()
{
    InstallCommitJobHooks();
    if (s_jobReady.load())
        s_commitPassReady.store(true, std::memory_order_release);
    InstallCommitDescentHooks();
    // Exact SE1.7.104 bytes (dumpbin of the pinned image): the list-6 per-object worker builds NiUpdateData
    // {0.0, 2000h} and calls [rax+188h] with RCX=object, RDX=&data, then jumps away; EE2B70 calls
    // [rax+180h] with RCX=RDI=node only when flags+F4 bit2 is set, then reloads ecx. RAX is dead after both.
    const bool prerequisites = s_hookReady.load() && s_jobReady.load() && s_commitPassReady.load() && s_descentReady.load() && s_skinCallsReady && s_engineFrame;
    const bool guards =
        prerequisites && PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x66C180), {0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x38}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x66C1C0), {0x83, 0x79, 0x08, 0x01, 0x76, 0x21}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x66C1CF), {0xC7, 0x44, 0x24, 0x74, 0x00, 0x20, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48,
                                                                            0x8D, 0x54, 0x24, 0x70, 0xFF, 0x90, 0x88, 0x01, 0x00, 0x00, 0xEB, 0x31}) &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2B70),
            {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x4C, 0x8B, 0xFA}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEE2B83), {0x8B, 0x89, 0xF4, 0x00, 0x00, 0x00, 0x8B, 0xC1, 0xC1, 0xE8, 0x02, 0xA8, 0x01, 0x74, 0x12, 0x48, 0x8B,
                                                                            0x07, 0x48, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x01, 0x00, 0x00, 0x8B, 0x8F, 0xF4, 0x00, 0x00, 0x00});
    const bool verified = guards;
    s_bodyCommitReady = verified;
    if (!guards)
        spdlog::debug("SEBodyCommit: exact prerequisites or guards failed; the body commit is off and frame end poses every copy");
    // Rigid composition requires the native parent-copy and compose paths.
    const auto nativeCompose = reinterpret_cast<WorldCompose*>(s_imageBase + 0x3059A0);
    const auto nodeCall = TiltedPhoques::GetCall<WorldCompose*>(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC042));
    const auto ownCall = TiltedPhoques::GetCall<WorldCompose*>(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC214));
    const bool nodeNative = nodeCall == nativeCompose;
    const bool ownNative = ownCall == nativeCompose;
    const bool rigidGuards =
        verified && PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEBF40), {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEBFBF), {0x48, 0x8B, 0x4B, 0x40, 0x48, 0x85, 0xC9, 0x74, 0x0E, 0x48, 0x8B, 0x01,
                                                                            0xFF, 0x90, 0x30, 0x01, 0x00, 0x00, 0xE9, 0x4C, 0x01, 0x00, 0x00}) &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEEBFD6), {0x48, 0x8B, 0x4B, 0x30, 0x48, 0x89, 0x7C, 0x24, 0x70, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x9F, 0x00, 0x00, 0x00}) &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEEBFE8), {0x8B, 0x83, 0xF4, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC1, 0x7C, 0xC1, 0xE8, 0x09, 0xA8, 0x01, 0x74, 0x40}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC039), {0x4C, 0x8D, 0x43, 0x48, 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC047), {0x48, 0x8B, 0xD0}) &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0x3059A0), {0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00, 0x41, 0x0F, 0x29, 0x73, 0xE8, 0x33, 0xC0}) &&
        nodeNative;
    s_commitRigidReady = rigidGuards;
    if (verified && !rigidGuards)
        spdlog::debug("SEBodyCommit: exact UpdateWorldData/compose guards failed; rigid pass disabled, flat commit unaffected");
    // Verify the native compose calls and collision vtables before admitting collision-owning rigid nodes.
    bool collisionGuards =
        rigidGuards && VerifyFallbackCallerSites() &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC1A0), {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x30}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC1B1), {0x48, 0x85, 0xC9, 0x0F, 0x84, 0x9F, 0x00, 0x00, 0x00, 0x8B, 0x83, 0xF4, 0x00,
                                                                            0x00, 0x00, 0x48, 0x83, 0xC1, 0x7C, 0xC1, 0xE8, 0x09, 0xA8, 0x01, 0x74, 0x40}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC20B), {0x4C, 0x8D, 0x43, 0x48, 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0xEEC219), {0x48, 0x8B, 0xD0, 0x0F, 0x10, 0x00, 0x0F, 0x11, 0x43, 0x7C}) && ownNative &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0x105D300),
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x79, 0x20}) &&
        PatchCheck::Is(
            reinterpret_cast<uint8_t*>(s_imageBase + 0x106AFD0), {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB7, 0x49, 0x18, 0x48, 0x8B, 0xD9}) &&
        PatchCheck::Is(reinterpret_cast<uint8_t*>(s_imageBase + 0x106B160), {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49, 0x89, 0x73, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x20});
    for (const auto& k : kCommitCollisionClasses)
    {
        const auto* table = reinterpret_cast<const void*>(s_imageBase + k.Vtable);
        uintptr_t update{}, slot158{}, slot160{}, slot168{}, slot178{};
        collisionGuards = collisionGuards && ReadAt(table, 0x130, update) && ReadAt(table, 0x158, slot158) && ReadAt(table, 0x160, slot160) && ReadAt(table, 0x168, slot168) &&
                          ReadAt(table, 0x178, slot178) && update == s_imageBase + k.Update && slot158 == s_imageBase + k.Slot158 && slot160 == s_imageBase + k.Slot160 &&
                          slot168 == s_imageBase + k.Slot168 && slot178 == s_imageBase + k.Slot178;
    }
    for (const auto& call : kCommitCollisionCalls)
    {
        auto* site = reinterpret_cast<uint8_t*>(s_imageBase + call.Site);
        collisionGuards = collisionGuards && PatchCheck::Is(site, {call.Opcode}) && TiltedPhoques::GetCall<uint8_t*>(site) == reinterpret_cast<uint8_t*>(s_imageBase + call.Target);
    }
    s_commitRigidCollisionReady = collisionGuards;
    if (rigidGuards && !collisionGuards)
        spdlog::debug("SEBodyCommit: exact collision world-update guards failed; collision-owning rigid items refused, rest of the rigid pass unaffected");
}

void Hook(void* apTree)
{
    if (s_jobReady.load(std::memory_order_relaxed))
    {
        WorldTreeScope scope(apTree);
        s_original(apTree);
        return;
    }
    s_original(apTree);
}

static TiltedPhoques::Initializer s_initialize(
    []
    {
        // A held object may be drawn from the hands this commit poses.
        PosedHands::SetQuery(&HandsPosedFromVR);
        auto& db = VersionDb::Get();
        int major{}, minor{}, revision{}, build{};
        db.GetLoadedVersion(major, minor, revision, build);
        unsigned long long offset{};
        auto* target = static_cast<uint8_t*>(db.FindAddressById(kHelperId));
        if (major != 1 || minor != 7 || revision != 104 || build != 0 || !db.FindOffsetById(kHelperId, offset) || offset != kHelperRva ||
            !PatchCheck::Is(target, {0x40, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x8B, 0x81, 0x28, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF1}))
        {
            spdlog::debug("SEBodyCommit: exact SE1.7.104 helper guard failed; the body commit is off");
            return;
        }
        s_imageBase = reinterpret_cast<uintptr_t>(target) - kHelperRva;
        unsigned long long frameOffset{};
        if (db.FindOffsetById(411489, frameOffset) && frameOffset == 0x33363F4)
            s_engineFrame = reinterpret_cast<volatile uint32_t*>(db.FindAddressById(411489));
        s_original = reinterpret_cast<Helper*>(target);
        TP_HOOK_IMMEDIATE(&s_original, &Hook);
        if (s_original == reinterpret_cast<Helper*>(target) || !(target[0] == 0xE9 || (target[0] == 0xFF && target[1] == 0x25)))
        {
            spdlog::debug("SEBodyCommit: helper hook not in place; the body commit is off");
            return;
        }
        s_hookReady.store(true);
        unsigned long long builderOffset{}, callerOffset{};
        auto* composeCall = reinterpret_cast<uint8_t*>(s_imageBase + 0x1015B24);
        auto* builderCall = reinterpret_cast<uint8_t*>(s_imageBase + 0x1578CFB);
        // The native skin compose and build calls the commit's writes rely on, unpatched.
        s_skinCallsReady = s_engineFrame && db.FindOffsetById(77461, builderOffset) && builderOffset == 0x1015800 && db.FindOffsetById(108322, callerOffset) &&
                           callerOffset == 0x1578C80 && PatchCheck::Is(composeCall, {0xE8, 0x77, 0xFE, 0x2E, 0xFF, 0x8D, 0x04, 0x7F}) &&
                           PatchCheck::Is(builderCall, {0xE8, 0x00, 0xCB, 0xA9, 0xFF, 0x48, 0x8B, 0x7D, 0x48, 0x4C, 0x8B, 0x65, 0x50});
        if (!s_skinCallsReady)
            spdlog::debug("SEBodyCommit: exact skin call or frame guards failed; the body commit is off");
        InstallBodyCommit();
    });
} // namespace

namespace
{
// The slot aActor's copy holds or may take (registry mutex held), every slot's binding state brought up to date first.
RemoteBodySlots::Choice ChooseSlotLocked(const uint32_t aActor, const BodyPoseCommit::RemotePlayerKey& acPlayer, const double aNow) noexcept
{
    for (auto& owner : s_owners)
        RemoteBodySlots::Observe(owner, owner.Actor && s_active.find(owner.Actor) != s_active.end(), aNow);
    return RemoteBodySlots::Choose(s_owners, aActor, acPlayer, aNow);
}
// Slot aIndex passes to aActor's copy, or is emptied (registry mutex held). The previous copy's presentation ends at once:
// frame end poses it, and the slot's commit state starts over at its next pass (SyncCommitSelected), rebuilding the
// topology from the new copy's fresh 3D.
void AssignSlotLocked(const size_t aIndex, const uint32_t aActor, const BodyPoseCommit::RemotePlayerKey& acPlayer) noexcept
{
    auto& slot = s_slots[aIndex];
    auto& owner = s_owners[aIndex];
    if (owner.Actor && slot.DwellActor == owner.Actor)
        WithdrawDwellLocked(slot);
    InvalidateGraceLocked(slot);
    DisarmJobIdentity(slot);
    slot.Actor.store(aActor, std::memory_order_release);
    slot.PosedHands.store(0, std::memory_order_release);
    slot.CommittedBody.store(0, std::memory_order_release);
    slot.Epoch.fetch_add(1, std::memory_order_acq_rel);
    owner = {};
    owner.Actor = aActor;
    owner.Player = acPlayer;
}
// aActor's copy left (0: every copy): its slot is free for another remote player's copy.
void ReleaseSlotsLocked(const uint32_t aActor) noexcept
{
    for (size_t i = 0; i < s_owners.size(); ++i)
        if (s_owners[i].Actor && (!aActor || s_owners[i].Actor == aActor))
            AssignSlotLocked(i, 0, {});
}
} // namespace

namespace
{
// SampleWithProvenance returns an empty pose with the CURRENT generation once every endpoint expires.
// It can first collapse the expired pair to its still-fresh After endpoint. Both unrefreshed shapes
// retain only the prior committed values; neither replacement sample is admitted as new input.
bool RetainGraceForUnrefreshedSource(Actor* apActor, const RemotePose::Sample& acMissing, double aNowNetwork, const RemotePose::Identity& acIdentity) noexcept
{
    if (!std::isfinite(aNowNetwork) || (acMissing.Pose.HasData && !GraceSourcePoseValid(acMissing.Pose)))
        return false;
    const auto root = reinterpret_cast<uintptr_t>(apActor->GetNiNode()); // Publish contract: current live owner-thread actor
    std::lock_guard<std::mutex> guard(s_mutex);
    const auto found = s_active.find(apActor->formID);
    const size_t cSlot = SlotOf(apActor->formID);
    if (found == s_active.end() || cSlot == kNoSlot || !s_slots[cSlot].Grace.Source)
        return false;
    auto& grace = s_slots[cSlot].Grace;
    const auto& source = *grace.Source;
    if (!(acIdentity == source.TrustedIdentity) || !BodyPoseCommit::UnrefreshedGraceSample(source.Sample, acMissing) || !root || root != source.RootHint ||
        aNowNetwork < grace.LastNetworkMs || aNowNetwork < source.Sample.Before.ReceivedAtMs || aNowNetwork < source.Sample.After.ReceivedAtMs ||
        !RetainExpiredBindingLocked(*found->second))
        return false;
    grace.LastNetworkMs = aNowNetwork; // monotonicity guard only: never used to calculate a deadline
    return true;                       // no job identity publication, no source ledger mutation, no receive-time renewal
}
} // namespace
void Publish(Actor* apActor, const RemotePose::Sample& acSample, double aNowNetworkMs, const RemotePose::Identity& acIdentity) noexcept
{
    if (!s_hookReady.load() || !apActor || !apActor->formID)
        return;
    // Tick() hands publication to the thread of each client update (PosePublishThread.h); with none yet, this one takes it.
    DWORD expected{};
    s_ownerThread.compare_exchange_strong(expected, PosePublishThread::OwnerForPublish(expected, GetCurrentThreadId()));
    if (!PosePublishThread::MayPublish(s_ownerThread.load(), GetCurrentThreadId()))
        return;
    try
    {
        auto* extension = apActor->GetExtension();
        if (!extension || !extension->IsRemotePlayer() || apActor->actorState.IsDeadOrDying() || apActor->actorState.IsBleedingOut())
        {
            RemoveBinding(apActor->formID);
            return;
        }
        if (RetainGraceForUnrefreshedSource(apActor, acSample, aNowNetworkMs, acIdentity))
            return;
        if (!acSample.Pose.HasData || acSample.Pose.NoBones || !RemotePose::IsFresh(acSample.Before, acSample.Generation, aNowNetworkMs, RemotePose::Policy::LiveVRPlayer) ||
            !RemotePose::IsFresh(acSample.After, acSample.Generation, aNowNetworkMs, RemotePose::Policy::LiveVRPlayer))
        {
            RemoveBinding(apActor->formID); // keep replay/deadline ledger until an explicit lifecycle reset
            return;
        }
        // A copy with no slot to hold or take is posed at frame end and needs no binding.
        const auto remote = BodyPoseCommit::RemotePlayerOf(acIdentity.Session, acIdentity.Entity);
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            if (ChooseSlotLocked(apActor->formID, remote, SteadyMs()).Kind == RemoteBodySlots::Claim::None)
                return;
        }
        auto* root = apActor->GetNiNode();
        std::shared_ptr<const Binding> previous;
        uint64_t registryEpoch{};
        double deadline{};
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            registryEpoch = s_registryEpoch;
            const auto it = s_active.find(apActor->formID);
            if (it != s_active.end())
                previous = it->second;
            if ((!previous && s_active.size() >= kMaxActors) || !SourceDeadlineLocked(apActor->formID, acSample, aNowNetworkMs, SteadyMs(), deadline))
            {
                s_active.erase(apActor->formID);
                Disarm(apActor->formID);
                s_activeCount.store(s_active.size());
                return;
            }
        }
        if (!root)
        {
            RemoveBinding(apActor->formID);
            return;
        }
        void* tree{};
        if (previous && previous->RootHint == reinterpret_cast<uintptr_t>(root) && IsTree(reinterpret_cast<void*>(previous->TreeHint)) &&
            UnderRoot(reinterpret_cast<void*>(previous->TreeHint), root))
            tree = reinterpret_cast<void*>(previous->TreeHint);
        else
        {
            // Borrowed identity hints only. RPM traversal cannot establish exclusion against engine teardown.
            Walk(
                root,
                [&](void* apObject) noexcept
                {
                    if (IsTree(apObject))
                        tree = apObject;
                    return !tree;
                });
            if (!tree)
            {
                RemoveBinding(apActor->formID);
                return;
            }
        }
        auto next = std::make_shared<Binding>();
        next->FormId = apActor->formID;
        next->Sample = acSample;
        next->TrustedIdentity = acIdentity;
        next->PublishedNetworkMs = aNowNetworkMs;
        next->ExpiresAtSteadyMs = deadline;
        next->RootHint = reinterpret_cast<uintptr_t>(root);
        next->TreeHint = reinterpret_cast<uintptr_t>(tree);
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            if (registryEpoch != s_registryEpoch || SteadyMs() > deadline)
                return;
            const auto cNow = SteadyMs();
            const auto cChoice = ChooseSlotLocked(next->FormId, remote, cNow); // re-check under the same lock as the insert
            if (cChoice.Kind == RemoteBodySlots::Claim::None)
                return;
            auto& slot = s_slots[cChoice.Index];
            if (slot.Grace.Source && slot.Grace.Source->FormId == next->FormId &&
                (!(GraceKeyOf(*slot.Grace.Source) == GraceKeyOf(*next)) || slot.Grace.Source->Sample.Before.SourceTick != next->Sample.Before.SourceTick ||
                 slot.Grace.Source->Sample.After.SourceTick != next->Sample.After.SourceTick || !GraceSourcePoseValid(next->Sample.Pose)))
                InvalidateGraceLocked(next->FormId);
            // Insertion may allocate; only claim the slot once a binding exists.
            s_active[apActor->formID] = next;
            s_activeCount.store(s_active.size());
            if (cChoice.Kind != RemoteBodySlots::Claim::Held)
                AssignSlotLocked(cChoice.Index, next->FormId, remote);
            RemoteBodySlots::Observe(s_owners[cChoice.Index], true, cNow); // bound again: a later unbind starts a fresh hold
            PublishJobIdentity(slot, next->FormId, next->Sample.Generation, next->Sample.After.SourceTick, next->TreeHint, next->RootHint, next->ExpiresAtSteadyMs);
        }
    }
    catch (...)
    {
    } // failed publication never changes native actor behavior
}

void Forget(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return;
    try
    {
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            ++s_registryEpoch;
            s_active.erase(aFormId);
            s_sources.erase(aFormId); // explicit lifecycle boundary; caller must also invalidate pose generation
            Disarm(aFormId);
            ReleaseSlotsLocked(aFormId);
            s_activeCount.store(s_active.size());
        }
    }
    catch (...)
    {
    }
}

void ForgetForGeneration(uint32_t aFormId, uint64_t aGeneration) noexcept
{
    if (!aFormId || !aGeneration)
        return;
    try
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        const auto source = s_sources.find(aFormId);
        if (source == s_sources.end() || source->second.Generation != aGeneration)
            return;
        ++s_registryEpoch;
        s_sources.erase(source);
        const auto active = s_active.find(aFormId);
        if (active != s_active.end() && active->second->Sample.Generation == aGeneration)
            s_active.erase(active);
        Disarm(aFormId);
        s_activeCount.store(s_active.size());
    }
    catch (...)
    {
    }
}

void Clear() noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            ++s_registryEpoch;
            s_active.clear();
            s_sources.clear();
            Disarm(0);
            ReleaseSlotsLocked(0);
            s_activeCount.store(0);
        }
    }
    catch (...)
    {
    }
}

void Tick() noexcept
{
    // The client update calling this owns publication until the next one, on whichever of the game's job threads it runs.
    s_ownerThread.store(GetCurrentThreadId());
    if (!PosePublishThread::MayPublish(s_ownerThread.load(), GetCurrentThreadId()))
        return;
    ++s_gameFrame;
    try
    {
        const auto now = SteadyMs();
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            for (auto it = s_active.begin(); it != s_active.end();)
                if (now > it->second->ExpiresAtSteadyMs && !RetainExpiredBindingLocked(*it->second))
                {
                    Disarm(it->first);
                    it = s_active.erase(it);
                }
                else
                    ++it;
            s_activeCount.store(s_active.size());
        }
    }
    catch (...)
    {
    }
}

bool Presents(const uint32_t aFormId) noexcept
{
    const size_t cSlot = SlotOf(aFormId);
    return cSlot != kNoSlot && RemoteBodyPoser::NativeCommitPoses(s_slots[cSlot].CommittedBody.load(std::memory_order_acquire), aFormId, EngineFrame());
}

void BeforePresent() noexcept
{
    ++s_presentFrame;
    try
    {
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            for (auto& slot : s_slots)
                slot.Dwell.Presented(SteadyMs(), s_presentFrame.load(std::memory_order_acquire));
        }
    }
    catch (...)
    {
    }
}
} // namespace SEBodyPresentation
#endif
