#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_DwarvenSphere.h>

AnimationGraphDescriptor_DwarvenSphere::AnimationGraphDescriptor_DwarvenSphere(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kIsStaggering = 0,
        kstaggerMagnitude = 1,
        kcurrentDefaultState = 2,
        kiSyncTurnState = 3,
        kTurnDelta = 4,
        kiState = 5,
        kiState_SphereDefault = 6,
        kiSyncIdleLocomotion = 7,
        kDirection = 8,
        kSampledSpeed = 9,
        kbAnimationDriven = 10,
        kIsEquipping = 11,
        kIsUnequipping = 12,
        kiRightHandType = 13,
        kiState_SphereRanged = 14,
        kbWeapReady = 15,
        kIsAttackReady = 16,
        kIsAttacking = 17,
        kIsRecoiling = 18,
        kbIsSynced = 19,
        kSpeed = 20,
        kIsFiringBow = 21,
        kSpeedStartVar = 22,
        kbEquipOK = 23,
        kiLeftHandType = 24,
        kIsBlocking = 25,
        kbAllowRotation = 26,
        kSpeedDamped = 27,
        kTurnDeltaDamped = 28,
        kTurnMin = 29,
        kiRightHandType_8bit = 30,
        kIntDirection = 31,
        kfMinSpeed = 32,
        kbAttached = 33,
        kIsBashing = 34,
        kstaggerDirection = 35,
        kiGetUpType = 36,
    };

    uint64_t key = 17742465010135092947;

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
