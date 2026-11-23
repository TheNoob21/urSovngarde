#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>
#include <Structs/VRPose.h>
#include <Systems/PlaybackClock.h>
#include <Systems/RemotePoseInterpolation.h>
#include <Systems/RemotePoseState.h>
#include <Systems/StrideMatch.h>
#include <Systems/CrouchStance.h>

struct Actor;

struct InterpolationComponent
{
    struct TimePoint
    {
        uint64_t Tick{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        AnimationVariables Variables{};
        float Direction{};
        VRPose VRPoseData{};
        RemotePose::Provenance PoseSource{};

        TimePoint() = default;
        TimePoint(const TimePoint&) = default;
        TimePoint& operator=(const TimePoint&) = default;
    };

    List<TimePoint> TimePoints;
    // Which of the owner's ticks to show now, from when this actor's updates arrive (see PlaybackClock).
    PlaybackClock Clock;
    glm::vec3 Position;
    // Left over from extrapolating past the newest point when the late one arrived; it fades out instead of jumping.
    glm::vec3 PositionError{};
    bool IsExtrapolating{false};
    bool HasNewTimePoint{false};
    // Latest interpolated VR pose, applied to the remote actor by VRBodySync.
    VRPose InterpolatedVRPose;
    // A dead body's resting pose its owner sent before this copy could lie in it, as one sent with its spawn: held until its ragdoll
    // is here, then settled into once (VRBodySync::PlaceCorpse, CopyBodyRules::OnHeldRestPose). HasData while held.
    VRPose RestPose;
    RemotePose::State PoseState;
    RemotePose::Sample PresentationPose;
    RemotePose::Policy PosePolicy{RemotePose::Policy::SparseBody};
    // See RemoteAnimationComponent::ForeignGraph: the owner's animation variables are not applied either.
    bool ForeignGraph{false};

    // Placed once since it was made or changed hands: the first placement glides from where the copy stood (HandOffGap).
    bool Placed{false};
    glm::vec3 HandOffGap{};
    double HandOffSince{};
    // The part of HandOffGap shown last frame.
    glm::vec3 HandOffShown{};
    // What was added to the target this frame: the late-point and hand-off corrections. A rider on this mount takes it too.
    glm::vec3 Correction{};

    // The mount this copy is seated on (MountService). For the frame, its place is taken at the mount's playback tick, with
    // the mount's correction, so that rider and horse move as one.
    entt::entity SeatedOn{entt::null};
    bool PlaceOnMount{false};
    double PlaceAtTick{};
    glm::vec3 PlaceCorrection{};
    // For the frame: the horse this copy sits on, drawn already this frame. The copy is held on it (SeatPin), not moved by its own
    // updates. Pinned stays set while it is, the seat as seen from the horse once the copy is in the saddle (SeatKnown).
    Actor* PinTo{nullptr};
    bool Pinned{false};
    bool SeatKnown{false};
    glm::vec3 Seat{};
    double SeatSince{};
    // Until the seat is known: the place on the horse its owner's rider holds now, since when, and since when the copy sits.
    glm::vec3 SeatCandidate{};
    double SeatCandidateSince{};
    double SeatedSince{-1.0};
    // Let off its horse: the glide to its owner's place never goes below that place (SeatPin::AboveGround).
    bool GroundGlide{false};

    // A VR body carried on past its newest pose is blended back from the last pose shown when a late one arrives.
    RemotePoseInterpolation::BlendBack PoseBlend{};

    // A remote player's speed as it is shown moving, eased (StrideMatch); negative until known. Its legs are given it.
    float StrideSpeed{-1.f};
    // A VR player's way across the ground against its body's facing, eased (StrideMatch::StepDirection); negative while it
    // stands or is not one. Its legs are given it, so a step to the side or back in their room is not shown as a walk forward.
    float StrideDirection{-1.f};
    // Since when its legs and its owner's graph speed have disagreed, so that a lasting disagreement is said once.
    StrideMatch::MismatchWatch StrideMismatch{};
    StrideMatch::SprintWatch Sprint{};
    // A VR player's real crouch, shown as its sneak stance while their legs are not sent (CrouchStance).
    CrouchStance::Watch Crouch{};
};
