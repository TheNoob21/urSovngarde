#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_IceWraith.h>

AnimationGraphDescriptor_IceWraith::AnimationGraphDescriptor_IceWraith(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kDefaultBlend = 0,
        kbAnimationDriven = 1,
        kFastBlend = 2,
        kIsAttackReady = 3,
        kbEquipOK = 4,
        kiSyncIdleLocomotion = 5,
        kiSyncDefaultState = 6,
        kTurnDelta = 7,
        kiSyncTurnState = 8,
        kDirection = 9,
        kstaggerMagnitude = 10,
        kTurnDamped = 11,
        kbAllowRotation = 12,
        kTimeDelta = 13,
        kiState = 14,
        kiState_IceWraithDefault = 15,
        kstaggerDirection = 16,
        kiGetUpType = 17,
    };

    uint64_t key = 5077838081812273209;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
                kIsAttackReady,
            },
            {
                kTurnDelta,
                kDirection,
            },
            {
                kiSyncIdleLocomotion,
                kiSyncTurnState,
                kiSyncDefaultState,
            }));
}
