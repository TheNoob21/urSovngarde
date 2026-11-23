#include <TiltedOnlinePCH.h>

#include <limits>
#include <mutex>
#include <unordered_set>

#include <Systems/InterpolationSystem.h>
#include <Systems/SpawnReveal.h>
#include <Systems/RemotePoseInterpolation.h>
#include <Systems/RemoteSmoothing.h>
#include <Systems/SeatPin.h>
#include <Systems/StrideMatch.h>
#include <Systems/PlayerKnockMirror.h>
#include <Systems/JumpSync.h>
#include <MountSync.h>
#include <SneakState.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <PlayerCharacter.h>
#include <World.h>

#include <Forms/ActorValueInfo.h>
#include <Games/Skyrim/CopyBodyRules.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Forms/BGSAction.h>
#include <Games/Skyrim/SpellAimRules.h>
#include <BSAnimationGraphManager.h>
#include <Games/Skyrim/VR/PosedCopyPlacement.h>
#include <Services/DiscoveryService.h>
#include <Structs/AnimationGraphDescriptor.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Services/CopyWorkGate.h>
#ifndef SKYRIMVR
#include <Games/Skyrim/SEBodyPresentation.h>
#endif

namespace
{
uint64_t s_poseSession = 1;

// A VR player's body is shown at least this far behind, so a pose is there on both sides of the shown tick even when
// the headset game draws, and sends, only 20-something times a second.
constexpr double cVRBodyMinDelay = 100.0;

// How each float the copy's graph is sent is shown between two points (RemoteSmoothing::BlendOf), by the variable's name in
// the copy's own graph. Found once per graph descriptor; null while the graph or its descriptor is not there yet.
const Vector<RemoteSmoothing::FloatBlend>* FloatBlends(Actor* apActor) noexcept
{
    static TiltedPhoques::Map<size_t, Vector<RemoteSmoothing::FloatBlend>> s_kinds;
    const auto* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->GraphDescriptorHash)
        return nullptr;
    const size_t cHash = pExtension->GraphDescriptorHash;
    if (const auto it = s_kinds.find(cHash); it != s_kinds.end())
        return &it->second;

    const AnimationGraphDescriptor* pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(cHash);
    BSAnimationGraphManager* pManager = nullptr;
    if (!pDescriptor || !apActor->animationGraphHolder.GetBSAnimationGraph(&pManager))
        return nullptr;
    SortedMap<uint32_t, String> names;
    {
        BSScopedLock<BSRecursiveLock> _{pManager->lock};
        names = pManager->DumpAnimationVariables(false);
    }
    pManager->Release();
    if (names.empty())
        return nullptr;

    Vector<RemoteSmoothing::FloatBlend> kinds;
    kinds.reserve(pDescriptor->FloatLookupTable.size());
    for (const auto cIndex : pDescriptor->FloatLookupTable)
    {
        const auto cName = names.find(cIndex);
        kinds.push_back(cName != names.end() ? RemoteSmoothing::BlendOf(std::string_view(cName->second)) : RemoteSmoothing::FloatBlend::Newer);
    }
    return &s_kinds.emplace(cHash, std::move(kinds)).first->second;
}

// Where "Speed", "SpeedSampled" and "Direction" sit among the floats the copy's graph is sent, "IsSneaking" among its switches
// and "iIsInSneak" among its integers, -1 for one it has not, and whether the graph is the humanoid one (0_master), told by the
// variables the behaviour replacers tell it by.
struct LegSlots
{
    int Speed{-1};
    int Sampled{-1};
    int Direction{-1};
    int Sneaking{-1};
    int InSneak{-1};
    int InJump{-1};
    bool Humanoid{false};
};

// Found once per graph descriptor, by name in the copy's own graph; null while the graph or its descriptor is not there yet.
const LegSlots* LegSlotsOf(Actor* apActor) noexcept
{
    static TiltedPhoques::Map<size_t, LegSlots> s_slots;
    const auto* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->GraphDescriptorHash)
        return nullptr;
    const size_t cHash = pExtension->GraphDescriptorHash;
    if (const auto it = s_slots.find(cHash); it != s_slots.end())
        return &it->second;

    const AnimationGraphDescriptor* pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(cHash);
    BSAnimationGraphManager* pManager = nullptr;
    if (!pDescriptor || !apActor->animationGraphHolder.GetBSAnimationGraph(&pManager))
        return nullptr;
    SortedMap<uint32_t, String> names;
    {
        BSScopedLock<BSRecursiveLock> _{pManager->lock};
        names = pManager->DumpAnimationVariables(false);
    }
    pManager->Release();
    if (names.empty())
        return nullptr;

    LegSlots slots;
    bool footIk = false;
    bool femaleOffset = false;
    for (const auto& [cIndex, cName] : names)
    {
        footIk = footIk || cName == "bHumanoidFootIKEnable";
        femaleOffset = femaleOffset || cName == "FemaleOffset";
    }
    slots.Humanoid = footIk && femaleOffset;
    for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
    {
        const auto cName = names.find(pDescriptor->FloatLookupTable[i]);
        if (cName == names.end())
            continue;
        if (cName->second == "Speed")
            slots.Speed = static_cast<int>(i);
        else if (cName->second == "SpeedSampled")
            slots.Sampled = static_cast<int>(i);
        else if (cName->second == "Direction")
            slots.Direction = static_cast<int>(i);
    }
    for (size_t i = 0; i < pDescriptor->BooleanLookUpTable.size(); ++i)
        if (const auto cName = names.find(pDescriptor->BooleanLookUpTable[i]); cName != names.end())
        {
            if (cName->second == "IsSneaking")
                slots.Sneaking = static_cast<int>(i);
            else if (cName->second == "bInJumpState")
                slots.InJump = static_cast<int>(i);
        }
    for (size_t i = 0; i < pDescriptor->IntegerLookupTable.size(); ++i)
        if (const auto cName = names.find(pDescriptor->IntegerLookupTable[i]); cName != names.end() && cName->second == "iIsInSneak")
            slots.InSneak = static_cast<int>(i);
    return &s_slots.emplace(cHash, slots).first->second;
}

// aTick: the owner's tick the actor is placed at, so that its pose and its place are from the same instant.
void PublishPose(Actor* apActor, InterpolationComponent& aComponent, const double aNow, const uint64_t aTick) noexcept
{
    const double cMaxExtrapolation = aComponent.PosePolicy == RemotePose::Policy::LiveVRPlayer ? RemotePoseInterpolation::kMaxPoseExtrapolationMs : 0.0;
    const uint64_t cTick = aTick;
    RemotePose::Sample sample = RemotePoseInterpolation::SampleWithProvenance(
        aComponent.TimePoints, cTick, aComponent.PoseState.Generation(), aNow, aComponent.PosePolicy, RemotePose::kLiveTimeoutMs, cMaxExtrapolation);

    // Carried on past the newest pose, the body would go straight back to the late pose when it comes: it is blended back
    // from the pose last shown instead.
    if (cMaxExtrapolation > 0.0)
        RemotePoseInterpolation::Present(aComponent.PoseBlend, aComponent.PresentationPose, sample, cTick, aNow);
    else
        aComponent.PoseBlend.Carried = false;

    aComponent.PresentationPose = sample;
    aComponent.InterpolatedVRPose = aComponent.PresentationPose.Pose;
#ifdef SKYRIMVR
    VRBodySync::SetRemotePoseSample(apActor, aComponent.PresentationPose, aNow, aComponent.PosePolicy);
#else
    SEBodyPresentation::Publish(apActor, aComponent.PresentationPose, aNow, aComponent.PoseState.GetIdentity());
    if (apActor && apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer())
    {
        // Every remote VR player is posed at frame end (VRBodySync::OnFrameEnd), except the copies SEBodyPresentation commits.
        VRBodySync::SetRemotePoseSample(apActor, aComponent.PresentationPose, aNow, aComponent.PosePolicy);
        VRBodySync::SetRemoteSpellAim(apActor->formID, aComponent.InterpolatedVRPose);
    }
#endif
}

// Past the newest point the actor keeps moving for at most this long, then holds still until data arrives.
constexpr double cMaxExtrapolation = 150.0;
// Larger corrections snap instead of sliding the actor into place.
constexpr float cMaxPositionCorrection = 256.f;
// Time constant of a correction fading out: about 95% of it is gone after three times this.
constexpr double cPositionCorrectionTime = 50.0;
// Two points further apart than this are a teleport, not movement to cross.
constexpr float cTeleportDistance = 1024.f;

// A copy that got up away from where its owner has it closes the gap at most this many units a second faster than its owner moves.
constexpr float cGetUpCatchUpSpeed = 300.f;
// A gap wider than this is closed at once: walking it would take too long.
constexpr float cGetUpCatchUpDistance = 512.f;

// An NPC copy that just got up, while it closes the gap to where its owner has it and the speed of its steps is measured.
struct Rising
{
    std::chrono::steady_clock::time_point Since;
    float Fastest;
    // Where it stands against where its owner has it; shrinks to nothing.
    glm::vec3 Offset;
};
TiltedPhoques::Map<uint32_t, Rising> s_rising;

// NPC copies lying in their own ragdoll: one line when one goes down, one when it is up again, and one if it still lies after 15 s.
struct Down
{
    std::chrono::steady_clock::time_point Since;
    std::chrono::steady_clock::time_point QueuedSince;
    uint32_t GetUpsSent;
    bool SaidLong;
    // Put on its feet where its owner stands, once its own get-ups were all refused.
    bool Stood;
    // The owner's last action when it went down here: a later one tells whether its owner's NPC is still down.
    uint64_t ActionTickAtDown;
    // Which fall this is (s_falls): its ragdoll is put where its owner's NPC fell at most once per fall (VRBodySync::PlaceCorpse).
    uint64_t Fall;
};
TiltedPhoques::Map<uint32_t, Down> s_down;
uint64_t s_falls = 0;

// Each player copy's mirrored fall, by form id.
TiltedPhoques::Map<uint32_t, PlayerKnockMirror::Mirror> s_playerKnocks;
// Flat knocks a player copy from its frame (KnockExplosion). VR knocks it only with the owner's own "Ragdoll", once admitted.
#ifdef SKYRIMVR
constexpr bool kFrameKnocksPlayerCopy = false;
#else
constexpr bool kFrameKnocksPlayerCopy = true;
#endif

#ifndef SKYRIMVR
struct ReplayedFall
{
    CopyBodyRules::CopyAuthority Authority;
    Actor* ActorAtArm;
    void* RootAtArm;
    bool RevivedAtArm;
    uint64_t NativeLifeAtArm;
    uint64_t ActionTickAtArm;
    bool EssentialDown;
    bool KnockRequested = false;
    std::chrono::steady_clock::time_point KnockRequestedAt{};
    std::chrono::steady_clock::time_point ArmedAt;
    CopyBodyRules::ObservedFall Observation{};
    CopyBodyRules::ObservedFall KnockObservation{};
    CopyBodyRules::OwnerRecoveryEvidence Recovery{};
};
TiltedPhoques::Map<uint32_t, ReplayedFall> s_replayedFalls;
#endif

// Who a copy is shown for, as its pose state has it, for the moves PlaceCorpse makes or queues for it.
CopyBodyRules::CopyAuthority AuthorityOf(const InterpolationComponent& acComponent) noexcept
{
    const RemotePose::Identity& identity = acComponent.PoseState.GetIdentity();
    return CopyBodyRules::CopyAuthority{identity.Session, identity.Owner, identity.Entity, acComponent.PoseState.Generation()};
}

#ifndef SKYRIMVR
// Called only by main-thread accepted network/action handlers. The native resurrection hook touches no map.
ReplayedFall* BoundObservedFall(World& aWorld, Actor* apActor, const bool aRefreshRevivedMetadata = false) noexcept
{
    if (!apActor || apActor->actorState.IsDeadOrDying())
        return nullptr;
    const auto* extension = apActor->GetExtension();
    if (!extension || !extension->IsRemote() || extension->IsRemotePlayer())
        return nullptr;
    const auto found = s_replayedFalls.find(apActor->formID);
    if (found == s_replayedFalls.end())
        return nullptr;
    ReplayedFall& fall = found.value();
    if (fall.ActorAtArm != apActor || fall.RootAtArm != apActor->GetNiNode() || fall.NativeLifeAtArm != extension->NativeLifeGeneration.load(std::memory_order_relaxed))
        return nullptr;
    const auto view = aWorld.view<RemoteComponent, InterpolationComponent>();
    for (const auto entity : view)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        if (remote.CachedRefId != apActor->formID || remote.DeadAtOwner || fall.Authority != AuthorityOf(view.get<InterpolationComponent>(entity)))
            continue;
        // An already-alive owner notification changes this metadata, but performs no native revival/new life.
        if (aRefreshRevivedMetadata)
            fall.RevivedAtArm = extension->RevivedSinceDeath;
        return fall.RevivedAtArm == extension->RevivedSinceDeath ? &fall : nullptr;
    }
    return nullptr;
}
#endif

