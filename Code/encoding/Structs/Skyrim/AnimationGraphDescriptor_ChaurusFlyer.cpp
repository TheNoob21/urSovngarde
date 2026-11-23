#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_ChaurusFlyer.h>

AnimationGraphDescriptor_ChaurusFlyer::AnimationGraphDescriptor_ChaurusFlyer(AnimationGraphDescriptorManager& aManager)
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
        kiState_ChaFlyerDefault = 15,
        kstaggerDirection = 16,
        kiGetUpType = 17,
        kbMLh_Ready = 18,
        kbWantCastLeft = 19,
        kiCombatStance = 20,
        kIsStaggering = 21,
        kIsRecoiling = 22,
        kbIsSynced = 23,
    };

    uint64_t key = 10814878731587440876;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
                kIsRecoiling,
                kIsStaggering,
                kIsAttackReady,
            },
            {
                kTurnDelta,
                kDirection,
            },
            {
                kiSyncIdleLocomotion,
                kiSyncTurnState,
                kiCombatStance,
                kiSyncDefaultState,
            }));
}
