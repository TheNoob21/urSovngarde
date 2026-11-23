#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Wisp.h>

AnimationGraphDescriptor_Wisp::AnimationGraphDescriptor_Wisp(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kiState = 0,
        kiState_WispDefault = 1,
        kbAnimationDriven = 2,
        kbAllowRotation = 3,
        kSpeed = 4,
        kiSyncIdleLocomotion = 5,
        kDefaultBlend = 6,
        kSlowBlend = 7,
        kDirection = 8,
        kTurnDelta = 9,
        kiSyncTurnState = 10,
        kfMinTurnDelta = 11,
        kbHeadTracking = 12,
        kbHeadTrackingOn = 13,
        kbCanHeadTrack = 14,
        kIsStaggering = 15,
        kIsRecoiling = 16,
        kTargetLocation = 17,
        kcamerafromx = 18,
        kcamerafromy = 19,
        kcamerafromz = 20,
        kbEquipOK = 21,
        kIsAttackReady = 22,
        kIsAttacking = 23,
        kiSyncDefaultState = 24,
        kiLeftHandType = 25,
        kbMLh_Ready = 26,
        kbWantCastLeft = 27,
        kIsCasting = 28,
        kiRightHandType = 29,
        kbMRh_Ready = 30,
        kbWantCastRight = 31,
        kstaggerMagnitude = 32,
        kIsBlocking = 33,
        kiWantBlock = 34,
        kIsBashing = 35,
        kiGetUpType = 36,
        kstaggerDirection = 37,
    };

    uint64_t key = 4699000156716604007;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
                kbHeadTracking,
                kIsRecoiling,
                kIsStaggering,
                kIsAttacking,
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
                kiSyncDefaultState,
            }));
}