// Whether the owner's bones of a body arrive now: the pose shown was received within the live timeout.
// A body's sparse pose is held for as long as the buffer keeps its points, so having one says nothing of whether more come.
bool OwnerBonesArrive(const InterpolationComponent& acComponent, const double aNow) noexcept
{
    const RemotePose::Sample& sample = acComponent.PresentationPose;
    return sample.Pose.HasData && SpellAimRules::NeedsBodyPresentation(sample.Pose.NoBones, sample.Pose.HasRootPosition) && sample.After.Valid &&
           std::isfinite(sample.After.ReceivedAtMs) && aNow >= sample.After.ReceivedAtMs && aNow - sample.After.ReceivedAtMs <= RemotePose::kLiveTimeoutMs;
}

// Whether the pose shown lies between two of the owner's poses: held past the newest, the owner has stopped moving it, or its next pose is late.
bool OwnerBonesMove(const InterpolationComponent& acComponent) noexcept
{
    const RemotePose::Sample& sample = acComponent.PresentationPose;
    return sample.Before.Valid && sample.After.Valid && sample.Before.SourceTick < sample.After.SourceTick;
}

// NPC copies in the game's own bleedout here: since when their owner's word has had them up, and when they were last taken out.
struct BleedingOutCopy
{
    std::chrono::steady_clock::time_point OwnerUpSince{};
    std::chrono::steady_clock::time_point TakenOutAt{};
};
TiltedPhoques::Map<uint32_t, BleedingOutCopy> s_bleedingOut;
// Copies kneeling in their graph while their life says alive (ActorExtension::KneelsInGraph): since when, its standing readings,
// the life (NativeLifeGeneration) it kneels in, where it knelt, and for a player's copy since when its owner's word has them up.
struct GraphKneel
{
    std::chrono::steady_clock::time_point SeenAt{};
    uint32_t HighSamples{};
    uint64_t Life{};
    glm::vec3 At{};
    std::chrono::steady_clock::time_point OwnerUpSince{};
};
TiltedPhoques::Map<uint32_t, GraphKneel> s_graphKneels;
// The tick of each player copy's owner's latest "bleedOutStop", by form id: their word that they are up (PlayerUpAfterKneel).
TiltedPhoques::Map<uint32_t, uint64_t> s_playerOwnerStops;
// Each player copy's jump against its owner's (JumpSync::Landing): noted where its actions play, read where it is placed.
std::mutex s_landingsLock;
TiltedPhoques::Map<uint32_t, JumpSync::Landing> s_landings;
// NPC copies put on their feet where their owner stands, and when: once in 30 s each, as their next fall starts a new entry above.
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_stoodUp;

// Copies dead here while their owner's game has not said they died, so that it is logged once per copy.
std::unordered_set<uint32_t> s_saidDeadAlone;

// A copy whose owner went quiet (a crash, a frozen game, a lost connection) would play the newest report's walk on the spot.
// After this long with no report, a copy that walks is stopped instead.
constexpr double cQuietStopAfter = 1500.0;
// Copies stopped that way, by form id: when, and when reports came back after (zero while none have).
// Kept here, not in the component: it is made again when the actor changes hands, and the stop still has to be undone after.
struct QuietStop
{
    std::chrono::steady_clock::time_point Since;
    std::chrono::steady_clock::time_point Back;
};
TiltedPhoques::Map<uint32_t, QuietStop> s_quietStopped;

// Not sitting, riding, sleeping or flying: the sit and sleep state (bits 14-17) and the fly state (bits 18-20) are clear.
// Only then do the graph's own move events stop and start the legs.
bool IsOnFoot(const Actor* apActor) noexcept
{
#ifdef SKYRIMVR
    // A dragon's copy keeps its own flight state, which only its game's flight handlers set here (FlyState.h); in the air it may still read as on its feet.
    if (apActor->IsDragon())
        return false;
#endif
    const uint32_t cFlags = apActor->actorState.flags1;
    return ((cFlags >> 14) & 0xF) == 0 && ((cFlags >> 18) & 0x7) == 0;
}

// How fast the copy's legs go: the graph's own Speed, or the actor's when that is higher.
// Some animation frameworks (MGO's) drive the legs from the graph while the actor's speed reads zero, and the walk went on.
float LegSpeed(Actor* apActor) noexcept
{
    static BSFixedString s_speed("Speed");
    float graphSpeed = 0.f;
    apActor->animationGraphHolder.GetVariableFloat(&s_speed, &graphSpeed);
    return std::max(graphSpeed, apActor->GetSpeed());
}

// Copies that went quiet without being stopped, so that why is logged once per quiet spell.
TiltedPhoques::Set<uint32_t> s_quietNotStopped;
// VR players' copies shown in their sneak stance for a real crouch (MatchCrouch), so a replayed action keeps it (HoldStride).
TiltedPhoques::Set<uint32_t> s_crouching;

#ifdef SKYRIMVR
// TEMPORARY diagnostic for a player copy seen flipping between two poses several times a second (with PoseFlipDiag in VRBodySync); remove once understood.
// It counts the poses that jumped away and came straight back, and shows every pose as it came.
// Whether two poses are further apart than a body moves between two updates: the hips' horizontal place, or a bone other than the hands.
// The hands are left out because a wrist can flick that fast.
bool PosesFarApart(const VRPose& acA, const VRPose& acB, const float aHips, const float aDegrees) noexcept
{
    if (acA.HasHips && acB.HasHips && glm::distance(glm::vec2{acA.HipOffset[0], acA.HipOffset[1]}, glm::vec2{acB.HipOffset[0], acB.HipOffset[1]}) > aHips)
        return true;
    // Two rotations are more than aDegrees apart when the absolute dot product of their quaternions is below the cosine of half of it.
    const float cCosHalf = std::cos(glm::radians(aDegrees) / 2.f);
    for (size_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        if (i == VRPose::kLeftHand || i == VRPose::kRightHand)
            continue;
        if (std::fabs(glm::dot(static_cast<glm::quat>(acA.Bones[i]), static_cast<glm::quat>(acB.Bones[i]))) < cCosHalf)
            return true;
    }
    return false;
}

// A pose that jumped away from the one before and is back beside it in the one after: a body read in another state, not a movement.
// On the rig the sending game's skeleton was sometimes in another state for one update, and the copy jumped to that pose and straight back.
bool IsPoseSpike(const VRPose& acBefore, const VRPose& acPose, const VRPose& acAfter) noexcept
{
    if (acBefore.NoBones || acPose.NoBones || acAfter.NoBones)
        return false;
    return PosesFarApart(acBefore, acPose, 15.f, 60.f) && PosesFarApart(acPose, acAfter, 15.f, 60.f) && !PosesFarApart(acBefore, acAfter, 7.5f, 30.f);
}

// Spikes in each remote player's poses: counted once each, logged every 30 s while there are any.
struct PoseSpikes
{
    uint64_t LastTick = 0;
    uint32_t Count = 0;
    std::chrono::steady_clock::time_point NextLog{};
};
TiltedPhoques::Map<uint32_t, PoseSpikes> s_poseSpikes;
#endif

// A player copy still in its jump while its owner reads as on the ground (bInJumpState of acOwner, the owner's newest point)
// is sent the fall it missed and the landing (JumpSync::Landing). aOffFeet: down, seated, riding, swimming or kneeling here, where its
// graph plays that and the jump is over.
void LandCopy(Actor* apActor, const AnimationVariables& acOwner, const bool aOffFeet) noexcept
{
    const LegSlots* pSlots = LegSlotsOf(apActor);
    if (!pSlots || pSlots->InJump < 0 || static_cast<size_t>(pSlots->InJump) >= acOwner.Booleans.size())
        return;
    const auto cNow = std::chrono::steady_clock::now();
    JumpSync::Synth synth = JumpSync::Synth::None;
    {
        std::scoped_lock lock(s_landingsLock);
        const auto it = s_landings.find(apActor->formID);
        if (it == s_landings.end())
            return;
        if (aOffFeet)
        {
            s_landings.erase(it);
            return;
        }
        synth = it.value().Update(acOwner.Booleans[pSlots->InJump], cNow);
    }
    if (synth == JumpSync::Synth::None)
        return;
    static BSFixedString s_fall("JumpFall");
    static BSFixedString s_land("JumpLand");
    const bool cTaken = apActor->SendAnimationEvent(synth == JumpSync::Synth::Fall ? &s_fall : &s_land);
    spdlog::debug(
        "Jump: player copy {:X} still in its jump while its owner is on the ground; sent '{}' ({})", apActor->formID, synth == JumpSync::Synth::Fall ? "JumpFall" : "JumpLand",
        cTaken ? "taken" : "refused");
}

// The graph's own stop, then no speed for the legs, as when the owner's character stands still.
void StopQuietCopy(Actor* apActor, const double aQuietFor) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    if (const auto stopped = s_quietStopped.find(apActor->formID); stopped != s_quietStopped.end())
        stopped.value().Back = {};
    else
    {
        // Copies removed while stopped leave their entry behind.
        for (auto it = s_quietStopped.begin(); it != s_quietStopped.end();)
            it = cNow - it->second.Since > 10min ? s_quietStopped.erase(it) : std::next(it);

        static BSFixedString s_moveStop("moveStop");
        const float cSpeed = LegSpeed(apActor);
        const bool cTaken = apActor->SendAnimationEvent(&s_moveStop);
        s_quietStopped[apActor->formID] = QuietStop{cNow, {}};
        spdlog::info("QuietStop: copy {:X} had no report for {:.1f} s while moving (speed {:.0f}); stopped ('moveStop' {})", apActor->formID, aQuietFor / 1000.0, cSpeed,
                     cTaken ? "taken" : "not taken");
    }

    static BSFixedString s_speed("Speed");
    apActor->animationGraphHolder.SetVariableFloat(&s_speed, 0.f);
    apActor->SetSpeed(0.f);
}

// Reports came back, and their variables are on the copy.
// It is started again once they say it moves, or it would slide in its standing state: its owner's own start is long gone.
// The first reports back can say it stands, then speed up (live-copy-quiet), so it is watched until it moves.
// One that stands through 2 s of reports is left to them.
void WatchQuietCopy(Actor* apActor) noexcept
{
    const auto stopped = s_quietStopped.find(apActor->formID);
    if (stopped == s_quietStopped.end())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    if (stopped->second.Back == std::chrono::steady_clock::time_point{})
        stopped.value().Back = cNow;
    const auto cBackFor = std::chrono::duration_cast<std::chrono::milliseconds>(cNow - stopped->second.Back).count();

    const float cSpeed = LegSpeed(apActor);
    if (cSpeed > 1.f)
    {
        static BSFixedString s_moveStart("moveStart");
        const bool cTaken = apActor->SendAnimationEvent(&s_moveStart);
        spdlog::info("QuietStop: copy {:X} moves again, {} ms after its reports came back (speed {:.0f}); started ('moveStart' {})", apActor->formID, cBackFor, cSpeed,
                     cTaken ? "taken" : "not taken");
        s_quietStopped.erase(stopped);
    }
    else if (cNow - stopped->second.Back > 2s)
    {
        spdlog::info("QuietStop: copy {:X} stood through {} ms of reports since they came back; left to them", apActor->formID, cBackFor);
        s_quietStopped.erase(stopped);
    }
}

// The copy's Speed slots when its legs are matched to the speed it is shown moving (StrideMatch::Applies), null when they are not.
const LegSlots* StrideSlots(Actor* apActor) noexcept
{
    const auto* pExtension = apActor->GetExtension();
    if (!pExtension || !StrideMatch::Applies(pExtension->IsRemote(), apActor->IsMount(), IsOnFoot(apActor), true))
        return nullptr;
    const LegSlots* pSlots = LegSlotsOf(apActor);
    return pSlots && pSlots->Humanoid && pSlots->Speed >= 0 ? pSlots : nullptr;
}

