#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Slaughterfish.h>

AnimationGraphDescriptor_Slaughterfish::AnimationGraphDescriptor_Slaughterfish(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kiCombatStance = 0,
        kiSyncIdleLocomotion = 1,
        kInt32Variable00 = 2,
        kIsAttackReady = 3,
        kiSyncTurnDirection = 4,
        kbAnimationDriven = 5,
        kblendFast = 6,
        kTurnDelta = 7,
        kfMinTurnDelta = 8,
        kSpeed = 9,
        kDirection = 10,
        kIsStaggering = 11,
        kIsRecoiling = 12,
        kbEquipOK = 13,
        kiState_SlaughterfishSwim = 14,
        kiGetUpType = 15,
        kblendDefault = 16,
        kiCurrentStateID = 17,
        kAggroWarningBlend = 18,
        kbAllowRotation = 19,
        kiState = 20,
        kSpeedSampled = 21,
        kInt32Variable01 = 22,
    };

    uint64_t key = 16841759923128033557;

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
                kSpeed,
                kTurnDelta,
                kDirection,
                kSpeedSampled,
            },
            {
                kiSyncIdleLocomotion,
                kiCombatStance,
                kiCurrentStateID,
            }));
}
