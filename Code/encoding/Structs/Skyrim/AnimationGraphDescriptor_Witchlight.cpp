#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Witchlight.h>

AnimationGraphDescriptor_Witchlight::AnimationGraphDescriptor_Witchlight(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kDefaultBlend = 0,
        kiState = 1,
        kiState_WitchlightDefault = 2,
        kbEquipOK = 3,
        kIsAttackReady = 4,
        kIsAttacking = 5,
        kiSyncDefaultState = 6,
        kIsStaggering = 7,
        kIsRecoiling = 8,
        kSpeed = 9,
        kiSyncIdleLocomotion = 10,
        kDirection = 11,
        kSlowBlend = 12,
        kbAnimationDriven = 13,
        krunRate = 14,
        krunSpeedMult = 15,
        kstaggerMagnitude = 16,
        kstaggerDirection = 17,
    };

    uint64_t key = 6076589067610934853;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kIsRecoiling,
                kIsStaggering,
                kIsAttacking,
                kIsAttackReady,
            },
            {
                kSpeed,
                kDirection,
            },
            {
                kiSyncIdleLocomotion,
                kiSyncDefaultState,
            }));
}