// aVRBody: a live VR player's copy, whose steps in their room move it while its graph's direction says forward.
void MatchStride(
    Actor* apActor, InterpolationComponent& aComponent, const glm::vec3& acPrevious, const glm::vec3& acPlaced, const bool aReset, const double aDeltaTime, const double aNow,
    const bool aVRBody, AnimationVariables& aVariables) noexcept
{
    const LegSlots* pSlots = StrideSlots(apActor);
    if (!pSlots)
    {
        aComponent.StrideDirection = -1.f;
        return;
    }

    const auto cSample = StrideMatch::GroundSpeed(acPrevious, acPlaced, aDeltaTime, aReset);
    if (cSample)
        aComponent.StrideSpeed = StrideMatch::Ease(aComponent.StrideSpeed, *cSample, aDeltaTime);
    else
    {
        aComponent.StrideSpeed = -1.f;
        aComponent.StrideDirection = -1.f;
        aComponent.StrideMismatch = {};
        aComponent.Sprint = {};
        return;
    }

    const float cOwner = static_cast<size_t>(pSlots->Speed) < aVariables.Floats.size() ? aVariables.Floats[pSlots->Speed] : 0.f;
    const float cLegs = StrideMatch::LegSpeed(aComponent.StrideSpeed, cOwner);
    StrideMatch::Drive(aVariables, pSlots->Speed, pSlots->Sampled, cLegs);

    // The legs go the way the copy is shown moving against its body's facing, so a step to the side strafes and one back walks backwards.
    if (aVRBody && pSlots->Direction >= 0 && aComponent.StrideSpeed >= StrideMatch::kStillSpeed)
    {
        if (const auto cStep = StrideMatch::StepDirection(acPrevious, acPlaced, apActor->rotation.z))
            aComponent.StrideDirection = StrideMatch::EaseDirection(aComponent.StrideDirection, *cStep, aDeltaTime);
    }
    else
        aComponent.StrideDirection = -1.f;
    if (aComponent.StrideDirection >= 0.f && static_cast<size_t>(pSlots->Direction) < aVariables.Floats.size())
        aVariables.Floats[pSlots->Direction] = aComponent.StrideDirection;
    if (StrideMatch::SayMismatch(aComponent.StrideMismatch, StrideMatch::Mismatch(cLegs, cOwner), aNow))
        spdlog::debug(
            "StrideMatch: copy {:X} is shown moving {:.0f} units a second while its owner's graph says {:.0f}; its legs follow what is shown", apActor->formID, cLegs, cOwner);

    static BSFixedString s_isSprinting("IsSprinting");
    bool sprinting = false;
    apActor->animationGraphHolder.GetVariableBool(&s_isSprinting, &sprinting);
    if (StrideMatch::StopsSprint(aComponent.Sprint, sprinting && aComponent.StrideSpeed >= 0.f, aComponent.StrideSpeed, aNow))
    {
        static BSFixedString s_sprintStop("SprintStop");
        const bool cTaken = apActor->SendAnimationEvent(&s_sprintStop);
        spdlog::debug(
            "StrideMatch: copy {:X} sprinted while shown moving {:.0f} units a second; 'SprintStop' {}", apActor->formID, aComponent.StrideSpeed, cTaken ? "taken" : "not taken");
    }
    // Back at a sprint's pace after a slow spell ended its sprint: it sprints again, as its legs could not keep that pace in a run.
    else if (StrideMatch::StartsSprint(aComponent.Sprint, sprinting && aComponent.StrideSpeed >= 0.f, aComponent.StrideSpeed, aNow))
    {
        static BSFixedString s_sprintStart("SprintStart");
        const bool cTaken = apActor->SendAnimationEvent(&s_sprintStart);
        spdlog::debug(
            "StrideMatch: copy {:X} shown moving {:.0f} units a second without its sprint; 'SprintStart' {}", apActor->formID, aComponent.StrideSpeed,
            cTaken ? "taken" : "not taken");
    }
}

// A VR player crouching for real is shown in its sneak stance while its legs are not sent (CrouchStance). Not with leg trackers
// or a lower body an add-on fills in (HasLegs), whose legs show the crouch, nor while its owner's sneak toggle has it sneaking.
// A remote player's copy shown starting or stopping to sneak, once per change: from its owner's sneak, or from a VR player's real
// crouch (MatchCrouch). Read before MatchCrouch writes the stance's variables.
void NoteSneak(Actor* apActor, const InterpolationComponent& acComponent, const AnimationVariables& acVariables) noexcept
{
    const auto* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->IsRemotePlayer())
        return;
    const LegSlots* pSlots = StrideSlots(apActor);
    const bool cButton = SneakState::Of(apActor->actorState.flags1) || (pSlots && CrouchStance::OwnerSneaks(acVariables, pSlots->Sneaking));
    const bool cSneaking = cButton || acComponent.Crouch.Crouched;

    static TiltedPhoques::Map<uint32_t, bool> s_shown;
    if (s_shown.size() > 256)
        s_shown.clear();
    const auto it = s_shown.try_emplace(apActor->formID, false).first;
    if (it->second == cSneaking)
        return;
    it.value() = cSneaking;

    uint32_t serverId = 0;
    if (entt::locator<World>::has_value())
    {
        auto& world = World::Get();
        const auto view = world.view<FormIdComponent, RemoteComponent>();
        if (const auto cEntity = Utils::FindByFormId(view, apActor->formID))
            serverId = view.get<RemoteComponent>(*cEntity).Id;
    }
    spdlog::debug(
        "Sneak: copy {:X} (server id {:X}) sneaking {} ({})", apActor->formID, serverId, cSneaking ? 1 : 0,
        !cSneaking ? "stood up"
        : cButton  ? "from button"
                   : "from VR hip drop");
}

void MatchCrouch(Actor* apActor, InterpolationComponent& aComponent, const VRPose& acPose, const bool aVRBody, const double aNow, AnimationVariables& aVariables) noexcept
{
    if (!aVRBody && !aComponent.Crouch.Crouched)
        return;
    const LegSlots* pSlots = StrideSlots(apActor);
    const bool cOwnerSneaks = SneakState::Of(apActor->actorState.flags1) || (pSlots && CrouchStance::OwnerSneaks(aVariables, pSlots->Sneaking));
    const auto* pExtension = apActor->GetExtension();
    // Down, kneeling in its graph for its owner's fall, seated, sleeping, riding or swimming: its graph plays that, and is not sent
    // the stance's events.
    const bool cMounted = aComponent.PinTo != nullptr || aComponent.PlaceOnMount || (Actor::CanReadMount() && apActor->IsOnMount());
    const bool cDown = (pExtension && pExtension->KneelsInGraph.load(std::memory_order_relaxed)) || apActor->actorState.IsDeadOrDying() || apActor->actorState.IsBleedingOut() ||
                       apActor->actorState.IsKnockedDown() || CrouchStance::LowForOtherReason(apActor->actorState.flags1, cMounted);
    const bool cAllowed = pSlots && aVRBody && !acPose.HasLegs && !cOwnerSneaks && !cDown;
    const float cHip = acPose.HasHips ? acPose.HipOffset[2] : std::numeric_limits<float>::quiet_NaN();
    const bool cWas = aComponent.Crouch.Crouched;
    const bool cCrouched = CrouchStance::Update(aComponent.Crouch, cAllowed, cHip, aNow);
    if (cCrouched != cWas)
    {
        if (cCrouched)
            s_crouching.insert(apActor->formID);
        else
            s_crouching.erase(apActor->formID);
        // The owner's own sneak plays from their toggle: it is not stood out of here.
        if (!cOwnerSneaks && !cDown)
        {
            static BSFixedString s_sneakStart("SneakStart");
            static BSFixedString s_sneakStop("SneakStop");
            const bool cTaken = apActor->SendAnimationEvent(cCrouched ? &s_sneakStart : &s_sneakStop);
            spdlog::debug(
                "CrouchStance: VR player copy {:X} {} (hips {:.0f}, standing {:.0f}); '{}' {}", apActor->formID, cCrouched ? "crouches" : "stands up", cHip,
                aComponent.Crouch.Standing, cCrouched ? "SneakStart" : "SneakStop", cTaken ? "taken" : "not taken");
        }
    }
    if (cCrouched && pSlots)
        CrouchStance::Hold(aVariables, pSlots->Sneaking, pSlots->InSneak);
}
} // namespace

void InterpolationSystem::HoldStride(Actor* apActor, AnimationVariables& aVariables) noexcept
{
    const LegSlots* pSlots = StrideSlots(apActor);
    if (!pSlots)
        return;
    static BSFixedString s_speed("Speed");
    float legs = 0.f;
    if (apActor->animationGraphHolder.GetVariableFloat(&s_speed, &legs))
        StrideMatch::Drive(aVariables, pSlots->Speed, pSlots->Sampled, legs);
    // A real crouch shown as the sneak stance stays through the owner's actions, whose variables stand.
    if (s_crouching.find(apActor->formID) != s_crouching.end() && !CrouchStance::OwnerSneaks(aVariables, pSlots->Sneaking))
        CrouchStance::Hold(aVariables, pSlots->Sneaking, pSlots->InSneak);
}

bool InterpolationSystem::OnFoot(const Actor* apActor) noexcept
{
    return IsOnFoot(apActor);
}

float InterpolationSystem::FootSpeed(Actor* apActor) noexcept
{
    return IsOnFoot(apActor) ? LegSpeed(apActor) : 0.f;
}

float InterpolationSystem::ReportedSpeed(const uint32_t aFormId, const InterpolationComponent& acInterpolationComponent) noexcept
{
    const auto& points = acInterpolationComponent.TimePoints;
    if (points.size() < 2 || s_quietStopped.find(aFormId) != s_quietStopped.end())
        return 0.f;

    const auto& last = points.back();
    const auto& before = *std::prev(points.end(), 2);
    if (last.Tick <= before.Tick)
        return 0.f;

    return glm::distance(last.Position, before.Position) * 1000.f / static_cast<float>(last.Tick - before.Tick);
}

uint64_t InterpolationSystem::GetPlaybackTick(const InterpolationComponent& acInterpolationComponent) noexcept
{
    const double cRiderTick = acInterpolationComponent.Clock.IsRunning() ? acInterpolationComponent.Clock.GetRenderTick() : 0.0;
    const double cTick = SeatPin::PlaybackTick(acInterpolationComponent.PlaceOnMount, acInterpolationComponent.PlaceAtTick, cRiderTick);
    return cTick > 0.0 ? static_cast<uint64_t>(cTick) : 0;
}

