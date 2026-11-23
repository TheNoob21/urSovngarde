#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Riekling.h>

AnimationGraphDescriptor_Riekling::AnimationGraphDescriptor_Riekling(AnimationGraphDescriptorManager& aManager)
{
    enum Variables
    {
        kSpeedSampled = 0,
        kDirection = 1,
        kdefaultBlend = 2,
        kiSyncIdleLocomotion = 3,
        kiSyncRunWalk = 4,
        kiRightHandType = 5,
        kiSyncDefaultState = 6,
        kIsAttackReady = 7,
        kbEquipOK = 8,
        kbAnimationDriven = 9,
        kstaggerMagnitude = 10,
        kstaggerDirection = 11,
        kiGetUpType = 12,
        kiState = 13,
        kSpeed = 14,
        kiState_RieklingDefault = 15,
        kTurnDelta = 16,
        kiSyncTurnState = 17,
        kIsFiringBow = 18,
        kFootIKEnable = 19,
        km_onOffGain = 20,
        km_groundAscendingGain = 21,
        km_groundDescendingGain = 22,
        km_footPlantedGain = 23,
        km_footRaisedGain = 24,
        km_footUnlockGain = 25,
        km_worldFromModeFeedbackGain = 26,
        km_errorUpDownBias = 27,
        km_alignWorldFromModelGain = 28,
        km_hipOrientationGain = 29,
        kbHeadTrackingOn = 30,
        kcamerafromx = 31,
        kcamerafromy = 32,
        kcamerafromz = 33,
        kiLeftHandType = 34,
        kLipAA = 35,
        kLipDST = 36,
        kLipEE = 37,
        kLipFF = 38,
        kLipK = 39,
        kLipL = 40,
        kLipR = 41,
        kLipTH = 42,
        kLipAADamped = 43,
        kLipDSTDamped = 44,
        kLipEEDamped = 45,
        kLipFFDamped = 46,
        kLipKDamped = 47,
        kLipLDamped = 48,
        kLipRDamped = 49,
        kLipTHDamped = 50,
        kLipGain = 51,
        kbIsSynced = 52,
        kbAllowRotation = 53,
        kTargetLocation = 54,
        kbTalkable = 55,
        kSlowBlend = 56,
        kFastBlend = 57,
        kIsBleedingOut = 58,
        kbForceIdleStop = 59,
    };

    uint64_t key = 2401375257788400503;

    AnimationGraphDescriptorManager::Builder s_builder(
        aManager, key,
        AnimationGraphDescriptor(
            {
                kbAnimationDriven,
                kbAllowRotation,
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
