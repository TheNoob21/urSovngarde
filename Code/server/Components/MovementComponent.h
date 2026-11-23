#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>
#include <Structs/VRPose.h>
#include <ClientTrust.h>

struct MovementComponent
{
    uint64_t Tick{}; // the owner's tick of the newest report taken; 0 before the first and after a hand-over
    glm::vec3 Position;
    glm::vec3 Rotation;
    AnimationVariables Variables;
    float Direction;
    bool Flying{}; // a dragon in the air in its owner's game (Movement::Flying)
    VRPose VRPoseData; // VR players' upper-body pose, relayed as-is to other players
    ClientTrust::HeldJump HeldJump; // a jump beyond the game's speeds, held for the next report
    uint64_t MidRelayTick{};        // the owner's tick of the last report passed on to players a little way off (RelayThinning)
    uint64_t FarRelayTick{};        // and to players far off
};
