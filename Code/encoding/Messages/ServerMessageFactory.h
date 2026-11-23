#pragma once

#include <TiltedCore/Stl.hpp>
#include <Messages/Message.h>
#include <MetaMessage.h>

#include <Messages/AuthenticationResponse.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyHitReaction.h>
#include <Messages/NotifyActivate.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/NotifySpellCast.h>
#include <Messages/NotifyInterruptCast.h>
#include <Messages/NotifyAddTarget.h>
#include <Messages/NotifyRemoveSpell.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/TeleportCommandResponse.h>
#include <Messages/StringCacheUpdate.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/NotifyPlayerHealthUpdate.h>
#include <Messages/NotifySettingsChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/NotifyDroppedItem.h>
#include <Messages/NotifyDroppedItemRemoved.h>
#include <Messages/NotifyDroppedItemMove.h>
#include <Messages/NotifyWorldObjectMove.h>
#include <Messages/NotifyClash.h>
#include <Messages/NotifyVoiceData.h>
#include <Messages/NotifyPlayerWhereabouts.h>
#include <Messages/NotifyArrowNocked.h>
#include <Messages/NotifyDownedState.h>
#include <Messages/NotifyActionEvents.h>
#include <Messages/NotifyHourMove.h>
#include <Messages/NotifyReferenceEnable.h>
#include <Messages/NotifyQuestCatchUp.h>
#include <Messages/NotifyPartyFastTravel.h>
#include <Messages/NotifyTrackedQuests.h>
#include <Messages/NotifyDetectionLevels.h>
#include <Messages/NotifyFollowerCommand.h>
#include <Messages/NotifyCrimeReport.h>
#include <Messages/NotifyStuckArrow.h>
#include <Messages/NotifyDismember.h>
#include <Messages/NotifyWordLearned.h>
#include <Messages/NotifyTrapFire.h>
#include <Messages/NotifyDragonSoul.h>
#include <Messages/NotifyCombatTarget.h>
#include <Messages/NotifyDismount.h>
#include <Messages/NotifyModListsDiffer.h>
#include <Messages/NotifyNpcFurniture.h>
#include <Messages/NotifyNpcHeadTrack.h>
#include <Messages/NotifyRagdollPose.h>
#include <Messages/NotifyPropBodies.h>
#include <Messages/NotifyItemTakeLost.h>
#include <Messages/NotifyMapDiscovery.h>
#include <Messages/NotifyActivationCatchUp.h>
#include <Messages/NotifyReferenceStates.h>
#include <Messages/NotifyArrowPickup.h>
#include <Messages/NotifyItemDataChange.h>
#include <Messages/NotifyMagicState.h>
#include <Messages/NotifyPlayerFurniture.h>
#include <Messages/NotifyServerIdentity.h>
#include <Messages/NotifyExtension.h>
#include <Messages/NotifyPartyPlugin.h>
#include <Messages/NotifyNpcPluginsDiffer.h>
#include <Messages/NotifyPlayerName.h>
#include <Messages/NotifyTimeSkip.h>
#include <Messages/NotifyPartyValue.h>
#include <Messages/NotifyWarState.h>
#include <Messages/NotifyQuestAliases.h>

using TiltedPhoques::UniquePtr;

struct ServerMessageFactory
{
    UniquePtr<ServerMessage> Extract(TiltedPhoques::Buffer::Reader& aReader) const noexcept;

    template <class T> static auto Visit(T&& func)
    {
        auto s_visitor = CreateMessageVisitor<
            AuthenticationResponse, AssignCharacterResponse, ServerReferencesMoveRequest, ServerTimeSettings, CharacterSpawnRequest, NotifyInventoryChanges, StringCacheUpdate,
            NotifyFactionsChanges, NotifyRemoveCharacter, NotifyQuestUpdate, NotifyPlayerList, NotifyPartyInfo, NotifyPartyInvite, NotifyActorValueChanges, NotifyPartyJoined,
            NotifyPartyLeft, NotifyActorMaxValueChanges, NotifyHealthChangeBroadcast, NotifyHitReaction, NotifyActivate, NotifyLockChange, AssignObjectsResponse,
            NotifyDeathStateChange, NotifyOwnershipTransfer, NotifyObjectInventoryChanges, NotifySpellCast, NotifyProjectileLaunch, NotifyInterruptCast, NotifyAddTarget,
            NotifyScriptAnimation, NotifyDrawWeapon, NotifyMount, NotifyNewPackage, NotifyRespawn, NotifySyncExperience, NotifyEquipmentChanges, NotifyChatMessageBroadcast,
            TeleportCommandResponse, NotifyPlayerRespawn, NotifyDialogue, NotifySubtitle, NotifyPlayerDialogue, NotifyActorTeleport, NotifyPlayerLeft, NotifyPlayerJoined,
            NotifyPlayerLevel, NotifyPlayerCellChanged, NotifyTeleport, NotifyPlayerHealthUpdate, NotifySettingsChange, NotifyWeatherChange, NotifySetWaypoint,
            NotifyRemoveWaypoint, NotifySetTimeResult, NotifyRemoveSpell, NotifyDroppedItem, NotifyDroppedItemRemoved, NotifyDroppedItemMove, NotifyWorldObjectMove, NotifyClash,
            NotifyVoiceData, NotifyPlayerWhereabouts, NotifyArrowNocked, NotifyDownedState, NotifyActionEvents, NotifyHourMove, NotifyReferenceEnable, NotifyWarState,
            NotifyPartyPlugin, NotifyPartyValue, NotifyNpcPluginsDiffer, NotifyPlayerName, NotifyTimeSkip, NotifyQuestAliases, NotifyQuestCatchUp, NotifyPartyFastTravel,
            NotifyTrackedQuests, NotifyDetectionLevels, NotifyFollowerCommand, NotifyCrimeReport, NotifyStuckArrow, NotifyDismember, NotifyWordLearned, NotifyTrapFire,
            NotifyDragonSoul, NotifyCombatTarget, NotifyDismount, NotifyModListsDiffer, NotifyNpcFurniture, NotifyNpcHeadTrack, NotifyRagdollPose, NotifyPropBodies,
            NotifyItemTakeLost, NotifyMapDiscovery, NotifyActivationCatchUp, NotifyItemDataChange, NotifyMagicState, NotifyPlayerFurniture, NotifyServerIdentity, NotifyExtension,
            NotifyReferenceStates, NotifyArrowPickup>;

        return s_visitor(std::forward<T>(func));
    }
};
