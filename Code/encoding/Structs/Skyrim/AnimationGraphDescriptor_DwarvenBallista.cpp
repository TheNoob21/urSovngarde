#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_DwarvenBallista.h>

AnimationGraphDescriptor_DwarvenBallista::AnimationGraphDescriptor_DwarvenBallista(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kIsStaggering = 0,
        kstaggerMagnitude = 1,
        kcurrentDefaultState = 2,
        kiSyncTurnState = 3,
        kTurnDelta = 4,
        kiState = 5,
        kiState_DwarvenBallistaDefault = 6,
        kiSyncIdleLocomotion = 7,
        kDirection = 8,
        kSampledSpeed = 9,
        kbAnimationDriven = 10,
        kIsEquipping = 11,
        kIsUnequipping = 12,
        kiRightHandType = 13,
        kbWeapReady = 14,
        kIsAttackReady = 15,
        kIsAttacking = 16,
        kIsRecoiling = 17,
        kbIsSynced = 18,
        kSpeed = 19,
        kIsFiringBow = 20,
        kSpeedStartVar = 21,
        kbEquipOK = 22,
        kiLeftHandType = 23,
        kIsBlocking = 24,
        kbAllowRotation = 25,
        kSpeedDamped = 26,
        kTurnDeltaDamped = 27,
        kTurnMin = 28,
        kiRightHandType_8bit = 29,
        kIntDirection = 30,
        kfMinSpeed = 31,
        kbAttached = 32,
        kIsBashing = 33,
        kstaggerDirection = 34,
        kiGetUpType = 35,
        kBlendDefault = 36,
        kIsBleedingOutTransition = 37,
    };

    uint64_t key = 2547439886223928659;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
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
            }));
}
