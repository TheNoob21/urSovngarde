#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Netch.h>

AnimationGraphDescriptor_Netch::AnimationGraphDescriptor_Netch(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kbAllowRotation = 0,
        kDirection = 1,
        kSpeed = 2,
        kSpeedSampled = 3,
        kiSyncSprintState = 4,
        kiSyncIdleLocomotion = 5,
        kiState = 6,
        kiState_NetchDefault = 7,
        kTurnDelta = 8,
        kDirectionDamped = 9,
        kSpeedDamped = 10,
        kiSyncTurnState = 11,
        kIsInCombat = 12,
        kiSyncDefaultState = 13,
        kiState_NetchSprinting = 14,
        kstaggerMagnitude = 15,
        kbAnimationDriven = 16,
        kiGetUpType = 17,
        kIsAttackReady = 18,
        kbEquipOK = 19,
        kIsAttacking = 20,
        kIsRecoiling = 21,
    };

    uint64_t key = 17787828040730466272;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
                kIsRecoiling,
                kIsAttacking,
                kIsAttackReady,
            },
            {
                kSpeed,
                kTurnDelta,
                kDirection,
                kSpeedSampled,
            },
            {
                kiSyncIdleLocomotion,
                kiSyncTurnState,
                kiSyncDefaultState,
            }));
}