void InterpolationSystem::Update(
    Actor* apActor, InterpolationComponent& aInterpolationComponent, const double aNow, const bool aDeadAtOwner, const uint64_t aLastActionTick,
    const uint32_t aOwnerFlags1) noexcept
{
    // A place kept for the frame end by an earlier step goes: only one kept in this step is given (PosedCopyPlacement).
    if (apActor)
        PosedCopyPlacement::DropKeptPlace(apActor->formID);
    // Alive again at its owner's: the pose it rested in is not settled into at its next death.
    if (!aDeadAtOwner && aInterpolationComponent.RestPose.HasData)
        aInterpolationComponent.RestPose = {};

    const double cDeltaTime = aInterpolationComponent.Clock.Advance(aNow);

    auto& movements = aInterpolationComponent.TimePoints;

    // A copy that changed hands is placed at its new owner's first point at once (gliding there), not left standing until the second.
    const bool cOnePoint = movements.size() == 1;
    if (movements.size() < 2 && !cOnePoint)
    {
        // A held or absent movement buffer must still withdraw an expired live body pose.
        PublishPose(apActor, aInterpolationComponent, aNow, GetPlaybackTick(aInterpolationComponent));
        return;
    }

    // Rider movement, pose and actions use the same tick as the horse.
    const bool cOnMount = aInterpolationComponent.PlaceOnMount && aInterpolationComponent.PlaceAtTick > 0.0;
    const double cRenderTick = SeatPin::PlaybackTick(cOnMount, aInterpolationComponent.PlaceAtTick, aInterpolationComponent.Clock.GetRenderTick());
    const uint64_t aTick = GetPlaybackTick(aInterpolationComponent);

    while (movements.size() > 2)
    {
        const auto second = *(++movements.begin());
        if (aTick > second.Tick)
            movements.pop_front();
        else
            break;
    }

    const auto& first = *(movements.begin());
    const auto& second = cOnePoint ? first : *(++movements.begin());

    // Before the first point it holds the first position; past the newest one it keeps going the way the actor moved.
    const double cTickDelta = second.Tick > first.Tick ? static_cast<double>(second.Tick - first.Tick) : 0.0;
    const bool cIsTeleport = glm::distance(first.Position, second.Position) > cTeleportDistance;
    float delta = 1.f;
    bool isExtrapolating = false;
    glm::vec3 target{};
    if (cRenderTick <= static_cast<double>(second.Tick))
    {
        if (cTickDelta > 0.0)
            delta = static_cast<float>(std::clamp((cRenderTick - static_cast<double>(first.Tick)) / cTickDelta, 0.0, 1.0));
        // A teleport happens at once, when its point is reached.
        if (cIsTeleport && delta < 1.f)
            delta = 0.f;
        target = TiltedPhoques::Lerp(first.Position, second.Position, delta);
    }
    else
    {
        target = second.Position;
        if (!cIsTeleport && cTickDelta > 0.0)
        {
            const double cExtrapolationTime = std::min(cRenderTick - static_cast<double>(second.Tick), cMaxExtrapolation);
            target = TiltedPhoques::Lerp(first.Position, second.Position, static_cast<float>(1.0 + cExtrapolationTime / cTickDelta));
            isExtrapolating = true;
        }
    }

    // Held past its only point: the next point ends that hold the way it ends an extrapolation, with a fade.
    if (cOnePoint && cRenderTick > static_cast<double>(first.Tick))
        isExtrapolating = true;

    // The late point arrived, so the actor is no longer where extrapolation put it: the difference fades out.
    auto& positionError = aInterpolationComponent.PositionError;
    if (aInterpolationComponent.HasNewTimePoint && aInterpolationComponent.IsExtrapolating)
        positionError = aInterpolationComponent.Position - aInterpolationComponent.HandOffShown - target;
    if (cIsTeleport || glm::length(positionError) > cMaxPositionCorrection)
        positionError = {};
    else
        positionError *= static_cast<float>(std::exp(-cDeltaTime / cPositionCorrectionTime));
    aInterpolationComponent.HasNewTimePoint = false;
    aInterpolationComponent.IsExtrapolating = isExtrapolating;

    // The first placement after the copy was made or changed hands: from where it stands to its new owner's place, over
    // RemoteSmoothing::kHandOffBlendMs, instead of a jump. Its new owner started it from its own copy, shown a little behind.
    const bool cFirstPlacement = !aInterpolationComponent.Placed;
    if (cFirstPlacement)
    {
        aInterpolationComponent.Placed = true;
        const glm::vec3 cShown = apActor ? glm::vec3(apActor->position) : aInterpolationComponent.Position;
        aInterpolationComponent.HandOffGap = !cIsTeleport ? RemoteSmoothing::HandOffGap(cShown, target) : glm::vec3{};
        aInterpolationComponent.HandOffSince = aNow;
    }
    glm::vec3 handOff{};
    if (aInterpolationComponent.HandOffGap != glm::vec3{})
    {
        const float cWeight = cIsTeleport ? 0.f : RemoteSmoothing::HandOffWeight(aNow - aInterpolationComponent.HandOffSince);
        if (cWeight <= 0.f)
            aInterpolationComponent.HandOffGap = {};
        handOff = aInterpolationComponent.HandOffGap * cWeight;
        // Off a horse, down to its owner's place on the ground, never through it.
        if (aInterpolationComponent.GroundGlide)
            handOff = SeatPin::AboveGround(handOff);
    }
    if (aInterpolationComponent.HandOffGap == glm::vec3{})
        aInterpolationComponent.GroundGlide = false;
    aInterpolationComponent.HandOffShown = handOff;
    aInterpolationComponent.Correction = positionError + handOff;
    // A seated rider takes its horse's correction, so that a late point or a hand-off of the horse carries both.
    if (cOnMount)
        aInterpolationComponent.Correction = aInterpolationComponent.PlaceCorrection;

    aInterpolationComponent.Position = target + aInterpolationComponent.Correction;
    const NiPoint3 position{aInterpolationComponent.Position};

    // The pose and actor position must come from the same instant.
    // Different playback ticks put the visible body and hands ahead of the actor/controller that combat collides with.
#ifdef SKYRIMVR
    // TEMPORARY, see IsPoseSpike: a remote player's poses that jumped away and straight back are counted; all are still shown.
    if (apActor && apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer())
    {
        std::array<const InterpolationComponent::TimePoint*, 64> posed{};
        size_t posedCount = 0;
        for (const auto& point : movements)
            if (point.VRPoseData.HasData && posedCount < posed.size())
                posed[posedCount++] = &point;

        PoseSpikes& spikes = s_poseSpikes[apActor->formID];
        for (size_t i = 1; i + 1 < posedCount; ++i)
            if (posed[i]->Tick > spikes.LastTick && IsPoseSpike(posed[i - 1]->VRPoseData, posed[i]->VRPoseData, posed[i + 1]->VRPoseData))
            {
                spikes.LastTick = posed[i]->Tick;
                ++spikes.Count;
            }
        const auto cNow = std::chrono::steady_clock::now();
        if (spikes.NextLog == std::chrono::steady_clock::time_point{})
            spikes.NextLog = cNow + 30s;
        else if (cNow >= spikes.NextLog)
        {
            if (spikes.Count)
                spdlog::info("PoseSpike: copy {:X}: {} poses in 30 s jumped away from the one before and straight back (shown as they came)", apActor->formID, spikes.Count);
            spikes.Count = 0;
            spikes.NextLog = cNow + 30s;
        }
    }
#endif
    PublishPose(apActor, aInterpolationComponent, aNow, aTick);

    if (!apActor)
        return;

    // A copy swims as its own animation has it, not as its owner says: one line each time that starts or stops here.
    {
        static TiltedPhoques::Map<uint32_t, bool> s_swimming;
        const bool cSwimming = apActor->actorState.IsSwimming();
        if (auto it = s_swimming.find(apActor->formID); it == s_swimming.end())
            s_swimming[apActor->formID] = cSwimming;
        else if (it->second != cSwimming)
        {
            it.value() = cSwimming;
            spdlog::info(
                "SwimDiag: copy {:X} {} swimming here at height {:.0f}; its owner has it at height {:.0f}", apActor->formID, cSwimming ? "starts" : "stops", apActor->position.z,
                position.z);
        }
    }

    const ActorExtension* pExtension = apActor->GetExtension();
    // Flat can accept a fall into its graph while leaving life/knock bits at alive/normal.
    // The native action alone is insufficient: only consecutive real pelvis/feet observations count.
    bool cObservedKnock = false;
    bool cObservedEssential = false;
    uint64_t cObservedLife = 0;
    uint64_t cObservedFallTick = 0;
    bool cObservedRecoveryProven = false;
#ifndef SKYRIMVR
    if (auto fall = s_replayedFalls.find(apActor->formID); fall != s_replayedFalls.end())
    {
        ReplayedFall& observed = fall.value();
        const bool remoteNpc = pExtension && pExtension->IsRemote() && !pExtension->IsRemotePlayer();
        const bool sameLifetime = remoteNpc && observed.Authority == AuthorityOf(aInterpolationComponent) && observed.ActorAtArm == apActor &&
                                  observed.RootAtArm == apActor->GetNiNode() && observed.RevivedAtArm == pExtension->RevivedSinceDeath &&
                                  observed.NativeLifeAtArm == pExtension->NativeLifeGeneration.load(std::memory_order_relaxed);
        const bool invalid = !CopyBodyRules::MayObserveFall(true, remoteNpc, apActor->actorState.IsDeadOrDying(), aDeadAtOwner, sameLifetime);
        if (invalid)
            Forget(apActor->formID); // main-thread lifetime cleanup; the native hook only changes atomics
        else
        {
            float height = 0.f;
            glm::vec3 pelvis{};
            const bool wasDown = observed.Observation.Down;
            const bool readable = VRBodySync::BodyPelvisHeight(apActor, height, pelvis) && std::isfinite(height);
            // A native DeathAnim can leave a copy kneeling before its owner's later push throws it flat.
            // Kneeling alone must not count as that ragdoll: require two real flat samples for the transition.
            if (observed.EssentialDown && observed.KnockRequested && CopyBodyRules::ObserveFall(observed.KnockObservation, readable, height, false))
            {
                observed.EssentialDown = false;
                observed.Observation = observed.KnockObservation;
                spdlog::debug(
                    "KnockDown: NPC copy {:X} went from its accepted essential fall to its later Ragdoll (pelvis {:.0f} above its foot); tracking its actual flat body",
                    apActor->formID, height);
            }
            const auto cSinceKnock = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - observed.KnockRequestedAt).count();
            const bool down = CopyBodyRules::HoldEssentialFall(observed.EssentialDown, observed.KnockRequested, static_cast<uint32_t>(std::max<long long>(cSinceKnock, 0)))
                                  ? observed.Observation.Down
                                  : CopyBodyRules::ObserveFall(observed.Observation, readable, height, observed.EssentialDown);
            cObservedKnock = down && !observed.EssentialDown;
            cObservedEssential = down && observed.EssentialDown;
            cObservedLife = down ? observed.NativeLifeAtArm : 0;
            cObservedFallTick = down ? observed.ActionTickAtArm : 0;
            cObservedRecoveryProven = down && observed.Recovery.Allows(observed.ActionTickAtArm, aLastActionTick);
            if (!wasDown && down)
                spdlog::debug(
                    "KnockDown: NPC copy {:X} lies after its accepted {} action (pelvis {:.0f} above its foot, native life {}, knock {}); observed from its bones", apActor->formID,
                    observed.EssentialDown ? "DeathAnim" : "Ragdoll", height, apActor->actorState.LifeState(), CopyBodyRules::KnockState(apActor->actorState.flags1));
            if (wasDown && !down)
                spdlog::debug(
                    "KnockObserve: copy {:X} stood from its observed {} after {} ms (pelvis {:.0f}, owner flags {:#x}, local flags {:#x}); observed native processing ends",
                    apActor->formID, observed.EssentialDown ? "essential fall" : "ragdoll",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - observed.ArmedAt).count(), height, aOwnerFlags1,
                    apActor->actorState.flags1);
            if ((wasDown && !down) || (!down && std::chrono::steady_clock::now() - observed.ArmedAt > 20s))
                s_replayedFalls.erase(fall);
        }
    }
