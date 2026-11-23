#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Seeker.h>

AnimationGraphDescriptor_Seeker::AnimationGraphDescriptor_Seeker(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kcurrentState = 0,
        kiSyncIdleLocomotion = 1,
        kblendSlow = 2,
        kcurrentDefaultState = 3,
        kiSyncTurnState = 4,
        kTurnDelta = 5,
        kDirection = 6,
        kSampledSpeed = 7,
        kiState = 8,
        kiState_HMDaedraDefault = 9,
        kbAllowRotation = 10,
        kbAnimationDriven = 11,
        kSpeed = 12,
        kiGetUpType = 13,
        kIsStaggering = 14,
        kstaggerMagnitude = 15,
        kstaggerDirection = 16,
        kIsEquipping = 17,
        kiRightHandType = 18,
        kbMLh_Ready = 19,
        kbMRh_Ready = 20,
        kIsCasting = 21,
        kIsAttackReady = 22,
        kbWantCastRight = 23,
        kbWantCastLeft = 24,
        kiLeftHandType = 25,
        kbEquipOK = 26,
        kbWeapReady = 27,
        kiCombatStance = 28,
        kNotCasting = 29,
        ktest = 30,
    };

    uint64_t key = 2345404321706051729;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
                kIsStaggering,
                kIsAttackReady,
            },
            {
                kSpeed,
                kTurnDelta,
                kDirection,
            },
            {
                kiSyncIdleLocomotion,
                kiSyncTurnState,
                kiCombatStance,
            }));
}