#endif
    if (pExtension)
        const_cast<ActorExtension*>(pExtension)->ObservedCopyFall.store(cObservedLife, std::memory_order_relaxed);
    const bool cObservedWithoutKnock = CopyBodyRules::ObservedWithoutNativeKnock(cObservedKnock || cObservedEssential, apActor->actorState.flags1);
    // The game may end this copy's bleedout from an actor job, which reads its owner's word from here (HookPerformAction).
    // Known as OnBleedingOut knows it: after an observed fall, only a word proven to come after it.
    if (pExtension && pExtension->IsRemote() && !pExtension->IsRemotePlayer())
        const_cast<ActorExtension*>(pExtension)
            ->OwnerWordOfCopy.store(
                static_cast<uint8_t>(CopyBodyRules::OwnerWordOf(aLastActionTick != 0 && (!cObservedFallTick || cObservedRecoveryProven), aOwnerFlags1, aDeadAtOwner)),
                std::memory_order_relaxed);

    // An NPC copy can go into the game's own bleedout here, from the low health it took from its owner, and copies finish no bleedout by themselves.
    // When its owner's NPC is up again and the copy refused "bleedOutStop", it lay there for good while the owner's fought on (Lydia, 2026-11-06).
    // So once its owner's word has had it up for a while, it is taken out: its health raised so it does not drop straight back, its life and
    // knock states cleared, and its graph sent the stop and back to standing. Never for a copy whose owner says it is dead.
    {
        const auto cNow = std::chrono::steady_clock::now();
        const bool cNativeBleedingOut = apActor->actorState.IsBleedingOut();
        // A copy kneeling in its graph only counts as bleeding out until it reads standing again.
        bool cKneelsInGraph = false;
        if (pExtension && pExtension->KneelsInGraph.load(std::memory_order_relaxed))
        {
            const uint64_t cLife = pExtension->NativeLifeGeneration.load(std::memory_order_relaxed);
            auto kneel = s_graphKneels.find(apActor->formID);
            if (kneel == s_graphKneels.end())
            {
                if (s_graphKneels.size() > 256)
                    s_graphKneels.clear();
                kneel = s_graphKneels.emplace(apActor->formID, GraphKneel{cNow, 0, cLife, movements.back().Position}).first;
            }
            // Revived since, and kneeling again: a new kneel, not the old one's time and readings.
            else if (kneel.value().Life != cLife)
                kneel.value() = GraphKneel{cNow, 0, cLife, movements.back().Position};
            float height = 0.f;
            glm::vec3 pelvis{};
            const bool cReadable = VRBodySync::BodyPelvisHeight(apActor, height, pelvis) && std::isfinite(height);
            const auto cSeenFor = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - kneel->second.SeenAt).count());
            // A player's copy is never forgotten kneeling, as nothing else would stand it: its owner's up word or move does (PlayerKneelEnds).
            const bool cPlayerKneel = pExtension->IsRemotePlayer();
            bool cPlayerUp = false;
            if (cPlayerKneel)
            {
                GraphKneel& playerKneel = kneel.value();
                const uint64_t cFallTick = pExtension->OwnerFallTick.load(std::memory_order_relaxed);
                const auto cStop = s_playerOwnerStops.find(apActor->formID);
                const bool cStopAfterKneel = cStop != s_playerOwnerStops.end() && cFallTick != 0 && cStop->second > cFallTick;
                const bool cOwnerUp = CopyBodyRules::PlayerUpAfterKneel(aLastActionTick > cFallTick, cStopAfterKneel, aOwnerFlags1);
                if (!cOwnerUp)
                    playerKneel.OwnerUpSince = {};
                else if (playerKneel.OwnerUpSince == std::chrono::steady_clock::time_point{})
                    playerKneel.OwnerUpSince = cNow;
                const uint32_t cUpFor = playerKneel.OwnerUpSince == std::chrono::steady_clock::time_point{}
                                            ? 0
                                            : static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - playerKneel.OwnerUpSince).count());
                const float cMoved = glm::distance(movements.back().Position, playerKneel.At);
                cPlayerUp = CopyBodyRules::PlayerKneelEnds(cOwnerUp, cUpFor, cMoved);
                if (cPlayerUp)
                {
                    static BSFixedString s_stop("bleedOutStop");
                    static BSFixedString s_reset("IdleForceDefaultState");
                    const bool cStopTaken = apActor->SendAnimationEvent(&s_stop);
                    const bool cResetTaken = apActor->SendAnimationEvent(&s_reset);
                    spdlog::debug(
                        "KnockDown: player copy {:X} stood from its graph kneel after {} ms (owner up for {} ms, moved {:.0f}); 'bleedOutStop' {}, "
                        "'IdleForceDefaultState' {}",
                        apActor->formID, cSeenFor, cUpFor, cMoved, cStopTaken ? "taken" : "not taken", cResetTaken ? "taken" : "not taken");
                }
            }
            const uint32_t cJudgedFor = cPlayerKneel ? std::min(cSeenFor, CopyBodyRules::kKneelForgetMs - 1) : cSeenFor;
            if (apActor->actorState.IsDeadOrDying() || cPlayerUp || CopyBodyRules::KneelEnded(kneel.value().HighSamples, cJudgedFor, cReadable, height))
            {
                const_cast<ActorExtension*>(pExtension)->KneelsInGraph.store(false, std::memory_order_relaxed);
                s_graphKneels.erase(kneel);
                spdlog::debug("KnockDown: copy {:X} no longer kneels in its graph after {} ms (pelvis {:.0f})", apActor->formID, cSeenFor, height);
            }
            else
                cKneelsInGraph = true;
        }
        else if (!s_graphKneels.empty())
            s_graphKneels.erase(apActor->formID);
        const bool cBleedingOut = cNativeBleedingOut || cObservedEssential || cObservedWithoutKnock || cKneelsInGraph;
        auto it = s_bleedingOut.find(apActor->formID);
        if (!cBleedingOut)
        {
            if (it != s_bleedingOut.end() && cNow - it->second.TakenOutAt > 10s)
                s_bleedingOut.erase(it);
        }
        else
        {
            if (it == s_bleedingOut.end())
            {
                // Copies removed while down are never seen again here.
                if (s_bleedingOut.size() > 256)
                    s_bleedingOut.clear();
                it = s_bleedingOut.emplace(apActor->formID, BleedingOutCopy{}).first;
            }
            BleedingOutCopy& bleeding = it.value();
            CopyBodyRules::BleedingOut copy{};
            copy.PlayerCopy = !pExtension || pExtension->IsRemotePlayer();
            copy.BleedingOutHere = true;
            copy.DeadAtOwner = aDeadAtOwner;
            copy.OwnerWordKnown = aLastActionTick != 0 && (!cObservedFallTick || cObservedRecoveryProven);
            copy.OwnerFlags1 = aOwnerFlags1;
            copy.TakenOutLately = bleeding.TakenOutAt != std::chrono::steady_clock::time_point{} && cNow - bleeding.TakenOutAt < 10s;
            if (copy.OwnerWordKnown && CopyBodyRules::OwnerStands(aOwnerFlags1))
            {
                if (bleeding.OwnerUpSince == std::chrono::steady_clock::time_point{})
                    bleeding.OwnerUpSince = cNow;
                copy.OwnerUpForMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - bleeding.OwnerUpSince).count());
            }
            else
                bleeding.OwnerUpSince = {};

            // The game's own recovery is not called while it exits or loads, nor just after a load (CopyWork.h): it is tried again then.
            static CopyWork::LoadGate s_recoveryGate;
            if (CopyBodyRules::OnBleedingOut(copy) == CopyBodyRules::Step::TakeOut && CopyWorkSettled(s_recoveryGate))
            {
                bleeding.TakenOutAt = cNow;
                bleeding.OwnerUpSince = {};
                cObservedEssential = false;
                cObservedKnock = false;
                s_down.erase(apActor->formID);
                if (pExtension)
                    const_cast<ActorExtension*>(pExtension)->ObservedCopyFall.store(false, std::memory_order_relaxed);
#ifndef SKYRIMVR
                s_replayedFalls.erase(apActor->formID);
#endif
                const uint32_t cStateBefore = apActor->actorState.flags1;
                spdlog::debug(
                    "KnockOwnerUp: copy {:X} has a later owner word at tick {} flags {:#x}, up for {} ms; native bleedout {}, observed without native knock {}, fall tick {}",
                    apActor->formID, aLastActionTick, aOwnerFlags1, copy.OwnerUpForMs, cNativeBleedingOut, cObservedWithoutKnock, cObservedFallTick);
                // The game's own way first: a bleedout of its own here ends as the game ends one (Actor::SetLifeState alive, whose stop then
                // plays: HookPerformAction), and a kneel in its graph only gets the stop the game would perform. Raising its health below
                // may already end it, the game's way too. The graph events are kept for when the game's stop is refused.
                auto* pWritable = const_cast<ActorExtension*>(pExtension);
                const auto cHow = CopyBodyRules::RecoverHow(apActor->actorState.flags1, cKneelsInGraph);
                const bool cGameWay = pWritable && cHow != CopyBodyRules::Recovery::Events;
                // OnBleedingOut found its owner's NPC up: the game's stop may play during the take-out, and the word is put back after it.
                const uint8_t cWordBefore = pWritable ? pWritable->OwnerWordOfCopy.load(std::memory_order_relaxed) : 0;
                if (cGameWay)
                {
                    pWritable->OwnerWordOfCopy.store(static_cast<uint8_t>(CopyBodyRules::OwnerWord::Up), std::memory_order_relaxed);
                    pWritable->EngineStopPlayed.store(0, std::memory_order_relaxed);
                }
                const float cHealthBefore = apActor->GetActorValue(ActorValueInfo::kHealth);
                const float cFullHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
                // Its own health is what put it down: at the floor it would go straight back. Its owner's next correction sets it as its owner has it.
                if (cHealthBefore < cFullHealth)
                    apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, cFullHealth);
                bool cGameStopPlayed = false;
                if (cGameWay)
                {
                    cGameStopPlayed = pWritable->EngineStopPlayed.load(std::memory_order_relaxed) == 1;
                    if (!cGameStopPlayed && CopyBodyRules::RecoverHow(apActor->actorState.flags1, false) == CopyBodyRules::Recovery::SetLifeState)
                    {
                        apActor->SetLifeState(LifeStates::kAlive);
                        cGameStopPlayed = pWritable->EngineStopPlayed.load(std::memory_order_relaxed) == 1;
                    }
                    // Alive now, its graph still kneeling: the stop the game would have performed.
                    if (!cGameStopPlayed && !apActor->actorState.IsDeadOrDying() && !apActor->actorState.IsBleedingOut())
                        if (auto* pStop = Cast<BGSAction>(TESForm::GetById(CopyBodyRules::kActionBleedoutStop)))
                        {
                            // As Actor::SetLifeState builds it: no input kind, no target.
                            TESActionData stopData(0, apActor, pStop, nullptr);
                            cGameStopPlayed = ActorMediator::Get()->PerformAction(&stopData);
                        }
                    if (cGameStopPlayed)
                        pWritable->KneelsInGraph.store(false, std::memory_order_relaxed);
                    pWritable->OwnerWordOfCopy.store(cWordBefore, std::memory_order_relaxed);
                }
                // Its knock state is its own and would hold it down; its life state too, when the game did not end the bleedout.
                apActor->actorState.flags1 &= cGameStopPlayed ? ~0xE000000u : ~(0x1E00000u | 0xE000000u);
                bool cStopTaken = cGameStopPlayed;
                bool cResetTaken = false;
                if (!cGameStopPlayed)
                {
                    BSFixedString stop("bleedOutStop");
                    cStopTaken = apActor->SendAnimationEvent(&stop);
                    BSFixedString reset("IdleForceDefaultState");
                    cResetTaken = apActor->SendAnimationEvent(&reset);
                    if (pWritable)
                        pWritable->KneelsInGraph.store(false, std::memory_order_relaxed);
                }
                spdlog::debug(
                    "KnockDown: NPC copy {:X} taken out {}", apActor->formID,
                    cGameStopPlayed ? (cHow == CopyBodyRules::Recovery::SetLifeState ? "by the game's own end of its bleedout" : "with the game's own stop")
                                    : "with the graph events");
                // The copy has lain where its own fall put it while its owner moved on. Its first upright placement
                // closes that gap at the same bounded rate as a knockdown's get-up, rather than snapping to the owner.
                const glm::vec3 cGap = static_cast<glm::vec3>(apActor->position) - static_cast<glm::vec3>(position);
                s_rising[apActor->formID] = Rising{cNow, 0.f, glm::length(cGap) <= cGetUpCatchUpDistance ? cGap : glm::vec3{}};
                spdlog::debug(
                    "KnockDown: NPC copy {:X} lay in its own bleedout while its owner's NPC was up for {} ms; taken out of its bleedout (life state {}, knock state {}, "
                    "health {:.0f} raised to {:.0f}; 'bleedOutStop' {}, 'IdleForceDefaultState' {})",
                    apActor->formID, copy.OwnerUpForMs, ActorState::LifeStateOf(cStateBefore), CopyBodyRules::KnockState(cStateBefore), cHealthBefore,
                    apActor->GetActorValue(ActorValueInfo::kHealth), cStopTaken ? "taken" : "not taken", cResetTaken ? "taken" : "not taken");
            }
        }
    }

    // A dying body falls with its own ragdoll, and a downed one has to be able to get back up where it fell.
    // Forcing either to follow its owner's position pulls the body away from the animation playing on it.
    if (apActor->actorState.IsDying() || apActor->actorState.IsBleedingOut() || cObservedEssential)
        return;

    // Another player's copy never goes down here, and its AI does not run here to finish a fall (HookActorProcess).
    // A knock that a hit here leaves on it stays for good, and no sword hit lands on it here after that.
    // So it stands again at once, as its owner's game decides whether that player goes down.
    // Its owner's own fall knocks it down here, it lies while its owner lies, and the game's own
    // get-up runs once its owner gets up. A knock its owner did not ask for is still taken off at once.
    if (pExtension && pExtension->IsRemotePlayer() && !apActor->actorState.IsDead())
    {
        const auto cNow = std::chrono::steady_clock::now();
        const uint32_t cKnockHere = PlayerKnockMirror::KnockState(apActor->actorState.flags1);
        auto& mirror = s_playerKnocks[apActor->formID];
        // Dying and bleeding-out copies returned above. On VR an admitted "Ragdoll" knocks it (AdmitPlayerRagdoll), or its graph's own.
        const bool cMayFall = PlayerKnockMirror::MayFall(false, false, OnFoot(apActor), aInterpolationComponent.PinTo != nullptr);
        const auto cAct = mirror.Frame(cKnockHere, cMayFall, cNow, kFrameKnocksPlayerCopy);
#ifdef SKYRIMVR
        // Its owner's fall told only by knock bits, with no "Ragdoll" to replay: the copy's graph is sent its own, once per fall.
        if (cAct == PlayerKnockMirror::Act::None && mirror.TakeGraphKnock(cKnockHere, cMayFall, cNow))
        {
            BSFixedString event("Ragdoll");
            const bool cTaken = apActor->SendAnimationEvent(&event);
            spdlog::debug("KnockDown: player copy {:X} goes down as its owner did, told by its knock bits; its graph {} 'Ragdoll'", apActor->formID, cTaken ? "took" : "refused");
            if (cTaken)
                return;
        }
#endif
        auto* pMirrored = const_cast<ActorExtension*>(pExtension);
        pMirrored->OwnerKnockRuns.store(mirror.RunsProcess(cKnockHere), std::memory_order_relaxed);
        switch (cAct)
        {
        case PlayerKnockMirror::Act::StandNow:
        {
            static std::chrono::steady_clock::time_point s_nextSaid{};
            if (cNow >= s_nextSaid)
            {
                s_nextSaid = cNow + 10s;
                spdlog::debug("KnockDown: player copy {:X} was knocked here (knock state {}) while its owner stands; stood up again", apActor->formID, cKnockHere);
            }
            apActor->actorState.flags1 &= ~0xE000000u;
            pMirrored->OwnerKnockRuns.store(false, std::memory_order_relaxed);
            break;
        }
        case PlayerKnockMirror::Act::Knock:
            // The game's own knock, with no push: it falls where it stands, and its knock state gets it up again.
            // On VR the owner's "Ragdoll" is played on the copy instead (AnimationSystem): that knock threw the VR player far (PlayerService).
#ifndef SKYRIMVR
            if (apActor->currentProcess)
            {
                const NiPoint3 cAt = apActor->position;
                apActor->currentProcess->KnockExplosion(apActor, &cAt, 0.f);
                pMirrored->OwnerKnockRuns.store(mirror.RunsProcess(PlayerKnockMirror::KnockState(apActor->actorState.flags1)), std::memory_order_relaxed);
            }
#endif
            spdlog::debug(
                "KnockDown: player copy {:X} goes down as its owner did (knock state now {})", apActor->formID, PlayerKnockMirror::KnockState(apActor->actorState.flags1));
            if (PlayerKnockMirror::KnockState(apActor->actorState.flags1) != 0)
                return;
            break;
        case PlayerKnockMirror::Act::Hold:
        case PlayerKnockMirror::Act::LetUp:
            // It lies where it fell, as an NPC copy's fall does; moved, its body would glide after its owner.
            return;
        case PlayerKnockMirror::Act::NudgeGetUp:
        {
            BSFixedString event("GetUpBegin");
            const bool cTaken = apActor->SendAnimationEvent(&event);
            spdlog::debug(
                "KnockDown: player copy {:X} waited with its get-up queued while its owner is up; sent it 'GetUpBegin' ({})", apActor->formID, cTaken ? "taken" : "not taken");
            return;
        }
        case PlayerKnockMirror::Act::Risen:
        {
            const glm::vec3 cGap = static_cast<glm::vec3>(apActor->position) - static_cast<glm::vec3>(position);
            s_rising[apActor->formID] = Rising{cNow, 0.f, glm::length(cGap) <= cGetUpCatchUpDistance ? cGap : glm::vec3{}};
            spdlog::debug("KnockDown: player copy {:X} is up again as its owner is, {:.0f} units from where its owner has it", apActor->formID, glm::length(cGap));
            break;
        }
        case PlayerKnockMirror::Act::None: break;
        }
    }

    // An NPC copy thrown to the floor lies in its own ragdoll until it gets up here (its knock state is its own).
    // Moved meanwhile, the body would glide along the floor behind its owner (live-copy-knockdown).
    const bool cKnockedNpc = !apActor->actorState.IsDead() && (apActor->actorState.IsKnockedDown() || cObservedKnock) && pExtension && !pExtension->IsRemotePlayer();
    {
        // KNOCK_STATE_ENUM 5: the ragdoll has come to rest and the get-up is queued.
        constexpr uint32_t cKnockQueued = 5;
        const uint32_t cKnockState = (apActor->actorState.flags1 >> 25) & 0x7;
        const auto cNow = std::chrono::steady_clock::now();
        const auto it = s_down.find(apActor->formID);
        if (cKnockedNpc && it == s_down.end())
        {
            s_down[apActor->formID] = Down{cNow, {}, 0, false, false, aLastActionTick, ++s_falls};
            spdlog::info("KnockDown: NPC copy {:X} lies in its own ragdoll (knock state {}); it stays where it fell until it gets up", apActor->formID, cKnockState);
        }
        else if (cKnockedNpc)
        {
            // Here the owner is no longer bleeding out (that returned above), so the copy should get up too.
            // A copy that fell into its own bleedout has the owner's life state written over it, and its queued get-up then never starts.
            // The graph's own get-up event starts it (live-copy-bleedout).
            // A get-up can fall back into the queue, as when the owner's bleedOutStop comes while it plays.
            // So it is sent again each time it waits queued that long once more, a few times at most.
            constexpr uint32_t cMaxGetUps = 3;
            Down& down = it.value();
            if (cKnockState != cKnockQueued)
                down.QueuedSince = {};
            else if (down.QueuedSince == std::chrono::steady_clock::time_point{})
                down.QueuedSince = cNow;
            else if (down.GetUpsSent < cMaxGetUps && cNow - down.QueuedSince > 1500ms)
            {
                ++down.GetUpsSent;
                BSFixedString event("GetUpBegin");
                const bool cTaken = apActor->SendAnimationEvent(&event);
                spdlog::info("KnockDown: NPC copy {:X} waited {} ms with its get-up queued; sent it 'GetUpBegin' ({}, try {})", apActor->formID,
                             std::chrono::duration_cast<std::chrono::milliseconds>(cNow - down.QueuedSince).count(), cTaken ? "taken" : "not taken", down.GetUpsSent);
                down.QueuedSince = {};
            }
            // Every get-up refused while its owner's NPC stands elsewhere: the copy would lie there for good while it fights on over there.
            // The other player could neither find it nor hit it (a brawl, 2026-11-15).
            // The game's own move puts it on its feet where its owner has it, and it follows from there.
            constexpr float cOwnerAway = 100.f;
            const float cApart = glm::distance(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(position));
            const auto cStoodBefore = s_stoodUp.find(apActor->formID);
            const bool cStoodLately = cStoodBefore != s_stoodUp.end() && cNow - cStoodBefore->second < 30s;
            TESObjectCELL* pCell = apActor->GetParentCellEx();
            if (down.GetUpsSent >= cMaxGetUps && !down.Stood && !cStoodLately && pCell && cKnockState == cKnockQueued && cNow - down.Since > 6s && cApart > cOwnerAway)
            {
                down.Stood = true;
                if (s_stoodUp.size() > 256)
                    s_stoodUp.clear();
                s_stoodUp[apActor->formID] = cNow;
                apActor->actorState.flags1 &= ~0xE000000u;
                // The move drops its 3D for a moment; noted, discovery does not take that for a copy gone.
                DiscoveryService::NoteMovedHere(apActor->formID);
                apActor->MoveTo(pCell, position);
                spdlog::info("KnockDown: NPC copy {:X} could not get up after {} tries while its owner stands {:.0f} units away; moved there on its feet", apActor->formID,
                             down.GetUpsSent, cApart);
            }
            if (!down.SaidLong && cNow - down.Since > 15s)
            {
                down.SaidLong = true;
                spdlog::warn("KnockDown: NPC copy {:X} still lies after 15 s (knock state {})", apActor->formID, cKnockState);
            }
        }
        else if (!cKnockedNpc && it != s_down.end())
        {
            // How far it stood up from where its owner has it: the copy jumps that far when it follows again.
            spdlog::info("KnockDown: NPC copy {:X} is up again after {} ms, {:.0f} units from where its owner has it", apActor->formID,
                         std::chrono::duration_cast<std::chrono::milliseconds>(cNow - it->second.Since).count(),
                         glm::distance(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(position)));
            s_down.erase(it);
            const glm::vec3 cGap = static_cast<glm::vec3>(apActor->position) - static_cast<glm::vec3>(position);
            s_rising[apActor->formID] = Rising{cNow, 0.f, glm::length(cGap) <= cGetUpCatchUpDistance ? cGap : glm::vec3{}};
        }
    }
    if (cKnockedNpc)
    {
        // Carried over there, as a player picks up a rabbit, it is drawn from its owner's bones; once they stop, its ragdoll goes where the owner left it.
        // The owner's push is not replayed here, so the copy collapses where it stands. Once its ragdoll has come to rest while its owner's NPC
        // still lies elsewhere, it is put there, once, so that its get-up starts where the owner's does (live-copy-knockdown).
        // PlaceCorpse moves a living body only in those two cases; otherwise a copy's own fall is left where it lies.
        const auto it = s_down.find(apActor->formID);
        CopyBodyRules::Knocked knocked{};
        VRBodySync::CorpsePlacement how{};
        if (it != s_down.end())
        {
            knocked.KnockStateHere = CopyBodyRules::KnockState(apActor->actorState.flags1);
            knocked.OwnerStillDown = CopyBodyRules::OwnerStillDown(aLastActionTick != it->second.ActionTickAtDown, aOwnerFlags1);
            knocked.ApartUnits = glm::length(glm::vec2{apActor->position.x - position.x, apActor->position.y - position.y});
            how.Fall = it->second.Fall;
        }
        how.Authority = AuthorityOf(aInterpolationComponent);
        how.pOwnerPose = &aInterpolationComponent.InterpolatedVRPose;
        how.OwnerBonesArrive = OwnerBonesArrive(aInterpolationComponent, aNow);
        how.OwnerBonesMove = OwnerBonesMove(aInterpolationComponent);
        // Once per fall, and only once a move did it: PlaceCorpse keeps that, and says when it happened (KnockDown).
        how.KnockedWhereOwnerFell = CopyBodyRules::PlaceWhereOwnerFell(knocked);
        VRBodySync::PlaceCorpse(apActor, position, how);
        return;
    }

    if (apActor->actorState.IsDead())
    {
        // A copy that died here alone lies where it fell.
        // The owner's position is then a living actor's, and the body would be dragged after it (live-corpse-owner-alive).
        if (!aDeadAtOwner)
        {
            if (s_saidDeadAlone.insert(apActor->formID).second)
                spdlog::info("CorpseDiag: {:X} is dead here, but its owner's game has not said it died; left where it lies", apActor->formID);
            return;
        }
        s_saidDeadAlone.erase(apActor->formID);

        // A dead body is moved to where the owner's corpse lies, but only when it is clearly elsewhere.
        // ForcePosition moves only the reference; the ragdoll the corpse is drawn from is moved by PlaceCorpse (live-corpse-place).
        constexpr float cCorpseSnapDistance = 64.f;
        // What is drawn, the ragdoll, goes where the owner has the body (VRBodySync::PlaceCorpse), and once the owner's bones stop it lies in their pose.
        VRBodySync::CorpsePlacement how{};
        how.Authority = AuthorityOf(aInterpolationComponent);
        how.pOwnerPose = &aInterpolationComponent.InterpolatedVRPose;
        how.pRestPose = &aInterpolationComponent.RestPose;
        how.OwnerBonesArrive = OwnerBonesArrive(aInterpolationComponent, aNow);
        how.OwnerBonesMove = OwnerBonesMove(aInterpolationComponent);
        VRBodySync::PlaceCorpse(apActor, position, how);
        const glm::vec3 current{apActor->position.x, apActor->position.y, apActor->position.z};
        const float cApart = glm::distance(current, position);
        if (cApart > cCorpseSnapDistance)
            apActor->ForcePosition(position);
        return;
    }

#ifdef SKYRIMVR
    // SlideDiag: a body that moves while the animation variables driving its legs do not change is sliding.
    // Both numbers are already here, so the check costs almost nothing.
    {
        static uint32_t s_moved = 0;
        static uint32_t s_movedFrozen = 0;
        static uint32_t s_worstActor = 0;
        static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

        // A move is counted by speed, over 50 units a second, so it does not depend on the frame rate.
        const float cStep = glm::distance(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(position));
        if (cDeltaTime > 0.0 && cStep / static_cast<float>(cDeltaTime) > 0.05f)
        {
            ++s_moved;
            // Counted only when the variables are unchanged and all zero.
            // A body running at a steady speed sends the same variables in every snapshot while its legs still move.
            if (first.Variables == second.Variables)
            {
                bool saysSomething = false;
                for (const float value : second.Variables.Floats)
                {
                    if (value != 0.f)
                    {
                        saysSomething = true;
                        break;
                    }
                }
                if (!saysSomething)
                {
                    ++s_movedFrozen;
                    s_worstActor = apActor->formID;
                }
            }
        }

        const auto slideNow = std::chrono::steady_clock::now();
        if (slideNow >= s_nextLog)
        {
            s_nextLog = slideNow + 10s;
            if (s_moved)
                spdlog::info("SlideDiag: {} of {} moves of a remote body came with animation variables that had not changed and said nothing (no speed, no direction); last was {:X}. Those are a body sliding rather than walking.",
                             s_movedFrozen, s_moved, s_worstActor);
            s_moved = 0;
            s_movedFrozen = 0;
            s_worstActor = 0;
        }
    }
#endif

    // A copy that just got up walks on to where its owner has it instead of jumping there.
    // The fastest it moves meanwhile is logged: a jump is far faster than a walk.
    NiPoint3 placed = position;
    if (const auto rising = s_rising.find(apActor->formID); rising != s_rising.end())
    {
        glm::vec3& offset = rising.value().Offset;
        const float cLeft = glm::length(offset);
        const float cClosed = cGetUpCatchUpSpeed * static_cast<float>(cDeltaTime) / 1000.f;
        offset = cLeft > cClosed ? offset * ((cLeft - cClosed) / cLeft) : glm::vec3{};
        placed = NiPoint3(static_cast<glm::vec3>(position) + offset);

        const float cStep = glm::distance(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(placed));
        if (cDeltaTime > 0.0)
            rising.value().Fastest = std::max(rising->second.Fastest, cStep * 1000.f / static_cast<float>(cDeltaTime));
        const auto cUpFor = std::chrono::steady_clock::now() - rising->second.Since;
        if ((cUpFor > 2s && offset == glm::vec3{}) || cUpFor > 5s)
        {
            spdlog::info("KnockDown: NPC copy {:X} moved at most {:.0f} units a second in its first {} ms up", apActor->formID, rising->second.Fastest,
                         std::chrono::duration_cast<std::chrono::milliseconds>(cUpFor).count());
            s_rising.erase(rising);
        }
    }

    // A rider's copy on its horse is held on the horse as drawn this frame, with the seat its owner had it in, so it does not move
    // about the saddle whatever the timing of either's updates. Its climb on and off plays from there, as the game's own does.
    Actor* pHorse = aInterpolationComponent.PinTo;
    if (pHorse && !SeatPin::Holds(static_cast<glm::vec3>(pHorse->position), pHorse->rotation.z))
        pHorse = nullptr;
    // The seat it is held in, as seen from the horse; given again from the horse at the frame end (PosedCopyPlacement).
    glm::vec3 heldSeat{};
    if (pHorse)
    {
        const float cHeading = pHorse->rotation.z;
        const glm::vec3 cHorse = static_cast<glm::vec3>(pHorse->position);
        if (!aInterpolationComponent.SeatKnown && ((apActor->actorState.flags1 >> 14) & 0xF) == MountSync::kSitStateSeated)
        {
            // Where its owner had it against the horse at the same instant (both at the horse's playback tick), once that holds still.
            const glm::vec3 cSample = SeatPin::Capture(static_cast<glm::vec3>(position), cHorse, cHeading);
            if (aInterpolationComponent.SeatedSince < 0.0)
            {
                aInterpolationComponent.SeatedSince = aNow;
                aInterpolationComponent.SeatCandidate = cSample;
                aInterpolationComponent.SeatCandidateSince = aNow;
            }
            else if (glm::distance(cSample, aInterpolationComponent.SeatCandidate) > SeatPin::kSeatSettleUnits)
            {
                aInterpolationComponent.SeatCandidate = cSample;
                aInterpolationComponent.SeatCandidateSince = aNow;
            }
            if (SeatPin::SeatSettled(aNow - aInterpolationComponent.SeatCandidateSince, aNow - aInterpolationComponent.SeatedSince))
            {
                aInterpolationComponent.SeatKnown = true;
                aInterpolationComponent.Seat = aInterpolationComponent.SeatCandidate;
                aInterpolationComponent.SeatSince = aNow;
            }
        }
        const glm::vec3 cSeat = aInterpolationComponent.SeatKnown ? aInterpolationComponent.Seat * SeatPin::SeatWeight(aNow - aInterpolationComponent.SeatSince) : glm::vec3{};
        heldSeat = cSeat;
        placed = NiPoint3(SeatPin::Place(cHorse, cHeading, cSeat));
        aInterpolationComponent.Position = static_cast<glm::vec3>(placed);
    }

    // A VR player's posed copy is placed and turned at the frame end, right before its pose is written (PosedCopyPlacement).
    // This runs on the game's script threads at no fixed point in the frame, and a placement here could rebuild the skeleton after the pose.
    // Not while the copy waits hidden for its first placement: that one shows it, so it lands now.
    // On flat, a copy the engine's own update draws (SEBodyPresentation) is not posed at the frame end, so it is placed now.
    const VRPose& cPresentedPose = aInterpolationComponent.InterpolatedVRPose;
    const bool cPosedPlayerCopy = cPresentedPose.HasData && !cPresentedPose.NoBones && pExtension && pExtension->IsRemotePlayer();
#ifdef SKYRIMVR
    const bool cPresentedByEngine = false;
#else
    const bool cPresentedByEngine = SEBodyPresentation::Presents(apActor->formID);
#endif
    const bool cPlaceAtFrameEnd = cPosedPlayerCopy && !cPresentedByEngine && !SpawnReveal::IsWaiting(apActor->formID) && PosedCopyPlacement::IsFrameEndRunning();
    const glm::vec3 cPrevious = static_cast<glm::vec3>(apActor->position);
    if (!cPlaceAtFrameEnd)
        apActor->ForcePosition(placed);
    // Where the copy stands for this step, also when its placement waits for the frame end.
    const glm::vec3 cPlacedNow = cPlaceAtFrameEnd ? static_cast<glm::vec3>(placed) : static_cast<glm::vec3>(apActor->position);
    SpawnReveal::Placed(apActor);
    // A creature of another kind than the owner's (see MarkForeignGraph) keeps its own animation state.
    if (!aInterpolationComponent.ForeignGraph)
    {
        if (pExtension && pExtension->IsRemotePlayer())
            LandCopy(
                apActor, second.Variables,
                pHorse || cOnMount || apActor->actorState.IsSwimming() || apActor->actorState.IsDeadOrDying() || apActor->actorState.IsBleedingOut() ||
                    apActor->actorState.IsKnockedDown() || ((apActor->actorState.flags1 >> 14) & 0xF) != 0 || pExtension->KneelsInGraph.load(std::memory_order_relaxed));
        const double cQuietFor = cRenderTick - static_cast<double>(movements.back().Tick);
        const bool cStopped = s_quietStopped.find(apActor->formID) != s_quietStopped.end();
        // Read only once quiet: it asks the graph for a variable.
        const float cLegSpeed = cQuietFor > cQuietStopAfter ? LegSpeed(apActor) : 0.f;
        if (cQuietFor > cQuietStopAfter && IsOnFoot(apActor) && (cStopped || cLegSpeed > 1.f))
        {
            StopQuietCopy(apActor, cQuietFor);
            aInterpolationComponent.StrideDirection = -1.f;
        }
        else
        {
            // TEMPORARY: a copy still moving by some measure but not stopped, and why, once per quiet spell.
            if (cQuietFor <= cQuietStopAfter)
                s_quietNotStopped.erase(apActor->formID);
            else if (!cStopped && cLegSpeed > 1.f && s_quietNotStopped.insert(apActor->formID).second)
            {
                static BSFixedString s_speed("Speed");
                float graphSpeed = 0.f;
                apActor->animationGraphHolder.GetVariableFloat(&s_speed, &graphSpeed);
                spdlog::info("QuietStop: copy {:X} had no report for {:.1f} s and moves, but was not stopped (on foot {}, speed {:.0f}, graph speed {:.0f}, flags1 {:#x})",
                             apActor->formID, cQuietFor / 1000.0, IsOnFoot(apActor), apActor->GetSpeed(), graphSpeed, apActor->actorState.flags1);
            }

            // Variables from before the last action played would undo what it set, until a newer point comes.
            // Movement can be lost, so the point after an action may never come.
            // Between two points both after it, the legs' floats go from one to the other as the body does, instead of
            // stepping to the newer point's at each point.
            if (second.Tick >= aLastActionTick)
            {
                const auto* pKinds = !cOnePoint && first.Tick >= aLastActionTick && !isExtrapolating && !cIsTeleport && delta < 1.f ? FloatBlends(apActor) : nullptr;
                static AnimationVariables s_blended;
                if (pKinds)
                    RemoteSmoothing::BlendVariables(first.Variables, second.Variables, delta, *pKinds, s_blended);
                else
                    s_blended = second.Variables;
                const bool cVRBody = aInterpolationComponent.PosePolicy == RemotePose::Policy::LiveVRPlayer && second.VRPoseData.HasData && !second.VRPoseData.NoBones;
                MatchStride(apActor, aInterpolationComponent, cPrevious, cPlacedNow, cIsTeleport || cFirstPlacement || pHorse, cDeltaTime, aNow, cVRBody, s_blended);
                NoteSneak(apActor, aInterpolationComponent, s_blended);
                MatchCrouch(apActor, aInterpolationComponent, second.VRPoseData, cVRBody, aNow, s_blended);
                apActor->LoadAnimationVariables(s_blended);
            }
            if (cStopped && cQuietFor <= cQuietStopAfter)
                WatchQuietCopy(apActor);
        }
    }

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        apActor->currentProcess->middleProcess->direction =
            aInterpolationComponent.StrideDirection >= 0.f ? StrideMatch::DirectionRadians(aInterpolationComponent.StrideDirection) : second.Direction;
    }

    auto rotA = first.Rotation;
    auto rotB = second.Rotation;

    const auto deltaX = TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta;
    const auto deltaY = TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta;
    const auto deltaZ = TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta;

    auto finalX = TiltedPhoques::Mod(rotA.x + deltaX, float(TiltedPhoques::Pi * 2));
    if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
        finalX -= TiltedPhoques::Pi * 2;

    const auto finalY = TiltedPhoques::Mod(rotA.y + deltaY, float(TiltedPhoques::Pi * 2));
    const auto finalZ = TiltedPhoques::Mod(rotA.z + deltaZ, float(TiltedPhoques::Pi * 2));

    float heading = finalZ;
    bool horseHeading = false;
    if (pHorse)
    {
        const bool cSeated = ((apActor->actorState.flags1 >> 14) & 0xF) == MountSync::kSitStateSeated;
        const bool cSampled = cOnMount && cRenderTick >= static_cast<double>(first.Tick) && cRenderTick <= static_cast<double>(second.Tick);
        heading = SeatPin::RiderHeading(pHorse->rotation.z, finalZ, cSeated, cSampled);
        horseHeading = !(cSeated && cSampled && std::isfinite(finalZ));
    }
    // A posed rider is held on its horse as the horse stands at the frame end: the horse can be placed after this step.
    if (cPlaceAtFrameEnd && pHorse)
        PosedCopyPlacement::KeepOnHorseForFrameEnd(apActor->formID, placed, NiPoint3(glm::vec3(finalX, finalY, heading)), pHorse->formID, NiPoint3(heldSeat), horseHeading);
    else if (cPlaceAtFrameEnd)
        PosedCopyPlacement::KeepForFrameEnd(apActor->formID, placed, NiPoint3(glm::vec3(finalX, finalY, heading)));
    else
        apActor->SetRotation(finalX, finalY, heading);
}

void InterpolationSystem::AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint, const double aArrivalTime) noexcept
{
    if (RemotePose::RaisesBodyDelay(acPoint.VRPoseData.HasData, acPoint.VRPoseData.NoBones, aInterpolationComponent.PosePolicy))
        aInterpolationComponent.Clock.SetMinTargetDelay(cVRBodyMinDelay);

    // A duplicate carries nothing new.
    if (!aInterpolationComponent.Clock.AddSample(aArrivalTime, acPoint.Tick))
        return;

    // Interpolating between two points too far apart for any actor to travel would drag (or launch) the actor across.
    // The buffer keeps no worldspace, so points either side of a worldspace change can be hundreds of cells apart.
    // Points are about 0.1 s apart, so a step over a cell (4096 units) drops the buffer and the actor arrives at the new place.
    if (!aInterpolationComponent.TimePoints.empty())
    {
        constexpr float cImpossibleStep = 4096.f;
        const glm::vec3& previous = aInterpolationComponent.TimePoints.back().Position;
        const float jump = glm::distance(previous, static_cast<glm::vec3>(acPoint.Position));
        if (jump > cImpossibleStep)
        {
            // Movement arrives unreliably: a late point from before the jump would send the actor back to where it left.
            if (acPoint.Tick < aInterpolationComponent.TimePoints.back().Tick)
                return;

            aInterpolationComponent.TimePoints.clear();
            aInterpolationComponent.Position = acPoint.Position;

            // Logged because the worldspace-change cause is a hypothesis; the log shows how often, how far and from where.
            static std::chrono::steady_clock::time_point s_nextLog;
            static uint32_t s_since = 0;
            ++s_since;
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextLog)
            {
                s_nextLog = now + std::chrono::seconds(10);
                spdlog::debug(
                    "JumpDiag: a buffered point was {:.0f} units from the one before it, at ({:.0f}); dropped the buffer so the actor arrives instead of flying. {} since the last "
                    "line.",
                    jump, acPoint.Position, s_since);
                s_since = 0;
            }
        }
    }

    auto prepared = acPoint;
    const auto accepted = aInterpolationComponent.PoseState.Accept(acPoint.Tick, aArrivalTime, acPoint.VRPoseData, aInterpolationComponent.PosePolicy);
    prepared.VRPoseData = accepted.Pose;
    prepared.PoseSource = accepted.Source;

    auto itor = std::begin(aInterpolationComponent.TimePoints);
    const auto end = std::cend(aInterpolationComponent.TimePoints);

    bool inserted = false;
    while (itor != end)
    {
        if (itor->Tick > acPoint.Tick)
        {
            aInterpolationComponent.TimePoints.insert(itor, prepared);
            inserted = true;
            break;
        }

        ++itor;
    }

    if (!inserted)
        aInterpolationComponent.TimePoints.push_back(prepared);
    aInterpolationComponent.HasNewTimePoint = true;

    // Normally Update consumes old points each frame.
    // If the playback clock stalls or the sender's clock runs ahead, the buffer would grow without bound, so keep the newest few seconds.
    // Update can hold the oldest retained point until the clock catches up.
    constexpr size_t cMaxBufferedPoints = 64;
    while (aInterpolationComponent.TimePoints.size() > cMaxBufferedPoints)
        aInterpolationComponent.TimePoints.pop_front();
}

InterpolationComponent& InterpolationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    Clean(aWorld, aEntity);
    auto& component = aWorld.emplace<InterpolationComponent>(aEntity);
    BindPoseIdentity(aWorld, aEntity);
    return component;
}

void InterpolationSystem::BindPoseIdentity(World& aWorld, const entt::entity aEntity) noexcept
{
    auto* component = aWorld.try_get<InterpolationComponent>(aEntity);
    const auto* remote = aWorld.try_get<RemoteComponent>(aEntity);
    if (!component || !remote)
        return;
    const RemotePose::Identity identity{s_poseSession, remote->OwnershipEpoch, (static_cast<uint64_t>(remote->Id) << 32) | remote->CachedRefId};
    const auto policy = aWorld.all_of<PlayerComponent>(aEntity) && !remote->DeadAtOwner ? RemotePose::Policy::LiveVRPlayer : RemotePose::Policy::SparseBody;
    const auto previousIdentity = component->PoseState.GetIdentity();
    const auto previousGeneration = component->PoseState.Generation();
    const bool changedIdentity = component->PoseState.Bind(identity);
    const bool changedPolicy = component->PosePolicy != policy;
    if (changedIdentity || changedPolicy)
    {
        if (!changedIdentity)
            component->PoseState.Reset();
        Forget(remote->CachedRefId);
        const auto previousFormId = static_cast<uint32_t>(previousIdentity.Entity);
        if (changedIdentity && previousFormId && previousFormId != remote->CachedRefId)
        {
#ifdef SKYRIMVR
            VRBodySync::ClearRemotePoseForGeneration(previousFormId, previousGeneration);
#else
            SEBodyPresentation::ForgetForGeneration(previousFormId, previousGeneration);
#endif
        }
        component->TimePoints.clear();
        component->Clock = {};
        component->PositionError = {};
        component->IsExtrapolating = false;
        component->HasNewTimePoint = false;
        component->InterpolatedVRPose = {};
        component->RestPose = {};
        component->PresentationPose = {};
        // Position stays where the copy was shown: the next owner's first placement glides from there.
        component->Placed = false;
        component->HandOffGap = {};
        component->HandOffShown = {};
        component->Pinned = false;
        component->SeatKnown = false;
        component->SeatedSince = -1.0;
        component->GroundGlide = false;
        component->Correction = {};
        // MountService marks it again while it is seated.
        component->SeatedOn = entt::null;
        component->PoseBlend = {};
    }
    component->PosePolicy = policy;
}

void InterpolationSystem::ResetPoseSession(World& aWorld) noexcept
{
    ++s_poseSession;
    // Nothing decided for a copy of the old session is made in the new one.
    ForgetCopyBodies();
    VRBodySync::ResetLocalSpellAim();
#ifndef SKYRIMVR
    SEBodyPresentation::Clear();
#endif
    for (const auto entity : aWorld.view<RemoteComponent, InterpolationComponent>())
        BindPoseIdentity(aWorld, entity);
}

void InterpolationSystem::TickPresentation() noexcept
{
#ifndef SKYRIMVR
    SEBodyPresentation::Tick();
#endif
}

void InterpolationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<InterpolationComponent>(aEntity))
    {
        if (const auto* remote = aWorld.try_get<RemoteComponent>(aEntity))
            Forget(remote->CachedRefId);
        aWorld.remove<InterpolationComponent>(aEntity);
    }
}

void InterpolationSystem::NoteReplayedFall(World& aWorld, Actor* apActor, const bool aEssentialDown, const uint64_t aNativeLifeAtAction, const uint64_t aActionTick) noexcept
{
#ifndef SKYRIMVR
    if (!apActor || apActor->actorState.IsDeadOrDying())
        return;
    const auto* extension = apActor->GetExtension();
    if (!extension || !extension->IsRemote() || extension->IsRemotePlayer() || !apActor->GetNiNode() ||
        extension->NativeLifeGeneration.load(std::memory_order_relaxed) != aNativeLifeAtAction)
        return;
    const auto view = aWorld.view<RemoteComponent, InterpolationComponent>();
    for (const auto entity : view)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        if (remote.CachedRefId != apActor->formID || !CopyBodyRules::MayObserveFall(true, true, apActor->actorState.IsDeadOrDying(), remote.DeadAtOwner, true))
            continue;
        const auto authority = AuthorityOf(view.get<InterpolationComponent>(entity));
        if (!authority.Valid())
            return;
        // A second accepted fall cannot discard observations of the body's first fall.
        if (const auto previous = s_replayedFalls.find(apActor->formID); previous != s_replayedFalls.end() && previous->second.Authority == authority &&
                                                                         previous->second.ActorAtArm == apActor && previous->second.RootAtArm == apActor->GetNiNode() &&
                                                                         previous->second.NativeLifeAtArm == aNativeLifeAtAction && previous->second.Observation.Down)
        {
            if (aActionTick > previous->second.ActionTickAtArm)
            {
                previous.value().ActionTickAtArm = aActionTick;
                previous.value().Recovery = {};
            }
            if (!aEssentialDown && previous->second.EssentialDown && !previous->second.KnockRequested)
            {
                previous.value().KnockRequested = true;
                previous.value().KnockRequestedAt = std::chrono::steady_clock::now();
                previous.value().KnockObservation = {};
                previous.value().ActionTickAtArm = aActionTick;
                previous.value().Recovery = {};
            }
            return;
        }
        s_replayedFalls[apActor->formID] = ReplayedFall{
            authority, apActor, apActor->GetNiNode(), extension->RevivedSinceDeath, aNativeLifeAtAction, aActionTick, aEssentialDown, false, std::chrono::steady_clock::now()};
        return;
    }
#else
    (void)aWorld;
    (void)apActor;
    (void)aEssentialDown;
    (void)aNativeLifeAtAction;
    (void)aActionTick;
#endif
}

void InterpolationSystem::NoteOwnerGetUp(World& aWorld, Actor* apActor, const uint64_t aActionTick) noexcept
{
#ifndef SKYRIMVR
    if (auto* fall = BoundObservedFall(aWorld, apActor); fall && aActionTick > fall->ActionTickAtArm)
    {
        fall->Recovery.GetUpTick = aActionTick;
        spdlog::debug("KnockOwnerSignal: copy {:X} received its owner's GetUpBegin tick {} after fall tick {}", apActor->formID, aActionTick, fall->ActionTickAtArm);
    }
#else
    (void)aWorld;
    (void)apActor;
    (void)aActionTick;
#endif
}

bool InterpolationSystem::AdmitPlayerRagdoll(World& aWorld, Actor* apActor, const bool aHistory, const uint32_t aOwnerFlags1) noexcept
{
    if (!apActor)
        return false;
    const auto it = s_playerKnocks.find(apActor->formID);
    if (it == s_playerKnocks.end())
        return false;
    const auto entity = Utils::FindByFormId(aWorld.view<FormIdComponent>(entt::exclude<ObjectComponent>), apActor->formID);
    const auto* pInterpolation = entity ? aWorld.try_get<InterpolationComponent>(*entity) : nullptr;
    const bool cPinned = pInterpolation && pInterpolation->PinTo;
    const bool cMayFall = PlayerKnockMirror::MayFall(apActor->actorState.IsDead(), apActor->actorState.IsDying() || apActor->actorState.IsBleedingOut(), OnFoot(apActor), cPinned);
    const uint32_t cKnockHere = PlayerKnockMirror::KnockState(apActor->actorState.flags1);
    return it.value().AdmitRagdoll(PlayerKnockMirror::WordOf(true, false, aOwnerFlags1), aHistory, cKnockHere, cMayFall, std::chrono::steady_clock::now());
}

void InterpolationSystem::NotePlayerOwnerAction(const uint32_t aFormId, const char* acpEventName, const uint32_t aOwnerFlags1) noexcept
{
    if (!aFormId)
        return;
    const bool cRagdoll = acpEventName && _stricmp(acpEventName, "Ragdoll") == 0;
    const bool cGetUp = acpEventName && _stricmp(acpEventName, "GetUpBegin") == 0;
    auto& mirror = s_playerKnocks[aFormId];
    const auto cBefore = mirror.GetPhase();
    mirror.OnOwnerWord(PlayerKnockMirror::WordOf(cRagdoll, cGetUp, aOwnerFlags1), std::chrono::steady_clock::now());
    if (mirror.GetPhase() != cBefore)
        spdlog::debug(
            "KnockDown: player copy {:X}: its owner {} ('{}', owner knock state {})", aFormId,
            mirror.GetPhase() == PlayerKnockMirror::Mirror::Phase::Down ? "went down" : "gets up", acpEventName ? acpEventName : "", PlayerKnockMirror::KnockState(aOwnerFlags1));
}

void InterpolationSystem::NoteCopyJumpEvent(const uint32_t aFormId, const char* acpEventName, const bool aTaken) noexcept
{
    if (!aFormId || !acpEventName || !JumpSync::IsJumpEvent(acpEventName))
        return;
    std::scoped_lock lock(s_landingsLock);
    if (s_landings.size() > 256)
        s_landings.clear();
    s_landings[aFormId].OnCopyEvent(acpEventName, aTaken, std::chrono::steady_clock::now());
}

void InterpolationSystem::NotePlayerOwnerStop(const uint32_t aFormId, const uint64_t aActionTick) noexcept
{
    if (!aFormId)
        return;
    if (s_playerOwnerStops.size() > 256)
        s_playerOwnerStops.clear();
    uint64_t& tick = s_playerOwnerStops[aFormId];
    tick = std::max(tick, aActionTick);
}

void InterpolationSystem::NoteOwnerAlive(World& aWorld, Actor* apActor) noexcept
{
#ifndef SKYRIMVR
    if (auto* fall = BoundObservedFall(aWorld, apActor, true))
    {
        fall->Recovery.Alive();
        spdlog::debug("KnockOwnerSignal: copy {:X} received an already-alive same-life owner word after fall tick {}", apActor->formID, fall->ActionTickAtArm);
    }
#else
    (void)aWorld;
    (void)apActor;
#endif
}

void InterpolationSystem::NoteOwnerHealth(World& aWorld, Actor* apActor, const float aHealth, const bool aAboveCopyFloor) noexcept
{
#ifndef SKYRIMVR
    if (auto* fall = BoundObservedFall(aWorld, apActor))
    {
        const bool wasHealthy = fall->Recovery.HealthyAfterAlive;
        fall->Recovery.Health(std::isfinite(aHealth) && aAboveCopyFloor);
        if (fall->Recovery.HealthyAfterAlive && !wasHealthy)
            spdlog::debug(
                "KnockOwnerSignal: copy {:X} received healthy owner value {:.0f} after its same-life alive word (fall tick {})", apActor->formID, aHealth, fall->ActionTickAtArm);
    }
#else
    (void)aWorld;
    (void)apActor;
    (void)aHealth;
    (void)aAboveCopyFloor;
#endif
}

void InterpolationSystem::ForgetCopyBodies() noexcept
{
#ifndef SKYRIMVR
    for (const auto& entry : s_replayedFalls)
        if (auto* actor = Cast<Actor>(TESForm::GetById(entry.first)); actor && actor == entry.second.ActorAtArm)
            if (auto* extension = actor->GetExtension())
                extension->ObservedCopyFall.store(false, std::memory_order_relaxed);
    s_replayedFalls.clear();
#endif
    s_down.clear();
    s_rising.clear();
    for (const auto& entry : s_playerKnocks)
        if (auto* actor = Cast<Actor>(TESForm::GetById(entry.first)))
            if (auto* extension = actor->GetExtension())
                extension->OwnerKnockRuns.store(false, std::memory_order_relaxed);
    s_playerKnocks.clear();
    for (const auto& entry : s_graphKneels)
        if (auto* actor = Cast<Actor>(TESForm::GetById(entry.first)))
            if (auto* extension = actor->GetExtension())
                extension->ForgetCopyBleedout();
    s_graphKneels.clear();
    s_playerOwnerStops.clear();
    {
        std::scoped_lock lock(s_landingsLock);
        s_landings.clear();
    }
    s_bleedingOut.clear();
    s_stoodUp.clear();
    VRBodySync::ForgetAllCorpses();
}

void InterpolationSystem::Forget(const uint32_t aFormId) noexcept
{
    if (!aFormId)
        return;
    if (auto* actor = Cast<Actor>(TESForm::GetById(aFormId)))
        if (auto* extension = actor->GetExtension())
        {
#ifndef SKYRIMVR
            extension->ObservedCopyFall.store(false, std::memory_order_relaxed);
#endif
            // Its kneel, its owner's word and its stop were of the copy before: a new owner's would get up or kneel on their account.
            extension->ForgetCopyBleedout();
            extension->OwnerKnockRuns.store(false, std::memory_order_relaxed);
        }
#ifndef SKYRIMVR
    s_replayedFalls.erase(aFormId);
#endif
    if (s_quietStopped.erase(aFormId))
        spdlog::info("QuietStop: copy {:X} forgotten while stopped (removed, or run by this game now)", aFormId);
    s_quietNotStopped.erase(aFormId);
    s_crouching.erase(aFormId);
    s_playerOwnerStops.erase(aFormId);
    {
        std::scoped_lock lock(s_landingsLock);
        s_landings.erase(aFormId);
    }
    // A new copy under the same id would otherwise get up, slide or stay put on the old one's account.
    s_down.erase(aFormId);
    s_rising.erase(aFormId);
    s_playerKnocks.erase(aFormId);
    s_bleedingOut.erase(aFormId);
    s_graphKneels.erase(aFormId);
    s_saidDeadAlone.erase(aFormId);
    // What PlaceCorpse knew of it, and the moves it queued for it, on both builds.
    VRBodySync::ForgetCorpse(aFormId);
#ifdef SKYRIMVR
    VRBodySync::ClearRemotePose(aFormId);
    s_poseSpikes.erase(aFormId);
#else
    SEBodyPresentation::Forget(aFormId);
#endif
}
