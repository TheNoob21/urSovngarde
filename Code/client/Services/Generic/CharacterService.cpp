#include <Services/GameExtension.h>
#include "Forms/TESObjectCELL.h"
#include <Helpers/Throttle.h>
#include <Helpers/PerfScope.h>
#include <Copies/CopyRegistry.h>
#include <Copies/Takeover.h>
#include <Copies/PlayerLook.h>
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>
#include <Services/MagicService.h>

#include <Services/CharacterService.h>
#include <Services/ObjectService.h>
#include <SharedHomes.h>
#include <ClientTrust.h>
#include <Services/MountService.h>
#include <Services/CombatService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>
#include <Services/InventoryService.h>
#include <Services/Generic/NpcEquipmentCheck.h>
#include <Services/DiscoveryService.h>
#include <Copies/RecentDeletes.h>
#include <Copies/RecentModelChanges.h>
#include <Copies/SceneLines.h>
#include <Copies/FrameStartSwitchOff.h>
#include <CrashHandler.h>
#include <unordered_set>
#include <Copies/CopyRemovalPolicy.h>
#include <Copies/CopyLifetime.h>
#include <PlayerCopyRules.h>
#include <CopyWork.h>
#include <BaseKind.h>
#include <Systems/PendingVoice.h>
#include <Systems/RaiseClaim.h>
#include <Forms/TESRace.h>
#include <GhostDeathPolicy.h>
#include <TimeManager.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/TESNPC.h>
#include <Interface/UI.h>
#include <Forms/TESQuest.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/RemoteSmoothing.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>
#include <Systems/LeveledNpcSystem.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Events/MoveActorEvent.h>
#include <Events/PartyJoinedEvent.h>

#include <Structs/ActionEvent.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <KillCredit.h>
#include <FlyState.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/RequestActionEvents.h>
#include <Messages/NotifyActionEvents.h>
#include <MovementPacker.h>
#include <Messages/ClashRequest.h>
#include <Messages/NotifyClash.h>
#include <Messages/NotifyArrowNocked.h>
#include <Messages/ArrowNockedRequest.h>
#include <Services/ArrowNocking.h>
#include <Services/SendGrid.h>
#include <Events/ClashEvent.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Systems/SpawnReveal.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Forms/TESQuest.h>
#include <Games/Misc/MenuTopicManager.h>
#include <PlayerCharacter.h>
#include <SneakState.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestFollowerState.h>
#include <Messages/NotifyFollowerCommand.h>
#include <Messages/NotifyStuckArrow.h>
#include <Messages/NotifyDismember.h>
#include <StuckArrows.h>
#include <FollowerCommands.h>
#include <DragonSouls.h>
#include <Messages/NotifyDragonSoul.h>
#include <FollowerCommand.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>

#include <World.h>
#include <Games/Memory.h>
#include <Forms/ActorValueInfo.h>
#include <Games/TES.h>
#include <AI/AIProcess.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Services/GrabbedBody.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Games/Skyrim/Interface/Menus/HUDMenuUtils.h>
#include <Services/PlayerMeterRules.h>
#include <Services/CopyWorkGate.h>
#include <Forms/MagicItem.h>
#include <Games/Skyrim/AnimationGraphEvents.h>
#include <PlayerGraphEvents.h>

// A raise's effect by what names it, looked up again where it is made: the spell, the effect's place in it, and the source.
struct RaiseCast
{
    uint32_t SpellId{};
    uint32_t EffectIndex{};
    uint32_t SourceId{};
    NiPoint3 ExplosionLocation{};
    float Magnitude{};
    float UnkFloat1{};
    MagicSystem::CastingSource CastingSource{};
    bool AreaTarget{};
    bool DualCast{};
};

namespace
{
// Followers of this player that another player's follower hides while it is far from here: server id -> reference.
// No copy is on its way for them, so they come back when the server no longer has that character.
TiltedPhoques::Map<uint32_t, uint32_t> s_hiddenFollowers;
// The copy last made here for each other player, so an older one left standing goes (RetireOlderPlayerCopy).
PlayerCopyRules::Remembered s_playerCopies;

// References that stopped being ghosts, to be announced: discovery does not raise one again whose 3D came back quickly.
TiltedPhoques::Vector<uint32_t> s_releasedGhosts;

// Released references whose stand-in had died: each is killed once it has its 3D, then announced (GhostDeathPolicy).
struct GhostToLayDown
{
    uint32_t FormId{0};
    std::chrono::steady_clock::time_point Since{};
    float DaysPassed{0.f}; //!< game time when it was queued (GhostDeathPolicy::HasExpired)
};
TiltedPhoques::Vector<GhostToLayDown> s_ghostsToLayDown;
constexpr size_t cMaxGhostsToLayDown = 256;
// The game's enable lands a little after it is asked for; one still disabled after this is kept so by this game's own save or scripts.
constexpr auto cEnableLandsWithin = std::chrono::seconds(5);

float CurrentDaysPassed() noexcept
{
    const TimeData* pTime = TimeData::Get();
    return pTime && pTime->GameDaysPassed ? pTime->GameDaysPassed->f : 0.f;
}

bool IsWaitingToLayDown(const uint32_t aFormId) noexcept
{
    return std::any_of(s_ghostsToLayDown.begin(), s_ghostsToLayDown.end(), [aFormId](const GhostToLayDown& acEntry) { return acEntry.FormId == aFormId; });
}

// Copies of another player's follower, by form id: one the game has freed can no longer say so itself.
std::unordered_set<uint32_t> s_followerCopies;

void MarkFollowerCopy(Actor* apCopy) noexcept
{
    apCopy->GetExtension()->SetFollowerCopy(true);
    s_followerCopies.insert(apCopy->formID);
}

// Whether a copy shows another player's follower: by its own mark, or by its id once the game has freed it.
bool IsFollowerCopyId(const uint32_t aFormId, Actor* apCopy) noexcept
{
    const ActorExtension* pExtension = apCopy ? apCopy->GetExtension() : nullptr;
    return s_followerCopies.contains(aFormId) || (pExtension && pExtension->IsFollowerCopy());
}

// Released while the game's disable was still on its way: enabled once it has landed, or it would stay disabled, and in a save too.
// It stays marked as hidden until then, so a save made in between writes it as enabled.
struct PendingGhostEnable
{
    uint32_t FormId{};
    std::chrono::steady_clock::time_point GiveUpAt{};
};
TiltedPhoques::Vector<PendingGhostEnable> s_pendingGhostEnables;

// No longer hidden by this game: enables it now, or once when its disable has landed. One this game did not hide is left alone.
void StopHiding(Actor* apGhost) noexcept
{
    auto* pExtension = apGhost->GetExtension();
    if (!pExtension || !pExtension->IsHiddenHere())
        return;
    if (apGhost->IsDisabled())
    {
        pExtension->SetHiddenHere(false);
        apGhost->EnableImpl();
    }
    else
        s_pendingGhostEnables.push_back({apGhost->formID, std::chrono::steady_clock::now() + std::chrono::seconds(60)});
}

// One step in a copy's life (made, placed, moved, switched off, freed, laid down) with what it ran beside: the thread, the game's
// load state and whether the copy had its 3D. A crash right after travel can then be placed against the load that made or freed the
// copies. Called before the step, while the actor is still there.
void LogCopyLife(const char* acpStep, const uint32_t aFormId, const Actor* apActor) noexcept
{
    const uint32_t cThread = GetCurrentThreadId();
#ifdef SKYRIMVR
    const char* cpMain = cThread == GameMainThreadId() ? " (main)" : "";
#else
    const char* cpMain = "";
#endif
    spdlog::info(
        "CopyLife: {} {:X}; thread {}{}, loading screen {}, {} loading screens so far, 3D {}", acpStep, aFormId, cThread, cpMain, LoadingScreenUp() ? "up" : "down",
        UI::LoadingScreensQueued(), apActor && const_cast<Actor*>(apActor)->GetNiNode() ? "yes" : "no");
}

// Hides a ghost in this game only: the save guard (Actor.cpp) writes it to a save as enabled.
// One disabled already is left alone: it is disabled by this game's own save or scripts, and nothing here enables it again.
// The game's disable lands a little later, so one just asked for is not asked for again.
// It is switched off at the start of the next frame, on the main thread (CharacterService::DisableQueuedCopies): this runs in the
// client's update, on one of the game's job threads, and switching off a live actor there while another job runs it crashes in the
// game's movement code. It is marked hidden there, right after its disable, as before.
void HideGhost(Actor* apGhost) noexcept
{
    constexpr auto cDisableLandsWithin = std::chrono::seconds(3);
    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_askedAt;

    if (apGhost->IsDisabled())
        return;
    const auto cNow = std::chrono::steady_clock::now();
    auto* pExtension = apGhost->GetExtension();
    if (const auto asked = s_askedAt.find(apGhost->formID); pExtension && pExtension->IsHiddenHere() && asked != s_askedAt.end() && cNow - asked->second < cDisableLandsWithin)
        return;
    s_askedAt[apGhost->formID] = cNow;
    FrameStartSwitchOff::QueueGhost(apGhost);
}

// Switches off the ghosts HideGhost queued since the last frame, on the main thread at the start of a frame.
// Only one still a ghost: one released meanwhile was never switched off, so it is left as it is.
// Not while a load is up or just over: the client's update can run beside this thread then and change the ghosts and the marks read
// and set here. They stay queued until it is over (and RunGhostChecks hides again any ghost the load gave back its 3D).
void SwitchOffQueuedGhosts() noexcept
{
    static CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate))
        return;
    std::vector<uint32_t> handles = FrameStartSwitchOff::TakeQueuedGhosts();
    if (handles.empty())
        return;
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());

    for (const uint32_t cHandle : handles)
    {
        // By handle: a reference this game made at run time can have its form id given to another one once it is gone.
        Actor* pGhost = Cast<Actor>(TESObjectREFR::GetByHandle(cHandle));
        CopyLifetime::GhostFound found{};
        found.Exiting = IsProcessExiting();
        found.Actor = pGhost && !pGhost->IsDeleted();
        found.StillGhost = pGhost && CopyRegistry::IsGhost(pGhost->formID);
        found.Disabled = found.Actor && pGhost->IsDisabled();
        found.Has3D = found.Actor && pGhost->GetNiNode() && !pGhost->IsStillLoading();
        if (!CopyLifetime::SwitchOffGhost(found))
            continue;
        LogCopyLife("hidden reference switched off", pGhost->formID, pGhost);
        pGhost->Disable();
        if (ActorExtension* pExtension = pGhost->GetExtension())
            pExtension->SetHiddenHere(true);
    }
}

// What the server was last told about a follower of this player: server id -> state.
struct FollowerReport
{
    bool Told{false};
    uint8_t Differs{0}; // checks in a row where the game says otherwise
    uint64_t Visit{0};
};
TiltedPhoques::Map<uint32_t, FollowerReport> s_followerReports;

// Temporary actors this game made that now belong to another player and are out of view: server id -> local actor.
// The server sends such an actor back as a character without a reference, so every client makes a copy of it.
// The client that made it remembers it here, does not announce it again, and takes it back as that same actor.
// Otherwise one creature becomes two, and an actor/copy pair crashes on travel (`live-copy-abandon`).
// A record lasts until the server removes the character or the connection ends.
struct HandedAway
{
    uint32_t FormId{};
    uint32_t BaseFormId{}; // 0 only when the actor had no base; such a record matches nothing (CopyLifetime::IsHandedAwayActor)
};
TiltedPhoques::Map<uint32_t, HandedAway> s_handedAway;

// Whether the phase waits on a package of an NPC remote here.
// Packages only complete on the client running the NPC's AI, so this copy of the scene can't get past the phase on its own.
static bool IsPhaseStuckOnRemotePackage(BGSScene* apScene, uint32_t aPhaseIndex) noexcept
{
    // A phase with completion conditions ends once they pass, even mid-action, so this copy catches up within milliseconds.
    const BGSScenePhase* pPhase = apScene->phases[aPhaseIndex];
    if (!pPhase || pPhase->HasCompletionConditions())
        return false;

    for (BGSSceneAction* pAction : apScene->actions)
    {
        if (!pAction || pAction->GetType() != BGSSceneAction::Type::kPackage || !pAction->BlocksPhase(aPhaseIndex))
            continue;

        Actor* pActor = Cast<Actor>(apScene->owningQuest ? apScene->owningQuest->GetAliasedRef(pAction->actorId) : nullptr);
        ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        if (pExtension && pExtension->IsRemote())
            return true;
    }

    return false;
}

// Whether the local copy of the line's scene voices it itself or already did, so the sender's would cut it off and end the phase early.
// A copy stuck in an earlier phase on a remote NPC's package never reaches the line, so it plays the sender's.
static bool IsLineVoicedByLocalScene(Actor* apActor, const GameId& acSceneId, uint32_t aScenePhase) noexcept
{
    BGSScene* pScene = apActor->GetCurrentScene();
    if (!acSceneId || !pScene || !pScene->isPlaying || pScene->formID != World::Get().GetModSystem().GetGameId(acSceneId))
        return false;

    // At or past the sender's phase. A phase index past the end means the scene is starting or ending here.
    const uint32_t cPhase = pScene->currentPhaseIndex;
    if (cPhase >= aScenePhase || cPhase >= pScene->phases.length)
        return true;

    return !IsPhaseStuckOnRemotePackage(pScene, cPhase);
}

// The game's first call for a voice line only starts loading its lip file, and plays nothing.
// So a line is asked for again once that file has loaded, which also moves the NPC's mouth.
struct PendingLine
{
    String File;
    GameId SceneId;
    uint32_t ScenePhase{};
    PendingVoice Timing;
};
// By server id: a copy's form id can pass to another actor while a line waits.
TiltedPhoques::Map<uint32_t, PendingLine> s_pendingLines;
constexpr size_t kMaxPendingLines = 64;
PendingVoice::Clock::time_point s_nextPendingLineCheck{};
constexpr float kVoiceReach = 4096.f;

// A scene line left to this game's own copy of the scene, played after all if that scene says nothing here.
// A copy runs its own AI but cannot complete its owner's packages, so its scene can sit in a phase without voicing the line.
struct SceneLine
{
    String File;
    uint32_t SceneId{};
    std::chrono::steady_clock::time_point HeardAt{};
};
TiltedPhoques::Map<uint32_t, SceneLine> s_sceneLines;

// By scene form id: when a sender's line last played for a scene that stayed silent here.
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_silentScenes;

bool StartLine(Actor* apActor, const char* acpFile) noexcept
{
    // The game's speak call reads the speaker's AI process for its lip sync without checking it.
    // An actor switched on or rebuilt a moment ago can have none yet, so its line waits.
    if (!apActor->currentProcess)
        return false;

    constexpr uint32_t cNoSound = 0xFFFFFFFF;
    uint32_t soundHandle[3]{};
    apActor->SpeakSound(acpFile, soundHandle);
    return soundHandle[0] != cNoSound;
}

// Reads the load state of the actor's lip file the way the game's own dialogue does (0x14066da92).
bool IsLipLoading(const Actor* apActor) noexcept
{
    constexpr size_t cLipEntry = 0x3B8;
    constexpr size_t cLoadState = 0xC;
    constexpr uint32_t cLoadStateBits = 0x70000000;
    constexpr uint32_t cQueued = 0x10000000;
    constexpr uint32_t cLoading = 0x20000000;

    // The third word of the process: an actor near the player has its high process data there.
    const auto* pHighData = apActor->currentProcess ? reinterpret_cast<const uint8_t*>(apActor->currentProcess->highProcess) : nullptr;
    if (!pHighData)
        return false;

    const uint8_t* pEntry = *reinterpret_cast<const uint8_t* const*>(pHighData + cLipEntry);
    if (!pEntry)
        return false;

    const uint32_t cState = *reinterpret_cast<const uint32_t*>(pEntry + cLoadState) & cLoadStateBits;
    return cState == cQueued || cState == cLoading;
}

// Whether the actor's voice plays here: the handle the game's speak call keeps in the high process data (0x14065a5c0), asked of the audio manager.
bool IsVoicePlaying(const Actor* apActor) noexcept
{
    using TIsPlaying = bool(const uint32_t* apHandle);
    POINTER_SKYRIMSE(TIsPlaying, s_isPlaying, 67620, 66359);
    constexpr size_t cVoiceHandle = 0x2FC;

    const auto* pHigh = apActor->currentProcess ? reinterpret_cast<const uint8_t*>(apActor->currentProcess->highProcess) : nullptr;
    if (!pHigh || !s_isPlaying.Get())
        return false;

    const uint32_t cHandle[3]{*reinterpret_cast<const uint32_t*>(pHigh + cVoiceHandle), 0, 0};
    return s_isPlaying.Get()(cHandle);
}

bool PlayOrDeferLine(uint32_t aServerId, PendingLine& aLine, PendingVoice::Clock::time_point aNow) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(aServerId);
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pActor || !pPlayer)
        return false;
    if (pActor->actorState.IsDeadOrDying() || glm::distance(static_cast<glm::vec3>(pActor->position), static_cast<glm::vec3>(pPlayer->position)) >= kVoiceReach)
        return true;
    if (!pActor->GetNiNode())
        return false;

    if (IsLineVoicedByLocalScene(pActor, aLine.SceneId, aLine.ScenePhase))
    {
        s_sceneLines[aServerId] = SceneLine{aLine.File, pActor->GetCurrentScene()->formID, aLine.Timing.FirstSeen};
        return true;
    }

    if (!pActor->currentProcess || (aLine.Timing.WaitingForLip(aNow) && IsLipLoading(pActor)))
        return false;

    if (!aLine.Timing.FirstAsked)
    {
        pActor->StopCurrentDialogue(true);
        aLine.Timing.FirstAsked = aNow;
    }

    if (!StartLine(pActor, aLine.File.c_str()))
        return false;
    spdlog::debug(
        "Dialogue: {:X} says {}; started here after {} ms", pActor->formID, aLine.File.c_str(),
        std::chrono::duration_cast<std::chrono::milliseconds>(aNow - aLine.Timing.FirstSeen).count());
    return true;
}

void RunPendingLines() noexcept
{
    if (s_pendingLines.empty())
        return;
    const auto cNow = PendingVoice::Clock::now();
    if (cNow < s_nextPendingLineCheck)
        return;
    s_nextPendingLineCheck = cNow + 100ms;

    for (auto it = s_pendingLines.begin(); it != s_pendingLines.end();)
    {
        auto& line = it.value();
        if (line.Timing.Expired(cNow) || PlayOrDeferLine(it->first, line, cNow))
            it = s_pendingLines.erase(it);
        else
            ++it;
    }
}

// A scene line left to this game's scene: if the scene speaks here, it voices the line; if it stays silent, the sender's line plays.
// Judged by any line of the scene rather than this file: a copy can say another variant, or say it a little before or after the sender.
void RunSceneLines() noexcept
{
    constexpr auto cSpokeWithin = std::chrono::seconds(6);         // before the sender's line arrived: a scene running a little ahead
    constexpr auto cFirstWait = std::chrono::seconds(3);           // a scene not yet seen silent, or a copy still talking
    constexpr auto cSilentWait = std::chrono::milliseconds(800);   // a scene that stayed silent for its last line
    constexpr auto cSilentFor = std::chrono::seconds(30);
    const auto cNow = std::chrono::steady_clock::now();
    for (auto it = s_sceneLines.begin(); it != s_sceneLines.end();)
    {
        const SceneLine& cLine = it->second;
        const auto cWaited = cNow - cLine.HeardAt;
        Actor* pActor = Utils::GetByServerId<Actor>(it->first);
        if (SceneLines::SceneSpokeSince(cLine.SceneId, cLine.HeardAt - cSpokeWithin))
        {
            spdlog::info("Dialogue: {:X} says {}; this scene speaks here too, so it voices the line", pActor ? pActor->formID : 0, cLine.File.c_str());
            s_silentScenes.erase(cLine.SceneId);
            it = s_sceneLines.erase(it);
            continue;
        }

        const auto cSilent = s_silentScenes.find(cLine.SceneId);
        const bool cKnownSilent = cSilent != s_silentScenes.end() && cNow - cSilent->second < cSilentFor;
        const bool cTalking = pActor && IsVoicePlaying(pActor);
        if (cWaited < (cKnownSilent && !cTalking ? cSilentWait : cFirstWait))
        {
            ++it;
            continue;
        }

        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (pActor && pPlayer && pActor->GetNiNode() && !pActor->actorState.IsDeadOrDying())
        {
            const float cDistance = glm::distance(static_cast<glm::vec3>(pActor->position), static_cast<glm::vec3>(pPlayer->position));
            if (cDistance < kVoiceReach)
            {
                spdlog::info("Dialogue: {:X} says {}; this scene said nothing here for {} ms, so the sender's line plays", pActor->formID, cLine.File.c_str(),
                             std::chrono::duration_cast<std::chrono::milliseconds>(cWaited).count());
                s_silentScenes[cLine.SceneId] = cNow;
                // No scene id from here on: the pending path would otherwise leave it to the scene again.
                if (!StartLine(pActor, cLine.File.c_str()) && (s_pendingLines.size() < kMaxPendingLines || s_pendingLines.contains(it->first)))
                    s_pendingLines.insert_or_assign(it->first, PendingLine{cLine.File, GameId{}, 0, {cNow, cNow}});
            }
        }
        it = s_sceneLines.erase(it);
    }
}

// References standing here whose teleport was refused, and when.
// The server sends that teleport right before it takes a character through a door and out of this player's view.
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_teleportsRefused;

// Actors found with no cell, which nothing would announce later.
TiltedPhoques::Vector<entt::entity> s_notAnnounced;

// Whether this actor is one that was handed away, and as which server character.
bool IsHandedAway(const Actor* apActor, uint32_t& aServerId) noexcept
{
    for (const auto& [serverId, entry] : s_handedAway)
    {
        if (entry.FormId != apActor->formID || !CopyLifetime::IsHandedAwayActor(entry.BaseFormId, apActor->baseForm ? apActor->baseForm->formID : 0))
            continue;
        aServerId = serverId;
        return true;
    }
    return false;
}

// Copies this client still had when its last connection ended: form id -> base form id.
// The clean-up on connect deletes them instead of announcing them to the server as new characters.
// The base is kept because the game reuses freed ids; an id also leaves this map when the game removes the actor.
TiltedPhoques::Map<uint32_t, uint32_t> s_lastConnectionCopies;

// Removed copies the game still holds handles on: disabled, and freed once the game lets go (CopyRemovalPolicy.h).
// The list never holds more than kMaxWaiting, whatever the game does, so copies cannot pile up.
// RecentDeletes records every deletion, so the crash handler can tell when a register points at a freed copy.
struct WaitingCopy
{
    uint32_t FormId{};
    uint32_t Handle{}; // tells this copy from a later actor on the same form id
    std::chrono::steady_clock::time_point QueuedAt{};
};

TiltedPhoques::Vector<WaitingCopy> s_waitingCopies;
// Removals the server or this client asked for while a load was up or just over, done once it is over (RunRemovalsAfterLoad).
// The client's update can run beside the main thread during a load, which loads and unloads cells and frees references itself.
TiltedPhoques::Vector<CopyLifetime::LaterRemoval> s_removalsAfterLoad;
// Asked by every removal and by the pass that does the kept ones; both run in the client's update.
CopyWork::LoadGate s_removalLoadGate;
// Older player copies on a horse this game put them on, retired: MountService takes each off its horse and deletes it after that.
TiltedPhoques::Vector<WaitingCopy> s_releasingCopies;

// Raises waiting for their body, and raises decided on for the main thread (CharacterService::MakeDecidedRaises).
RaiseClaim::Raises<RaiseCast> s_raises;
// A raise is being sent to the game that runs the body, on this thread: its own effect is not held for the body again.
thread_local bool t_sendingRaiseToOwner = false;

// The waiting entry of that actor, by handle as well as id, or null when it is not waiting to be freed.
WaitingCopy* FindWaitingCopy(const Actor* apActor) noexcept
{
    if (s_waitingCopies.empty())
        return nullptr;
    const uint32_t cHandle = apActor->GetHandle().handle.iBits;
    const auto it = std::find_if(s_waitingCopies.begin(), s_waitingCopies.end(),
                                 [apActor, cHandle](const WaitingCopy& acWaiting) { return acWaiting.FormId == apActor->formID && acWaiting.Handle == cHandle; });
    return it != s_waitingCopies.end() ? &*it : nullptr;
}

Actor* RememberOwnCopy(Actor* apActor) noexcept
{
    if (!apActor)
        return nullptr;

    // A waiting copy on this id is gone: its entry must not take the mark off the new one.
    std::erase_if(s_waitingCopies, [cFormId = apActor->formID](const WaitingCopy& acWaiting) { return acWaiting.FormId == cFormId; });
    std::erase_if(s_releasingCopies, [cFormId = apActor->formID](const WaitingCopy& acReleasing) { return acReleasing.FormId == cFormId; });
    // Nor may a freed follower copy's mark on this id pass to it.
    s_followerCopies.erase(apActor->formID);
    CopyRegistry::AddOwnCopy(apActor->formID);
    return apActor;
}

struct BaseMatch
{
    bool Differs = false;       // the owner's base is not this side's base
    bool Foreign = false;       // and it is another kind of creature
    TESNPC* pOwnerBase = nullptr;
};

// Compares the owner's base with the local one: a differing base is "foreign" when it is another kind of creature.
// Same name means same kind: a levelled list mixes humanoid races freely (Nord and Breton bandits share one graph).
// Only when the names differ does the race decide (Elk vs Deer is foreign; Dragon vs Blood Dragon is not).
// Two people are never foreign, whatever their races: a stand-in made from the owner's pick would lose the name its reference shows.
// Races come from Actor::race and TESNPC::raceForm; if those disagree for the local actor, the race is not trusted.
BaseMatch CompareBase(World& aWorld, Actor* apActor, const GameId& acRemoteBaseId, const bool aLog) noexcept
{
    BaseMatch match{};
    if (acRemoteBaseId == GameId{})
        return match;

    const uint32_t cRemoteBaseId = aWorld.GetModSystem().GetGameId(acRemoteBaseId);

    const TESNPC* pLocalBase = Cast<TESNPC>(apActor->baseForm);
    if (pLocalBase && pLocalBase->IsTemporary())
        pLocalBase = pLocalBase->GetTemplateBase();

    if (!cRemoteBaseId || !pLocalBase || pLocalBase->formID == cRemoteBaseId)
        return match;

    TESNPC* pRemoteBase = Cast<TESNPC>(TESForm::GetById(cRemoteBaseId));
    if (!pRemoteBase)
        return match;

    match.Differs = true;
    match.pOwnerBase = pRemoteBase;

    const char* pLocalName = pLocalBase->fullName.value.AsAscii();
    const char* pRemoteName = pRemoteBase->fullName.value.AsAscii();

    const bool sameName = std::strcmp(pLocalName ? pLocalName : "", pRemoteName ? pRemoteName : "") == 0;

    // The race is read only when it is this actor's own; a base whose race the actor does not have cannot speak for it.
    const bool cTrusted = apActor->race && pLocalBase->raceForm.race == apActor->race;
    const TESRace* pRemoteRace = pRemoteBase->raceForm.race;
    const bool cBothPersons = cTrusted && pRemoteRace && Actor::IsPersonRace(apActor->race) && Actor::IsPersonRace(pRemoteRace);
    const auto cVerdict = BaseKind::Decide(sameName, cTrusted && pRemoteRace ? apActor->race->formID : 0, cTrusted && pRemoteRace ? pRemoteRace->formID : 0, cBothPersons);
    match.Foreign = cVerdict.Foreign;
    const char* pHow = cVerdict.By == BaseKind::Decider::Race ? "race" : cVerdict.By == BaseKind::Decider::Name ? "name" : "name, race unreadable";

    if (aLog)
        spdlog::info("Base form differs on reference {:X}: owner has {:X} ({}), this side {:X} ({}); {} (decided by {})", apActor->formID, cRemoteBaseId,
                     pRemoteName ? pRemoteName : "?", pLocalBase->formID, pLocalName ? pLocalName : "?",
                     match.Foreign ? "another kind of creature" : "same kind, synced normally", pHow);

    return match;
}

// Fallback when no copy could be made: the local creature is adopted but never fed the owner's animation data.
// That data belongs to a graph it does not have; without it the creature slides instead of freezing.
void MarkForeignGraph(World& aWorld, const entt::entity aEntity, Actor* apActor, const GameId& acRemoteBaseId) noexcept
{
    if (!CompareBase(aWorld, apActor, acRemoteBaseId, true).Foreign)
        return;

    if (auto* pAnimation = aWorld.try_get<RemoteAnimationComponent>(aEntity))
        pAnimation->ForeignGraph = true;
    if (auto* pInterpolation = aWorld.try_get<InterpolationComponent>(aEntity))
        pInterpolation->ForeignGraph = true;
}

// The base a copy of the owner's temporary actor is made from: the owner's leveled pick, else the template base.
// A named base whose name differs from the pick's is kept: the pick is only where its stats come from.
// Example: Boethiah Cultist B0E87 takes its stats from Bandit Marauder 39D25 and must not appear as a bandit.
TESNPC* BaseForCopy(World& aWorld, const CharacterSpawnRequest& acMessage) noexcept
{
    TESNPC* pBase = Cast<TESNPC>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acMessage.BaseId)));
    if (acMessage.LeveledNpcPickId == GameId{})
        return pBase;

    TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));
    const char* pBaseName = pBase ? pBase->fullName.value.AsAscii() : nullptr;
    const char* pPickName = pPick ? pPick->fullName.value.AsAscii() : nullptr;
    if (pPick && pBase && pPick != pBase && pBaseName && *pBaseName && std::strcmp(pBaseName, pPickName ? pPickName : "") != 0)
    {
        spdlog::info("Copy for server id {:X} made from its own base {:X} ({}), not the levelled pick {:X} ({}) it takes its stats from", acMessage.ServerId, pBase->formID, pBaseName,
                     pPick->formID, pPickName ? pPickName : "?");
        return pBase;
    }
    return pPick ? pPick : pBase;
}

// Bodies asked to be revived while switched off, by form id.
// A disabled actor has no 3D and the game's revival does not take, so each waits for its switch on.
TiltedPhoques::Vector<uint32_t> s_revivalsOnSwitchOn;

// Actors whose life follows their owner's whatever this game's save says: ones the leader's game switched on here.
// A plugin uses the same soldier battle after battle, so a body left from an earlier one is no death this game saw first.
// One its owner revived is marked by the server instead (RevivedSinceDeath), so that a game joining later knows it too.
std::unordered_set<uint32_t> s_lifeFollowsOwner;

// When the leader's game last switched each actor on here.
// For a moment after, the actor is that game's to announce: its plugin or quest placed it, and this game's actor becomes its copy there.
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_leaderSwitchedOnAt;
constexpr auto cWaitForLeader = 5s;

// True while the leader's game may still announce an actor it switched on here; logs the wait's end when nothing came.
// A body here whose life follows its owner's waits for as long as it takes: announced from here, this game's save would make it the party's body.
bool WaitsForLeader(const Actor* apActor, const bool aIsLeader) noexcept
{
    const auto cIt = s_leaderSwitchedOnAt.find(apActor->formID);
    if (cIt == s_leaderSwitchedOnAt.end())
        return false;
    if (aIsLeader)
    {
        s_leaderSwitchedOnAt.erase(cIt);
        return false;
    }
    constexpr auto cForever = std::chrono::steady_clock::time_point::max();
    if (cIt->second == cForever || std::chrono::steady_clock::now() - cIt->second < cWaitForLeader)
        return true;
    const auto cSeconds = std::chrono::duration_cast<std::chrono::seconds>(cWaitForLeader).count();
    if (apActor->IsDead() && s_lifeFollowsOwner.contains(apActor->formID))
    {
        spdlog::info("Actor {:X} is a body here, and the leader's game sent no character within {} s of its switch on; left for that game to announce", apActor->formID, cSeconds);
        s_leaderSwitchedOnAt[apActor->formID] = cForever;
        return true;
    }
    spdlog::info("Actor {:X}: no character from the leader's game within {} s of its switch on; announced here", apActor->formID, cSeconds);
    s_leaderSwitchedOnAt.erase(cIt);
    return false;
}

// A body whose 3D stands has no death left to play, so it goes straight into its ragdoll.
void LayDownBody(Actor* apActor) noexcept
{
    if (apActor->IsTemporary())
        LogCopyLife("laid down (ragdoll)", apActor->formID, apActor);
    BSFixedString ragdoll("RagdollInstant");
    apActor->SendAnimationEvent(&ragdoll);
}

// An NPC dead in this game that its owner's game has alive: a death that game never saw, kept by its save.
// It is one killed behind a stand-in, or in a cell the other game did not have, or after its last save.
// Stood up here, it is a dead soldier walking again, a different one in each game.
// So a generic one stays dead here, as it lies; a unique one follows its owner, since a quest may have brought it back.
// So does one in s_lifeFollowsOwner, and one its owner's game revived since its last death (aRevivedAtOwner, the server's mark).
// That revive is a death the owner did see: a siege brings its soldiers back, and a body from an earlier siege is no reason to kill them.
// A kill made here where the owner did not see it, after that revive, is lost: the body stands up again, as it does at the owner.
bool IsDeadAgainstOwner(Actor* apActor, bool aDeadAtOwner, bool aRevivedAtOwner) noexcept
{
    if (aDeadAtOwner || !apActor || !apActor->IsDead())
        return false;
    if (apActor->GetExtension() && apActor->GetExtension()->IsPlayer())
        return false;
    if (aRevivedAtOwner)
    {
        spdlog::info("Death sync: {:X} is dead here, but its owner's game revived it since its last death; it follows its owner", apActor->formID);
        return false;
    }
    if (s_lifeFollowsOwner.contains(apActor->formID))
        return false;
    const auto* pBase = Cast<TESNPC>(apActor->baseForm);
    return pBase && !pBase->actorData.IsUnique();
}

// Its owner is sent a killing blow, the way this player's own killing blow on a copy goes, so that it dies there too.
// It goes straight to the server: a body is not one of this game's tracked actors, so a HealthChangeEvent finds no entity.
void SendKillingBlowToOwner(TransportService& aTransport, const Actor* apActor, uint32_t aServerId) noexcept
{
    if (aServerId == 0)
        return;
    // Credits nobody with a kill: it only brings the two games to the same death (KillCredit::CreditsAttacker).
    RequestHealthChangeBroadcast request{};
    request.Id = aServerId;
    request.DeltaHealth = -KillCredit::kDeathWinsBlow;
    request.Tick = aTransport.GetClock().GetCurrentTick();
    aTransport.Send(request);
    spdlog::info("DeathWins: {:X} (server id {:X}) is dead here but alive in its owner's game; kept dead here, and its owner is sent a killing blow", apActor->formID,
                 aServerId);
}

// Spawns the owner's creature and turns the local one into a ghost.
// Returns null when no copy could be made; the caller then adopts the local creature.
Actor* StandInForForeignCreature(Actor* apLocal, TESNPC* apOwnerBase) noexcept
{
    Actor* pCopy = RememberOwnCopy(Actor::Create(apOwnerBase));
    if (!pCopy)
    {
        spdlog::warn("Ghost: could not create a copy of {:X} for reference {:X}, adopting the local creature instead", apOwnerBase->formID, apLocal->formID);
        return nullptr;
    }
    LogCopyLife("made (stand-in)", pCopy->formID, pCopy);

    const TESNPC* pLocalBase = Cast<TESNPC>(apLocal->baseForm);
    if (pLocalBase && pLocalBase->IsTemporary())
        pLocalBase = pLocalBase->GetTemplateBase();

    CopyRegistry::SetStandIn(apLocal->formID, pCopy->formID);

    // Only a loaded creature is disabled now: one without 3D may be in the middle of being disposed of by the game.
    // Disabling it then crashes in the game's movement code (SkyrimVR.exe+0x714EB2).
    // One that is not loaded is disabled once it loads (ProcessNewEntity).
    const bool cLoaded = apLocal->GetNiNode() != nullptr && !apLocal->IsDeleted() && !apLocal->IsStillLoading();
    if (cLoaded)
        HideGhost(apLocal);

    spdlog::info("Ghost: reference {:X} rolled as {} ({:X}) here but the owner has {} ({:X}); copy {:X} stands in for it and the local one is {}", apLocal->formID,
                 pLocalBase ? pLocalBase->fullName.value.AsAscii() : "?", pLocalBase ? pLocalBase->formID : 0, apOwnerBase->fullName.value.AsAscii(), apOwnerBase->formID,
                 pCopy->formID, cLoaded ? "disabled" : "not loaded here (disabled once it loads)");
    return pCopy;
}

void EnableGhost(const uint32_t aGhostFormId) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pGhost = Cast<Actor>(TESForm::GetById(aGhostFormId));
    if (pGhost)
        StopHiding(pGhost);
    s_releasedGhosts.push_back(aGhostFormId);
    spdlog::info("Ghost: reference {:X} is a normal local creature again{}", aGhostFormId, pGhost ? "" : " (not loaded)");
}

// Another player's follower is shown on a copy, and this game's own reference stays as its save has it.
// Null when no copy can be made or a save would keep her hidden; she is then shown on the reference, as any other NPC.
Actor* FollowerCopy(Actor* apLocal, TESNPC* apOwnerBase, const uint32_t aServerId, const uint32_t aPlayerId) noexcept
{
    if (!apOwnerBase || !Actor::IsSaveGuarded())
    {
        spdlog::warn("Follower: {:X} (server id {:X}) stays on this game's own reference: {}", apLocal->formID, aServerId, apOwnerBase ? "saves are not guarded" : "no base to copy");
        return nullptr;
    }
    Actor* pCopy = StandInForForeignCreature(apLocal, apOwnerBase);
    if (!pCopy)
        return nullptr;
    MarkFollowerCopy(pCopy);
    spdlog::info("Follower: {:X} (server id {:X}) is player {:X}'s; copy {:X} shows her, and this game's own reference is left as it is", apLocal->formID, aServerId, aPlayerId, pCopy->formID);
    return pCopy;
}

// Whether a copy going now is the body of the owner's creature, read before it is deleted.
bool IsDeadCopy(Actor* apCopy) noexcept
{
    if (!apCopy || apCopy->IsDeleted() || !apCopy->IsDead())
        return false;
    const ActorExtension* pExtension = apCopy->GetExtension();
    return GhostDeathPolicy::OnStandInGone(true, pExtension && pExtension->IsFollowerCopy()) == GhostDeathPolicy::Action::LayDown;
}

// Whether the game running a character has said it is dead: its owner, or this game, which ran it and saw it die.
bool DeadByRunnersWord(const RemoteComponent* apRemote, const LocalComponent* apLocal) noexcept
{
    return apRemote ? apRemote->DeadAtOwner || apRemote->DeadHere : apLocal && apLocal->IsDead;
}

// A ghost whose stand-in was a body comes back as a body: the owner's creature died, so this game's own roll is dead too.
// Not a temporary reference: the game gives its id to another actor once it is gone, and that one would be killed.
void LayDownWhenReleased(const uint32_t aGhost, const uint32_t aCopyFormId) noexcept
{
    if (TESForm::IsTemporaryId(aGhost) || IsWaitingToLayDown(aGhost))
        return;
    if (s_ghostsToLayDown.size() >= cMaxGhostsToLayDown)
    {
        // Left as it is, and announced as before.
        s_releasedGhosts.push_back(s_ghostsToLayDown.front().FormId);
        s_ghostsToLayDown.erase(s_ghostsToLayDown.begin());
    }
    s_ghostsToLayDown.push_back({aGhost, std::chrono::steady_clock::now(), CurrentDaysPassed()});
    spdlog::debug("Ghost: reference {:X} comes back as a body: the owner's creature that copy {:X} stood in for is dead", aGhost, aCopyFormId);
}

// Called for every temporary actor this client deletes: if it was a stand-in, its ghost comes back.
void ReleaseGhostOf(const uint32_t aCopyFormId, const bool aCopyDead) noexcept
{
    const auto cGhost = CopyRegistry::TakeGhostOfCopy(aCopyFormId);
    if (!cGhost)
        return;
    if (aCopyDead)
        LayDownWhenReleased(*cGhost, aCopyFormId);
    EnableGhost(*cGhost);
}

uint32_t OutstandingHandles(const Actor* apActor) noexcept
{
    return static_cast<uint32_t>(apActor->handleRefObject.refCount) & 0x3FF;
}

void FreeCopy(Actor* apActor, const uint32_t aFormId, const char* acpWhy) noexcept
{
    const uint32_t refWord = static_cast<uint32_t>(apActor->handleRefObject.refCount);
    RecentDeletes::Record(apActor, aFormId, refWord & 0x3FF, refWord, 0);
    // session-report.py parses this line; keep the colon before the reason.
    spdlog::info("Temporary Remote Deleted {:X}: {}", aFormId, acpWhy);
    LogCopyLife("freed after its wait", aFormId, apActor);
    apActor->Delete();
    CopyRegistry::ForgetOwnCopy(aFormId); // the game may give this id to an actor of its own next
}

void RunWaitingCopies() noexcept
{
    // Not while a load is up or just over: this runs in the client's update, which can run beside the main thread during a load, and
    // the load frees and makes references itself. A copy left waiting is looked at again once it is over.
    static CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate) || s_waitingCopies.empty())
        return;

    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 250ms;

    TiltedPhoques::Vector<WaitingCopy> stillWaiting;
    stillWaiting.reserve(s_waitingCopies.size());

    for (const WaitingCopy& waiting : s_waitingCopies)
    {
        // By handle: the game reuses a freed form id, and an id lookup would then free the actor that has it now.
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(waiting.Handle));
        if (!pActor || pActor->IsDeleted() || !pActor->IsTemporary())
        {
            CopyRegistry::ForgetOwnCopy(waiting.FormId);
            continue; // gone already, which is the outcome we wanted
        }

        const auto waitedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - waiting.QueuedAt).count());
        const bool overBound = stillWaiting.size() >= CopyRemovalPolicy::kMaxWaiting;
        const uint32_t handles = OutstandingHandles(pActor);
        const auto reason = CopyRemovalPolicy::ReleaseReason(handles, waitedMs, overBound);

        if (reason != CopyRemovalPolicy::Released::NotYet)
        {
            FreeCopy(pActor, waiting.FormId, CopyRemovalPolicy::Describe(reason));

            // A copy freed while the game still holds it is the case the wait exists to prevent, so it gets its own warning.
            if (CopyRemovalPolicy::FreedWhileHeld(reason))
                spdlog::warn("CopyFreedHeld: {:X} freed after {} ms with {} handle(s) still outstanding -- {}",
                             waiting.FormId, waitedMs, handles, CopyRemovalPolicy::Describe(reason));
            continue;
        }

        // One that had no 3D when it was queued was left switched on; once its cell gives it 3D it is switched off.
        if (!pActor->IsDisabled() && pActor->GetNiNode() && !pActor->IsStillLoading())
            FrameStartSwitchOff::Queue(pActor);

        stillWaiting.push_back(waiting);
    }

    s_waitingCopies.swap(stillWaiting);
}

// An older player copy retired while on a horse this game put it on is taken off it first and deleted after that (MountService), as
// the disconnect does. True when MountService took its deletion over.
bool ReleaseThenRetire(const uint32_t aServerId, Actor* apCopy) noexcept
{
    if (!apCopy || !apCopy->IsTemporary() || !MountService::ReleaseBeforeDelete(aServerId, apCopy))
        return false;
    s_releasingCopies.push_back(WaitingCopy{apCopy->formID, apCopy->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
    return true;
}

bool IsReleasing(const Actor* apActor) noexcept
{
    if (s_releasingCopies.empty())
        return false;
    const uint32_t cHandle = apActor->GetHandle().handle.iBits;
    return std::any_of(
        s_releasingCopies.begin(), s_releasingCopies.end(),
        [apActor, cHandle](const WaitingCopy& acReleasing) { return acReleasing.FormId == apActor->formID && acReleasing.Handle == cHandle; });
}

// The copies MountService deletes once they are off their horses: each is forgotten as an own copy once it is gone. One its work never
// reached (a stand-in undo is held at most CopyWork::kHoldUndoMs over a load) is removed as any other copy, unless MountService still
// holds its deletion until it is off its horse (MountService::DeletionPending).
void RunReleasingCopies() noexcept
{
    if (s_releasingCopies.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<uint32_t> overdue;
    std::erase_if(
        s_releasingCopies,
        [cNow, &overdue](const WaitingCopy& acReleasing)
        {
            // By handle: the game reuses a freed form id.
            const Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(acReleasing.Handle));
            if (!pActor || pActor->IsDeleted() || pActor->formID != acReleasing.FormId)
            {
                CopyRegistry::ForgetOwnCopy(acReleasing.FormId);
                return true;
            }
            if (cNow - acReleasing.QueuedAt < std::chrono::milliseconds(CopyWork::kHoldUndoMs) + 5s || MountService::DeletionPending(acReleasing.Handle))
                return false;
            overdue.push_back(acReleasing.FormId);
            return true;
        });

    for (const uint32_t cFormId : overdue)
    {
        spdlog::debug("Player copy: {:X} was not taken off its horse in time; removed as it is", cFormId);
        CharacterService::DeleteTempActor(cFormId);
    }
}

void ReleaseAllGhosts() noexcept
{
    for (const auto& [ghost, copy] : CopyRegistry::TakeAllGhosts())
        EnableGhost(ghost);
}

// A ghost disabled while its cell loads can get its 3D back from the game and fight, unknown to the server.
// So every two seconds a loaded ghost that is enabled again is disabled again.
// Only one with 3D that is not deleted and no longer loading is touched: disabling one the game is disposing of crashes (SkyrimVR.exe+0x714EB2).
void RunGhostChecks() noexcept
{
    static std::chrono::steady_clock::time_point s_nextCheck{};
    static TiltedPhoques::Map<uint32_t, uint32_t> s_redisabled; // ghost form id -> times disabled again
    const auto cNow = std::chrono::steady_clock::now();
    // The first check two seconds after a ghost appears, not in the frame it was hidden, when its disable has not landed yet.
    if (CopyRegistry::Ghosts().empty())
    {
        s_nextCheck = cNow + std::chrono::seconds(2);
        return;
    }
    if (cNow < s_nextCheck || IsProcessExiting())
        return;
    s_nextCheck = cNow + std::chrono::seconds(2);

    TiltedPhoques::Vector<uint32_t> noLongerFollowers;
    for (const auto& [ghost, copy] : CopyRegistry::Ghosts())
    {
        Actor* pGhost = Cast<Actor>(TESForm::GetById(ghost));
        // Hidden with no copy only as this player's follower: after a load of a save where she is not one, she is not hidden any more.
        if (pGhost && copy == 0 && !pGhost->IsPlayerFollower())
        {
            noLongerFollowers.push_back(ghost);
            continue;
        }
        if (!pGhost || pGhost->IsDisabled() || !pGhost->GetNiNode() || pGhost->IsDeleted() || pGhost->IsStillLoading())
            continue;
        HideGhost(pGhost);
        const uint32_t cTimes = ++s_redisabled[ghost];
        if (cTimes == 1 || cTimes % 10 == 0)
            spdlog::info("Ghost: reference {:X} was enabled again with its 3D while copy {:X} stands in; disabled again ({} times)", ghost, copy, cTimes);
    }

    for (const uint32_t cGhost : noLongerFollowers)
    {
        CopyRegistry::ForgetGhost(cGhost);
        EnableGhost(cGhost);
    }
}
//! The values a copy that is to be dead starts with: a little health, so that it lives until it is killed with its 3D.
//! With none it dies as they are set, with nothing played, and the kill finds it dead and leaves it on its feet.
//! Never for a body already dead here: no kill would come to take that health, and it gets back up.
ActorValues ValuesUntilKilled(const ActorValues& acValues) noexcept
{
    ActorValues values = acValues;
    if (auto it = values.ActorValuesList.find(ActorValueInfo::kHealth); it != values.ActorValuesList.end() && it->second <= 0.f)
        it.value() = 1.f;
    return values;
}

/**
 * Choose the values a copy takes from its owner's.
 * A copy that is to be dead and is still alive here gets ValuesUntilKilled; any other copy gets the owner's values as they are.
 *
 * @param acValues The owner's values.
 * @param aDeadAtOwner Whether the character is dead in its owner's game.
 * @param apActor The copy the values are for.
 * @return The values to set on the copy.
 */
ActorValues ValuesForCopy(const ActorValues& acValues, const bool aDeadAtOwner, Actor* apActor) noexcept
{
    return aDeadAtOwner && !apActor->IsDead() ? ValuesUntilKilled(acValues) : acValues;
}

//! The values a player's fresh copy starts with: visible, and health never below a quarter of its maximum.
//! The server's stored values for a respawning player can be the negative health from the death.
//! A copy that starts that low goes down before the owner's real health arrives; the owner's game decides deaths.
ActorValues StandingValues(const ActorValues& acValues, const uint32_t aFormId) noexcept
{
    ActorValues values = acValues;
    // Invisibility (54) travels with every other value; a copy born invisible stays invisible until a reconnect.
    if (auto inv = values.ActorValuesList.find(ActorValueInfo::kInvisibility); inv != values.ActorValuesList.end() && inv->second != 0.f)
    {
        spdlog::info("Remote player {:X} copy would have spawned invisible ({:.2f}); spawned visible instead", aFormId, inv->second);
        inv.value() = 0.f;
    }
    auto it = values.ActorValuesList.find(ActorValueInfo::kHealth);
    if (it == values.ActorValuesList.end())
        return values;
    float floor = 25.f;
    if (const auto max = values.ActorMaxValuesList.find(ActorValueInfo::kHealth); max != values.ActorMaxValuesList.end() && max->second > 0.f)
        floor = std::max(floor, max->second * 0.25f);
    if (it->second < floor)
    {
        spdlog::info("Remote player {:X} copy would have spawned with the owner's health {:.0f} (a copy that starts down never gets up); started at {:.0f} instead", aFormId, it->second, floor);
        it.value() = floor;
    }
    return values;
}

/**
 * Choose the values a copy takes from its spawn message.
 * A player's copy never starts down, and a body that is to be dead lives until it is killed with its 3D.
 *
 * @param acMessage The spawn message from the server.
 * @param apActor The copy the values are for.
 * @return The owner's values, adjusted for the copy.
 */
ActorValues SpawnValues(const CharacterSpawnRequest& acMessage, Actor* apActor) noexcept
{
    if (acMessage.IsPlayer)
        return StandingValues(acMessage.InitialActorValues, apActor->formID);
    return ValuesForCopy(acMessage.InitialActorValues, acMessage.IsDead, apActor);
}

/**
 * Set up a copy of another game's character from its spawn message.
 * The copy is marked remote, turned and placed in this player's cell, and given the owner's values.
 * An NPC copy is also marked as waiting for its 3D step, so a death before that step is logged (SpawnDeathDiag).
 * A player's copy is also marked as one, and its entity gets the player's component.
 *
 * @param aWorld The world that holds the character's entity.
 * @param aEntity The character's entity.
 * @param apActor The copy.
 * @param acMessage The spawn message from the server.
 * @param acPosition Where the copy is placed.
 */
void SetUpCopyFromSpawn(World& aWorld, const entt::entity aEntity, Actor* apActor, const CharacterSpawnRequest& acMessage, const NiPoint3& acPosition) noexcept
{
    apActor->GetExtension()->SetRemote(true);
    // Marked before its values, which a copy can die of, so a death before the 3D step is logged with its stack (SpawnDeathDiag).
    apActor->GetExtension()->AwaitingFirst3D = !acMessage.IsPlayer;
    apActor->rotation.x = acMessage.Rotation.x;
    apActor->rotation.z = acMessage.Rotation.y;
    LogCopyLife("placed (from its spawn)", apActor->formID, apActor);
    apActor->MoveTo(PlayerCharacter::Get()->parentCell, acPosition);
    apActor->SetActorValues(SpawnValues(acMessage, apActor));

    apActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        apActor->SetIgnoreFriendlyHit(true);
        apActor->SetPlayerRespawnMode();
        // The copy stays essential (it cannot die here), but may get up again once its owner's real health arrives.
        apActor->SetNoBleedoutRecovery(false);
        aWorld.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
    }
}

/**
 * Kill a fresh copy or bring it back, so that it is dead or alive as in its owner's game.
 * A copy that is to be dead but has no 3D yet is left alive; the WaitingFor3D step kills it once it has its 3D.
 * Killed before that, it dies with nothing played, and that step then finds it dead and leaves it standing.
 *
 * @param apActor The copy.
 * @param aDeadAtOwner Whether the character is dead in its owner's game.
 */
void MatchOwnersLife(Actor* apActor, const bool aDeadAtOwner) noexcept
{
    if (apActor->IsDead() == aDeadAtOwner)
        return;
    if (!aDeadAtOwner)
        apActor->Respawn();
    else if (apActor->GetNiNode())
        apActor->Kill();
}

/**
 * Find the server id of a character that speaks, so that its line can be sent.
 * A speaker with no entity is not shared with the server, and is skipped without a word.
 * One with an entity but no server id yet is logged, as an error.
 *
 * @param aWorld The world that holds the characters' entities.
 * @param aFormId The speaker's form id.
 * @param acpCaller The calling function's name, for the log line.
 * @return The server id, or nothing when the line cannot be sent.
 */
std::optional<uint32_t> SpeakerServerId(World& aWorld, const uint32_t aFormId, const char* acpCaller) noexcept
{
    const auto cEntity = Utils::FindByFormId(aWorld.view<FormIdComponent>(entt::exclude<ObjectComponent>), aFormId);
    if (!cEntity)
        return std::nullopt;

    const auto cServerId = Utils::GetServerId(*cEntity);
    if (!cServerId)
        spdlog::error("{}: server id not found for form id {:X}", acpCaller, aFormId);
    return cServerId;
}

// Measurement: an NPC this game runs that travels while it lies down (bleedout, essential down, knocked down).
// Players saw downed essential NPCs slide along the floor; this says on whose game, and in which state.
// Called with each movement update of the NPC, ten to thirty times a second; one line per NPC at most every five seconds.
void NoteDownedTravel(uint32_t aFormId) noexcept
{
    struct Down
    {
        NiPoint3 From;
        std::chrono::steady_clock::time_point Since;
        std::chrono::steady_clock::time_point SaidAt;
    };
    static TiltedPhoques::Map<uint32_t, Down> s_down;

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    const uint32_t cFlags = pActor ? pActor->actorState.flags1 : 0;
    const uint32_t cLife = ActorState::LifeStateOf(cFlags);
    const uint32_t cKnock = (cFlags >> 25) & 0x7; // KNOCK_STATE_ENUM: 0 normal, anything else on the way down or up
    const bool cDown = pActor && !pActor->actorState.IsDeadOrDying() && (pActor->actorState.IsBleedingOut() || cLife == LifeStates::kUnconscious || cKnock != 0);
    if (!cDown)
    {
        s_down.erase(aFormId);
        return;
    }

    const auto cNow = std::chrono::steady_clock::now();
    auto [it, fresh] = s_down.try_emplace(aFormId, Down{pActor->position, cNow, {}});
    if (fresh || cNow - it->second.Since < 1s)
        return;
    const NiPoint3 cAt = pActor->position;
    const float dx = cAt.x - it->second.From.x, dy = cAt.y - it->second.From.y, dz = cAt.z - it->second.From.z;
    const float cMoved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (cMoved > 30.f && cNow - it->second.SaidAt > 5s)
    {
        it.value().SaidAt = cNow;
        spdlog::debug(
            "DownedTravel: own NPC {:X} moved {:.0f} units in {} ms while down (life state {}, knock state {}), now at ({:.0f})", aFormId, cMoved,
            std::chrono::duration_cast<std::chrono::milliseconds>(cNow - it->second.Since).count(), cLife, cKnock, cAt);
    }
    it.value().From = cAt;
    it.value().Since = cNow;
}

// Whether an actor is in this player's loaded area: it has its 3D and its cell is attached.
bool IsActorLoadedHere(Actor* apActor) noexcept
{
    const TESObjectCELL* pCell = apActor ? apActor->GetParentCellEx() : nullptr;
    return pCell && pCell->IsAttached() && apActor->GetNiNode();
}

// Whether a place is in this player's loaded area: an interior cell attached here, or an exterior cell of the player's
// worldspace within the loaded grid (uGridsToLoad 5, which the server enforces: two cells either side of the centre).
bool IsPlaceLoadedHere(World& aWorld, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) noexcept
{
    if (!acWorldSpaceId)
    {
        const auto* pCell = Cast<TESObjectCELL>(TESForm::GetById(aWorld.GetModSystem().GetGameId(acCellId)));
        return pCell && pCell->IsAttached();
    }

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const TESWorldSpace* pPlayerWorldSpace = pPlayer ? pPlayer->GetWorldSpace() : nullptr;
    const TES* pTES = TES::Get();
    if (!pPlayerWorldSpace || !pTES || pPlayerWorldSpace->formID != aWorld.GetModSystem().GetGameId(acWorldSpaceId))
        return false;

    const GridCellCoords cCoordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
    return std::abs(cCoordinates.X - pTES->centerGridX) <= 2 && std::abs(cCoordinates.Y - pTES->centerGridY) <= 2;
}

// Where an owner has its NPC, by this game's ids, for a copy left away from it (CopyRegistry::NoteAwayFromOwner).
CopyRegistry::OwnerPlace OwnerPlaceOf(World& aWorld, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) noexcept
{
    CopyRegistry::OwnerPlace place{};
    if (acWorldSpaceId)
        place.WorldSpaceId = aWorld.GetModSystem().GetGameId(acWorldSpaceId);
    else
        place.CellId = aWorld.GetModSystem().GetGameId(acCellId);
    place.Position = {acPosition.x, acPosition.y, acPosition.z};
    return place;
}

// A gained NPC carries on from where its last owner's game last had it, not from where its copy is drawn here a moment behind.
// Otherwise the other players see it step back by that delay when this game takes over.
// Only one standing in that same cell: a body, a rider or one knocked down stays where this game has it, and moving it does not change its cell.
void CarryOnFromLastOwner(World& aWorld, Actor* apActor, const NotifyOwnershipTransfer& acMessage) noexcept
{
    if (!acMessage.CellId || apActor->IsDead() || apActor->IsMount() || !InterpolationSystem::OnFoot(apActor) || apActor->actorState.IsBleedingOut() ||
        apActor->actorState.IsKnockedDown())
        return;

    const TESObjectCELL* pCell = apActor->GetParentCellEx();
    if (!pCell || pCell->formID != aWorld.GetModSystem().GetGameId(acMessage.CellId))
        return;

    const glm::vec3 cPosition{acMessage.Position.x, acMessage.Position.y, acMessage.Position.z};
    apActor->ForcePosition(NiPoint3(cPosition));
    apActor->SetRotation(apActor->rotation.x, apActor->rotation.y, acMessage.Heading);
}

// The player's own graph events with no action behind them (PlayerGraphEvents.h), sent as name-only actions.
// Taken while offline too, so that none waits to be sent on the next connection.
void SendPlayerGraphEvents(World& aWorld, const bool aOnline) noexcept
{
    while (const auto cName = AnimationGraphEvents::TakePlayerEvent())
    {
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (!aOnline || !pPlayer)
            continue;

        ActionEvent action;
        action.State1 = pPlayer->actorState.flags1;
        action.State2 = pPlayer->actorState.flags2;
        action.Type = PlayerGraphEvents::kTypeBit;
        action.Tick = aWorld.GetTick();
        action.ActorId = pPlayer->formID;
        action.EventName = TiltedPhoques::String(cName->data(), cName->size());
        pPlayer->SaveAnimationVariables(action.Variables);

        aWorld.GetRunner().Trigger(action);
    }
}

} // namespace

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_referenceAddedConnection = m_dispatcher.sink<ActorAddedEvent>().connect<&CharacterService::OnActorAdded>(this);
    m_referenceRemovedConnection = m_dispatcher.sink<ActorRemovedEvent>().connect<&CharacterService::OnActorRemoved>(this);
    m_inventoryChangesConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&CharacterService::OnNotifyInventoryChanges>(this);

    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this);
    m_actionConnection = m_dispatcher.sink<ActionEvent>().connect<&CharacterService::OnActionEvent>(this);

    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&CharacterService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&CharacterService::OnDisconnected>(this);
    m_raiseOwnerConnections[0] = m_world.on_construct<LocalComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);
    m_raiseOwnerConnections[1] = m_world.on_update<LocalComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);
    m_raiseOwnerConnections[2] = m_world.on_destroy<LocalComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);
    m_raiseOwnerConnections[3] = m_world.on_construct<RemoteComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);
    m_raiseOwnerConnections[4] = m_world.on_update<RemoteComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);
    m_raiseOwnerConnections[5] = m_world.on_destroy<RemoteComponent>().connect<&CharacterService::OnRaiseBodyOwnerChanged>(this);

    m_assignCharacterConnection = m_dispatcher.sink<AssignCharacterResponse>().connect<&CharacterService::OnAssignCharacter>(this);
    m_characterSpawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&CharacterService::OnCharacterSpawn>(this);
    m_referenceMovementSnapshotConnection = m_dispatcher.sink<ServerReferencesMoveRequest>().connect<&CharacterService::OnReferencesMoveRequest>(this);
    m_actionEventsConnection = m_dispatcher.sink<NotifyActionEvents>().connect<&CharacterService::OnNotifyActionEvents>(this);
    m_factionsConnection = m_dispatcher.sink<NotifyFactionsChanges>().connect<&CharacterService::OnFactionsChanges>(this);
    m_ownershipTransferConnection = m_dispatcher.sink<NotifyOwnershipTransfer>().connect<&CharacterService::OnOwnershipTransfer>(this);
    m_removeCharacterConnection = m_dispatcher.sink<NotifyRemoveCharacter>().connect<&CharacterService::OnRemoveCharacter>(this);
    // Another player's word to a follower this game runs, or the answer to this player's (FollowerCommands).
    m_followerCommandConnection = m_dispatcher.sink<NotifyFollowerCommand>().connect<&FollowerCommands::OnNotify>();
    // A dragon died: its soul for this player, once per dragon (DragonSouls).
    m_dragonSoulConnection = m_dispatcher.sink<NotifyDragonSoul>().connect<&DragonSouls::OnNotify>();
    // Arrows stuck in an actor and limbs cut off it in another game, put on the copies here each frame; nothing with their switches off (StuckArrows).
    m_stuckArrowConnection = m_dispatcher.sink<NotifyStuckArrow>().connect<&StuckArrows::OnNotifyStuckArrow>();
    m_dismemberConnection = m_dispatcher.sink<NotifyDismember>().connect<&StuckArrows::OnNotifyDismember>();
    m_stuckArrowUpdateConnection = m_dispatcher.sink<UpdateEvent>().connect<&StuckArrows::OnUpdate>();
    m_clashConnection = m_dispatcher.sink<NotifyClash>().connect<&CharacterService::OnNotifyClash>(this);
    m_arrowNockedConnection = m_dispatcher.sink<NotifyArrowNocked>().connect<&CharacterService::OnNotifyArrowNocked>(this);
    m_projectileLaunchedConnection = m_dispatcher.sink<ProjectileLaunchedEvent>().connect<&CharacterService::OnProjectileLaunched>(this);

    m_initPackageConnection = m_dispatcher.sink<InitPackageEvent>().connect<&CharacterService::OnInitPackageEvent>(this);
    m_newPackageConnection = m_dispatcher.sink<NotifyNewPackage>().connect<&CharacterService::OnNotifyNewPackage>(this);

    m_notifyRespawnConnection = m_dispatcher.sink<NotifyRespawn>().connect<&CharacterService::OnNotifyRespawn>(this);
    m_beastFormChangeConnection = m_dispatcher.sink<BeastFormChangeEvent>().connect<&CharacterService::OnBeastFormChange>(this);

    m_addExperienceEventConnection = m_dispatcher.sink<AddExperienceEvent>().connect<&CharacterService::OnAddExperienceEvent>(this);
    m_syncExperienceConnection = m_dispatcher.sink<NotifySyncExperience>().connect<&CharacterService::OnNotifySyncExperience>(this);

    m_dialogueEventConnection = m_dispatcher.sink<DialogueEvent>().connect<&CharacterService::OnDialogueEvent>(this);
    m_dialogueSyncConnection = m_dispatcher.sink<NotifyDialogue>().connect<&CharacterService::OnNotifyDialogue>(this);

    m_subtitleEventConnection = m_dispatcher.sink<SubtitleEvent>().connect<&CharacterService::OnSubtitleEvent>(this);
    m_subtitleSyncConnection = m_dispatcher.sink<NotifySubtitle>().connect<&CharacterService::OnNotifySubtitle>(this);

    m_actorTeleportConnection = m_dispatcher.sink<NotifyActorTeleport>().connect<&CharacterService::OnNotifyActorTeleport>(this);

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

// A copy just made for this character has this form id, so no other character may still point at it.
// The game reuses a deleted actor's id, and a character whose copy is gone would otherwise drive the new actor.
void CharacterService::ClaimCopyId(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    auto view = m_world.view<RemoteComponent>();
    for (const auto other : view)
    {
        if (other == aEntity)
            continue;
        auto& remoteComponent = view.get<RemoteComponent>(other);
        if (remoteComponent.CachedRefId != aFormId)
            continue;

        spdlog::warn("Copy id {:X} now belongs to server character {:X}; server character {:X} still pointed at it (its own copy is gone) and "
                     "gets a new one", aFormId, m_world.get<RemoteComponent>(aEntity).Id, remoteComponent.Id);
        remoteComponent.CachedRefId = 0;
        if (const auto* pFormId = m_world.try_get<FormIdComponent>(other); pFormId && pFormId->Id == aFormId)
            m_world.remove<FormIdComponent>(other);
    }
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    // What other games did to the actor goes with its copy; a copy made again is told by its spawn data.
    if (const auto* pRemote = m_world.try_get<RemoteComponent>(aEntity))
    {
        const auto* pForm = m_world.try_get<FormIdComponent>(aEntity);
        StuckArrows::OnRetired(pRemote->Id, pForm ? pForm->Id : pRemote->CachedRefId);
    }
    InterpolationSystem::Clean(m_world, aEntity);
    // The player mark goes first: a removed WaitingFor3D is read as "its 3D came" by services that then look up the player's actor
    // (OverlayService, WeatherService), and a copy still waiting for its 3D has no form id component to look it up by.
    m_world.remove<PlayerComponent>(aEntity);
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D>(aEntity);
}

void CharacterService::DeclineOwnership(const uint32_t aServerId, const uint32_t aOwnershipEpoch) const noexcept
{
    RequestOwnershipTransfer request{};
    request.ServerId = aServerId;
    request.OwnershipEpoch = aOwnershipEpoch;
    request.Reason = OwnershipReleaseReason::DeclineGrant;
    m_transport.Send(request);
}

void CharacterService::ReconcileActorData(
    const entt::entity aEntity, Actor* apActor, const uint32_t aOwnershipEpoch, const ActorData& acActorData, const bool aApplyInventory, const bool aIsLocalOwner) noexcept
{
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acActorData.IsDead;
        pWaitingFor3D->SpawnRequest.RevivedSinceDeath = acActorData.RevivedSinceDeath;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acActorData.IsWeaponDrawn;
        pWaitingFor3D->SpawnRequest.OwnershipEpoch = aOwnershipEpoch;
    }

    if (!apActor)
        return;

    // The server's mark, kept for an announcement from here (BuildActorData).
    if (ActorExtension* pExtension = apActor->GetExtension())
        pExtension->RevivedSinceDeath = acActorData.RevivedSinceDeath;

    apActor->SetActorValues(ValuesForCopy(acActorData.InitialActorValues, acActorData.IsDead, apActor));

    if (aApplyInventory)
    {
        // Equipping and unequipping read the actor's AI process: an actor not loaded here has none, and the game crashes on it.
        if (!apActor->HasLoadedProcess())
        {
            PendingInventory pending{acActorData.InitialInventory, apActor, 0, aOwnershipEpoch};
            if (const auto* pRemote = m_world.try_get<RemoteComponent>(aEntity))
                pending.ServerId = pRemote->Id;
            else if (const auto* pLocal = m_world.try_get<LocalComponent>(aEntity))
                pending.ServerId = pLocal->Id;
            m_pendingInventories[apActor->formID] = std::move(pending);
            spdlog::info("Inventory of {:X} kept until the actor is loaded here (no AI process yet)", apActor->formID);
        }
        else
        {
            m_pendingInventories.erase(apActor->formID);
            const Inventory currentInventory = apActor->GetActorInventory();
            if (currentInventory.Entries != acActorData.InitialInventory.Entries || currentInventory.CurrentMagicEquipment != acActorData.InitialInventory.CurrentMagicEquipment)
                apActor->SetActorInventory(acActorData.InitialInventory);
            // What its owner wears now, so a piece taken off later comes off here too (NpcEquipmentCheck::ApplyWornItems).
            NpcEquipmentCheck::NoteOwnerEquipment(apActor, acActorData.InitialInventory);
        }
    }

    if (apActor->IsDead() != acActorData.IsDead)
    {
        if (acActorData.IsDead)
        {
            // A copy still waiting for its 3D is killed by that step, so that it falls.
            // A body this game now owns has no such step left.
            if (aIsLocalOwner || !m_world.any_of<WaitingFor3D>(aEntity))
                apActor->Kill();
        }
        // An owner keeps its local death, and the death sync reports it.
        else if (aIsLocalOwner)
            spdlog::warn("Keeping actor {:X} dead although the server has it alive", apActor->formID);
        else if (IsDeadAgainstOwner(apActor, false, acActorData.RevivedSinceDeath))
        {
            const auto* pRemote = m_world.try_get<RemoteComponent>(aEntity);
            SendKillingBlowToOwner(m_transport, apActor, pRemote ? pRemote->Id : 0);
        }
        else
            apActor->Respawn();
    }

    if (aIsLocalOwner)
    {
        // A remote draw correction may still be queued when an ownership grant arrives.
        m_weaponDrawUpdates.erase(apActor->formID);

        if (apActor->actorState.IsWeaponDrawn() != acActorData.IsWeaponDrawn)
            apActor->SetWeaponDrawnEx(acActorData.IsWeaponDrawn);
    }
    else
        m_weaponDrawUpdates[apActor->formID] = {acActorData.IsWeaponDrawn};
}

// Sends, per kind of item (same base, same extra data), the count difference as an ordinary inventory change.
// The server applies it to its copy and relays it to the other players.
// Used for a follower whose gear another game overwrote on the server while it owned her.
void CharacterService::SendInventoryDifference(Actor* apActor, const uint32_t aServerId, const uint32_t aOwnershipEpoch, const Inventory& acServerInventory) const noexcept
{
    if (!apActor || !m_transport.IsConnected())
        return;

    const Inventory mine = apActor->GetActorInventory();
    const auto countIn = [](const Inventory& acInventory, const Inventory::Entry& acKind)
    {
        int32_t count = 0;
        for (const auto& entry : acInventory.Entries)
            if (entry.CanBeMerged(acKind))
                count += entry.Count;
        return count;
    };

    TiltedPhoques::Vector<Inventory::Entry> done;
    const auto alreadyDone = [&done](const Inventory::Entry& acKind)
    { return std::any_of(done.begin(), done.end(), [&acKind](const Inventory::Entry& acDone) { return acDone.CanBeMerged(acKind); }); };

    int32_t added = 0, removed = 0;
    for (const Inventory* pSide : {&mine, &acServerInventory})
    {
        for (const auto& kind : pSide->Entries)
        {
            if (alreadyDone(kind))
                continue;
            done.push_back(kind);
            const int32_t difference = countIn(mine, kind) - countIn(acServerInventory, kind);
            if (difference == 0)
                continue;

            RequestInventoryChanges request;
            request.ServerId = aServerId;
            request.OwnershipEpoch = aOwnershipEpoch;
            request.Item = kind;
            request.Item.Count = difference;
            request.Drop = false;
            request.UpdateClients = true;
            m_transport.Send(request);
            if (difference > 0)
                ++added;
            else
                ++removed;
        }
    }

    spdlog::info("Follower {:X}: the server's copy of her inventory put right ({} kinds of item added there, {} taken away)", apActor->formID, added, removed);
}

bool CharacterService::RequestOwnership(
    const uint32_t aFormId, const uint32_t aServerId, const entt::entity aEntity, const bool aKillingBlow, const bool aForHolding, const bool aForCrime) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::warn("Cannot request ownership of actor {:X} because its form {:X} is unavailable", aServerId, aFormId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::warn("Cannot request ownership of remote player actor {:X}", aServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::warn("Cannot request ownership of remote player summon {:X}", aServerId);
        return false;
    }

    const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity);
    if (!pRemoteComponent || pRemoteComponent->Id != aServerId || pRemoteComponent->OwnershipEpoch == 0)
        return false;

    RequestOwnershipClaim request;
    request.ServerId = aServerId;
    request.ExpectedOwnershipEpoch = pRemoteComponent->OwnershipEpoch;
    // The server lets a player claim their own follower whoever leads the party (see CanClaimOwnership).
    // A body is nobody's follower: it goes by the rules for bodies, flag or no flag.
    request.Follower = pActor->IsPlayerFollower() && !pActor->IsDead();
    request.KillingBlow = aKillingBlow;
    request.ForHolding = aForHolding && !pActor->IsDead();
    request.ForCrime = aForCrime;

    if (!m_transport.Send(request))
        return false;

    if (request.ForHolding)
        m_holdClaims[aServerId] = GrabHold::Claim{std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()};
    return true;
}

void CharacterService::RunHoldClaims() noexcept
{
    if (m_holdClaims.empty())
        return;

    const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto it = m_holdClaims.begin(); it != m_holdClaims.end();)
    {
        const auto cEntity = Utils::FindEntityByServerId(it->first);
        const auto* pLocal = cEntity ? m_world.try_get<LocalComponent>(*cEntity) : nullptr;
        const auto* pFormId = cEntity ? m_world.try_get<FormIdComponent>(*cEntity) : nullptr;
        Actor* pActor = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        const auto cStep = GrabHold::StepClaim(it.value(), cNow, pActor != nullptr, pLocal != nullptr, pActor && pActor->IsDead(), pActor && pActor->actorState.IsInKnockRagdoll());
        if (cStep == GrabHold::Step::Wait)
        {
            ++it;
            continue;
        }
        if (cStep == GrabHold::Step::End)
        {
            RequestOwnershipTransfer request{};
            request.ServerId = it->first;
            request.OwnershipEpoch = pLocal->OwnershipEpoch;
            request.Reason = OwnershipReleaseReason::HoldEnded;
            m_transport.Send(request);
            spdlog::debug("Hold: {:X} (server id {:X}) is {}; the hold ends", pFormId->Id, it->first, pActor->IsDead() ? "dead" : "up again");
        }
        it = m_holdClaims.erase(it);
    }
}

bool CharacterService::HoldBackTalk(Actor* apNpc) noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = apNpc ? Utils::FindByFormId(view, apNpc->formID) : std::nullopt;
    const ActorExtension* pExtension = apNpc ? apNpc->GetExtension() : nullptr;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();

    DialogueClaim::Talk talk{};
    talk.Connected = m_transport.IsConnected();
    talk.InParty = m_world.GetPartyService().IsInParty();
    talk.Remote = it.has_value() && pExtension && pExtension->IsRemote();
    talk.RemotePlayer = pExtension && pExtension->IsRemotePlayer();
    talk.Dead = apNpc && apNpc->IsDead();
    talk.Mount = apNpc && apNpc->IsMount();
    talk.InCombat = apNpc && apNpc->IsInCombat();
    talk.PlayerSneaking = pPlayer && SneakState::Of(pPlayer->actorState.flags1);
    talk.OthersFollower = pExtension && pExtension->IsOthersFollower();
    const BGSScene* pScene = apNpc ? apNpc->GetCurrentScene() : nullptr;
    talk.InScene = pScene && pScene->isPlaying;
    talk.HasEpoch = it.has_value() && view.get<RemoteComponent>(*it).OwnershipEpoch != 0;
    // What the server would refuse anyway goes to the copy at once, not after the wait.
    talk.Temporary = apNpc && apNpc->IsTemporary();
    talk.FlyingDragon = apNpc && apNpc->IsDragon() && apNpc->actorState.IsFlying();
    if (it.has_value())
    {
        const auto cRest = m_talkRests.find(view.get<RemoteComponent>(*it).Id);
        talk.Resting = cRest != m_talkRests.end() && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() < cRest->second;
    }
    if (!DialogueClaim::HoldsBackActivation(talk))
        return false;

    const auto& remote = view.get<RemoteComponent>(*it);
    // Pressed again while the claim is on its way: still held back.
    if (m_talkClaims.contains(remote.Id))
        return true;

    // The server's hold claim: a party member runs another member's living NPC for a while, and hands it back when done (GrabHold).
    RequestOwnershipClaim request;
    request.ServerId = remote.Id;
    request.ExpectedOwnershipEpoch = remote.OwnershipEpoch;
    request.ForHolding = true;
    if (!m_transport.Send(request))
        return false;

    m_talkClaims[remote.Id] = DialogueClaim::Claim{std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()};
    spdlog::info("Talk: {:X} (server id {:X}) runs in another game; asked for it so the conversation runs here", apNpc->formID, remote.Id);
    return true;
}

void CharacterService::RunTalkClaims() noexcept
{
    const double cNow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto it = m_talkRests.begin(); it != m_talkRests.end();)
        it = cNow >= it->second ? m_talkRests.erase(it) : std::next(it);
    if (m_talkClaims.empty())
        return;

    // Farther than this the player has walked off since pressing, and is not drawn into a conversation.
    constexpr float cTalkReachSquared = 400.f * 400.f;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    for (auto it = m_talkClaims.begin(); it != m_talkClaims.end();)
    {
        const auto cEntity = Utils::FindEntityByServerId(it->first);
        const auto* pLocal = cEntity ? m_world.try_get<LocalComponent>(*cEntity) : nullptr;
        const auto* pFormId = cEntity ? m_world.try_get<FormIdComponent>(*cEntity) : nullptr;
        Actor* pActor = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        const bool cExists = pActor && pPlayer && !pActor->IsDead();
        const bool cTalking = pActor && MenuTopicManager::IsPlayerDialogueSpeaker(pActor);
        const auto cStep = DialogueClaim::StepClaim(it.value(), cNow, cExists, pLocal != nullptr, pActor && pActor->HasLoadedProcess(), cTalking);

        if (cStep == DialogueClaim::Step::Activate || cStep == DialogueClaim::Step::ActivateCopy)
        {
            const glm::vec3 cApart = static_cast<glm::vec3>(pActor->position) - static_cast<glm::vec3>(pPlayer->position);
            if (glm::dot(cApart, cApart) <= cTalkReachSquared)
            {
                spdlog::info(
                    "Talk: {:X} (server id {:X}) {}", pActor->formID, it->first,
                    cStep == DialogueClaim::Step::Activate ? "runs here now; the conversation opens" : "was not handed over in time; talking to the copy");
                DialogueClaim::ScopedActivatingToTalk activatingToTalk;
                pActor->Activate(pPlayer, 0, nullptr, 1, false);
            }
            else
                spdlog::info("Talk: {:X} (server id {:X}) not talked to: the player walked off", pActor->formID, it->first);
            ++it;
            continue;
        }
        if (cStep == DialogueClaim::Step::Wait)
        {
            ++it;
            continue;
        }
        if (cStep == DialogueClaim::Step::End && pLocal)
        {
            RequestOwnershipTransfer request{};
            request.ServerId = it->first;
            request.OwnershipEpoch = pLocal->OwnershipEpoch;
            request.Reason = OwnershipReleaseReason::HoldEnded;
            m_transport.Send(request);
            spdlog::info("Talk: {:X} (server id {:X}) handed back, the conversation is over", pFormId->Id, it->first);
        }
        // The server took it back mid-talk, its hold having run too long: it refuses this player's claims for a while.
        if (DialogueClaim::StartsRest(it.value(), cExists, pLocal != nullptr))
        {
            m_talkRests[it->first] = cNow + DialogueClaim::kRestSeconds;
            spdlog::info("Talk: {:X} (server id {:X}) went back during the talk; talked to as a copy for {} s", pActor->formID, it->first, DialogueClaim::kRestSeconds);
        }
        it = m_talkClaims.erase(it);
    }
}

void CharacterService::ClaimForKillingBlow(const uint32_t aFormId) noexcept
{
    constexpr auto cKillWithin = std::chrono::seconds(3);

    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = Utils::FindByFormId(view, aFormId);
    if (!it)
        return;

    const uint32_t cServerId = view.get<RemoteComponent>(*it).Id;
    const auto cNow = std::chrono::steady_clock::now();
    if (const auto pending = m_killingBlows.find(cServerId); pending != m_killingBlows.end() && cNow < pending->second)
        return;

    if (!RequestOwnership(aFormId, cServerId, *it, true))
        return;
    m_killingBlows[cServerId] = cNow + cKillWithin;
    spdlog::info("KillingBlow: {:X} (server id {:X}) is protected, so its owner's game cannot let this player kill it; asked for it, to kill it here", aFormId, cServerId);
}

bool CharacterService::ClaimToRaise(Actor* apBody, const MagicTarget::AddTargetData& acData) noexcept
{
    if (!apBody || !acData.pEffectItem || !acData.pCaster || !acData.pSpell)
        return false;
    const ActorExtension* pBodyExtension = apBody->GetExtension();
    RaiseClaim::Attempt cast{};
    cast.Connected = m_transport.IsConnected();
    cast.CasterIsPlayer = acData.pCaster == PlayerCharacter::Get();
    cast.TargetIsCopy = pBodyExtension && pBodyExtension->IsRemote() && !pBodyExtension->IsPlayer();
    cast.Reanimate = acData.pEffectItem->IsReanimateEffect();
    cast.Dead = apBody->IsDead();
    cast.ToOwner = t_sendingRaiseToOwner;
    if (!RaiseClaim::AskFirst(cast))
        return false;

    // Kept by what names it, and looked up again where it is made: the effect is the spell's, and the spell may be gone by then.
    const auto& cEffects = acData.pSpell->listOfEffects;
    uint32_t effectIndex = 0;
    while (effectIndex < cEffects.length && cEffects[effectIndex] != acData.pEffectItem)
        ++effectIndex;
    if (effectIndex == cEffects.length)
        return false;
    RaiseCast raiseCast{};
    raiseCast.SpellId = acData.pSpell->formID;
    raiseCast.EffectIndex = effectIndex;
    raiseCast.SourceId = acData.pSource ? acData.pSource->formID : 0;
    raiseCast.ExplosionLocation = acData.ExplosionLocation;
    raiseCast.Magnitude = acData.fMagnitude;
    raiseCast.UnkFloat1 = acData.fUnkFloat1;
    raiseCast.CastingSource = acData.eCastingSource;
    raiseCast.AreaTarget = acData.bAreaTarget;
    raiseCast.DualCast = acData.bDualCast;
    const RaiseClaim::Target cBody{0, apBody->GetHandle().handle.iBits, s_raises.Session()};

    // The effect may be made off the main thread: the body is asked for with the next update.
    m_world.GetRunner().Queue([this, cFormId = apBody->formID, cBody, raiseCast] { AskToRaise(cFormId, cBody, raiseCast); });
    return true;
}

void CharacterService::AskToRaise(const uint32_t aFormId, const RaiseClaim::Target& acBody, const RaiseCast& acCast) noexcept
{
    const auto cNow = RaiseClaim::Clock::now();
    // The hook's reference first: a copy found under the form id alone may be one that replaced it.
    const auto* pBody = Cast<Actor>(TESObjectREFR::GetByHandle(acBody.Handle));
    const bool cSameBody = pBody && pBody->formID == aFormId;
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = cSameBody ? Utils::FindByFormId(view, aFormId) : std::optional<entt::entity>{};
    RaiseClaim::Target body = acBody;
    body.ServerId = it ? view.get<RemoteComponent>(*it).Id : 0;

    if (RaiseClaim::MayAsk(cSameBody, it.has_value()))
    {
        // Asked already: the newest effect is the one made.
        if (s_raises.Renew(body, cNow, acCast))
            return;
        if (body.Session == s_raises.Session() && RequestOwnership(aFormId, body.ServerId, *it) && s_raises.Wait(body, cNow, acCast))
        {
            spdlog::debug("Raise: {:X} (server id {:X}) is raised by this player; asked for its body, to raise it here", aFormId, body.ServerId);
            return;
        }
    }

    // Not to be asked for: sent to the game that runs the body.
    s_raises.NotAskedFor(body, cSameBody && pBody->IsDead(), cNow, acCast);
}

void CharacterService::RunRaisings() noexcept
{
    if (!s_raises.Waits())
        return;
    // Not handed over in time: sent to the game that runs the body, which raises it there.
    s_raises.Expire(
        RaiseClaim::Clock::now(),
        [](const uint32_t aServerId)
        {
            const Actor* pBody = Utils::GetByServerId<Actor>(aServerId);
            return pBody && pBody->IsDead();
        });
}

void CharacterService::OnRaiseBodyOwnerChanged(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    if (const auto* pRemote = aRegistry.try_get<RemoteComponent>(aEntity))
        s_raises.Withdraw(pRemote->Id);
    if (const auto* pLocal = aRegistry.try_get<LocalComponent>(aEntity))
        s_raises.Withdraw(pLocal->Id);
}

void CharacterService::MakeDecidedRaises() noexcept
{
    auto raises = s_raises.Take();
    if (raises.empty())
        return;
    const bool cLoading = IsProcessExiting() || LoadingScreenUp();
    for (size_t i = 0; i < raises.size(); ++i)
    {
        const auto& cRaise = raises[i];
        MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(cRaise.Cast.SpellId));
        EffectItem* pEffect = pSpell && cRaise.Cast.EffectIndex < pSpell->listOfEffects.length ? pSpell->listOfEffects[cRaise.Cast.EffectIndex] : nullptr;
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (!pEffect || !pEffect->IsReanimateEffect() || !pPlayer)
            continue;

        // By handle, and held through the effect: the game reuses a freed form id, and a copy can be replaced by another under the same
        // server id. Checked last before the effect; a change of hands after that is one after the raise.
        const RetainedReference cBody(cLoading ? 0 : cRaise.Body.Handle);
        Actor* pBody = Cast<Actor>(cBody.Get());
        const ActorExtension* pExtension = pBody && !pBody->IsDeleted() ? pBody->GetExtension() : nullptr;
        RaiseClaim::Found found{};
        found.Session = s_raises.Session();
        found.SameBody = pExtension != nullptr;
        found.Here = pExtension && pExtension->IsLocal();
        found.Dead = pExtension && pBody->IsDead();
        found.Loading = cLoading;
        found.Decided = s_raises.Decided(i);
        if (!RaiseClaim::StillToMake(cRaise, found, RaiseClaim::Clock::now()))
        {
            spdlog::debug("Raise: the raise of server id {:X} is no longer for the body it was decided for; not made", cRaise.Body.ServerId);
            continue;
        }
        MagicTarget::AddTargetData data{};
        data.pCaster = pPlayer;
        data.pSpell = pSpell;
        data.pEffectItem = pEffect;
        data.pSource = cRaise.Cast.SourceId ? Cast<TESBoundObject>(TESForm::GetById(cRaise.Cast.SourceId)) : nullptr;
        data.ExplosionLocation = cRaise.Cast.ExplosionLocation;
        data.fMagnitude = cRaise.Cast.Magnitude;
        data.fUnkFloat1 = cRaise.Cast.UnkFloat1;
        data.eCastingSource = cRaise.Cast.CastingSource;
        data.bAreaTarget = cRaise.Cast.AreaTarget;
        data.bDualCast = cRaise.Cast.DualCast;

        // On a copy the hook sends the raise to the game that runs the body (MagicTarget::IsRaiseOnCopy); it is not held for the body again.
        t_sendingRaiseToOwner = cRaise.Make == RaiseClaim::Then::ToOwner;
        const bool cRaised = pBody->magicTarget.AddTargetAsGame(data);
        t_sendingRaiseToOwner = false;
        spdlog::debug(
            "Raise: {:X} (server id {:X}) raised by this player {}: {}", pBody->formID, cRaise.Body.ServerId,
            cRaise.Make == RaiseClaim::Then::RaiseHere ? "here" : "in the game that runs it", cRaised);
    }
}

// A revive does not take on an actor switched off, or one still without its 3D.
static bool CanTakeRevival(Actor* apActor) noexcept
{
    return !apActor->IsDisabled() && apActor->GetNiNode() && !apActor->IsStillLoading();
}

void CharacterService::ReviveActor(const uint32_t aFormId) noexcept
{
    constexpr auto cReviveWithin = std::chrono::seconds(3);

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor || !pActor->IsDead())
        return;

    if (!CanTakeRevival(pActor))
    {
        if (std::find(s_revivalsOnSwitchOn.begin(), s_revivalsOnSwitchOn.end(), aFormId) == s_revivalsOnSwitchOn.end())
            s_revivalsOnSwitchOn.push_back(aFormId);
        return;
    }

    // Revived here directly: this game's own actor, whose word reaches every game, or a body dead only here.
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto it = Utils::FindByFormId(view, aFormId);
    if (!it || !view.get<RemoteComponent>(*it).DeadAtOwner)
    {
        pActor->Respawn();
        return;
    }

    const uint32_t cServerId = view.get<RemoteComponent>(*it).Id;
    if (!RequestOwnership(aFormId, cServerId, *it))
        return;
    m_revivals[cServerId] = std::chrono::steady_clock::now() + cReviveWithin;
    spdlog::info("Revive: {:X} (server id {:X}) is another game's, and only its owner's word reaches every game; asked for it, to revive it here", aFormId, cServerId);
}

void CharacterService::RunRevivalsOnSwitchOn() noexcept
{
    std::erase_if(s_revivalsOnSwitchOn, [this](const uint32_t aFormId)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
        if (!pActor || !pActor->IsDead())
            return true;
        if (!CanTakeRevival(pActor))
            return false;

        spdlog::info("Revive: {:X} is switched on, and revived as it asked to be", aFormId);
        ReviveActor(aFormId);
        return true;
    });
}

void CharacterService::OnGameLoaded() noexcept
{
    s_pendingLines.clear();
    s_sceneLines.clear();
    s_silentScenes.clear();
    s_revivalsOnSwitchOn.clear();
    // A body moved, stood up or waiting for it in the world before the load is not in this one.
    InterpolationSystem::ForgetCopyBodies();
    s_ghostsToLayDown.clear();

    const auto forget = [](const uint32_t aFormId)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
        if (ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr)
            pExtension->SetHiddenHere(false);
    };

    for (const auto& [ghost, copy] : CopyRegistry::Ghosts())
        forget(ghost);
    for (const PendingGhostEnable& acPending : s_pendingGhostEnables)
        forget(acPending.FormId);
    s_pendingGhostEnables.clear();
}

/**
 * Switch off the copies queued since the last frame (FrameStartSwitchOff): removed copies, and copies whose ragdoll falls away under the ground (FallStop).
 * Called on the main thread at the start of a frame.
 */
void CharacterService::DisableQueuedCopies() noexcept
{
    SwitchOffQueuedGhosts();

    std::vector<uint32_t> handles = FrameStartSwitchOff::TakeQueued();
    if (handles.empty() || IsProcessExiting())
        return;

    // A copy can be queued twice in a frame (removed by discovery, then by the server); its disable has not landed yet the second time.
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());

    for (const uint32_t cHandle : handles)
    {
        // By handle: the game reuses a freed form id.
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(cHandle));
        if (!pActor || pActor->IsDeleted() || !pActor->IsTemporary() || pActor->IsDisabled())
            continue;
        // One without 3D may be in the middle of being disposed of by the game, which disabling crashes; it waits unseen until it is freed.
        if (!pActor->GetNiNode() || pActor->IsStillLoading())
        {
            spdlog::info("CopyOff: {:X} left as it is: it has no 3D, so the game may be disposing of it", pActor->formID);
            continue;
        }
        LogCopyLife("switched off", pActor->formID, pActor);
        pActor->Disable();
#ifdef SKYRIMVR
        const char* cpLanded = pActor->IsDisabled() ? "disabled at once" : "the disable not landed yet";
        if (const uint32_t cThread = GetCurrentThreadId(); cThread == GameMainThreadId())
            spdlog::info("CopyOff: {:X} switched off at the start of a frame, on the main thread; {}", pActor->formID, cpLanded);
        else
            spdlog::info("CopyOff: {:X} switched off at the start of a frame, on thread {}; {}", pActor->formID, cThread, cpLanded);
#endif
    }
}

void CharacterService::FollowOwnersLife(const uint32_t aFormId) noexcept
{
    s_lifeFollowsOwner.insert(aFormId);
}

void CharacterService::LeaderSwitchedOn(const uint32_t aFormId) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    for (auto entry = s_leaderSwitchedOnAt.begin(); entry != s_leaderSwitchedOnAt.end();)
        entry = cNow - entry->second > 1min ? s_leaderSwitchedOnAt.erase(entry) : std::next(entry);
    s_leaderSwitchedOnAt[aFormId] = cNow;
    AnnounceAgain(aFormId);
}

void CharacterService::AnnounceAgain(const uint32_t aFormId) noexcept
{
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    const auto it = Utils::FindByFormId(view, aFormId);
    if (it && std::find(s_notAnnounced.begin(), s_notAnnounced.end(), *it) == s_notAnnounced.end())
        s_notAnnounced.push_back(*it);
}

// What is kept by form id about the copy on that id, and the reference it stood in for: let go of when its removal is asked for.
// Done at once even when the removal itself waits for a load to end: by then the game may have given the id to a new copy.
// apActor is the actor itself, or null when nothing has the id now.
static void ForgetCopyRecords(const uint32_t aFormId, Actor* apActor) noexcept
{
    s_followerCopies.erase(aFormId);
    CopyRegistry::ForgetAwayFromOwner(aFormId);
    if (!apActor || apActor->IsTemporary())
        s_playerCopies.Forget(aFormId, apActor ? apActor->GetHandle().handle.iBits : 0);
    // Taken off its horse first, MountService deletes it after that (ReleaseThenRetire).
    if (apActor && IsReleasing(apActor))
        return;
    // What it stood in for is not left hidden.
    ReleaseGhostOf(aFormId, apActor && apActor->IsTemporary() && IsDeadCopy(apActor));
}

// Removes the copy on that id: apActor is the actor itself, or null when nothing (or another actor) has the id now.
// aRecordsDone: ForgetCopyRecords ran already, when the removal was asked for.
static void RemoveCopy(const uint32_t aFormId, Actor* apActor, const bool aRecordsDone) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pActor = apActor;
    // Taken off its horse first, MountService deletes it after that (ReleaseThenRetire).
    if (pActor && IsReleasing(pActor))
    {
        if (!aRecordsDone)
            ForgetCopyRecords(aFormId, pActor);
        return;
    }
    const bool cDead = pActor && pActor->IsTemporary() && IsDeadCopy(pActor);
    if (!aRecordsDone)
    {
        s_followerCopies.erase(aFormId);
        CopyRegistry::ForgetAwayFromOwner(aFormId);
        if (!pActor || pActor->IsTemporary())
            s_playerCopies.Forget(aFormId, pActor ? pActor->GetHandle().handle.iBits : 0);
    }
    // Gone already: what it stood in for is not left hidden.
    if (!pActor && !aRecordsDone)
        ReleaseGhostOf(aFormId, false);
    if (pActor && pActor->IsTemporary())
    {
        const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
        const uint32_t handles = refWord & 0x3FF;

        // Freeing a copy the game still holds is a use-after-free.
        // It is disabled and waits like a copy whose cell unloaded, until the game lets go of it (RunWaitingCopies).
        if (CopyRemovalPolicy::OnRemoved(handles, s_waitingCopies.size()) == CopyRemovalPolicy::Action::WaitDisabled)
        {
            LogCopyLife("held (removed, to be switched off)", aFormId, pActor);
            spdlog::info("Temporary Remote Held {:X}: {} handle(s) still on it, disabled and queued ({} of {} slots used); the server removed it",
                         aFormId, handles, s_waitingCopies.size() + 1, CopyRemovalPolicy::kMaxWaiting);
            FrameStartSwitchOff::Queue(pActor);
            s_waitingCopies.push_back(WaitingCopy{aFormId, pActor->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
            if (!aRecordsDone)
                ReleaseGhostOf(aFormId, cDead);
            return;
        }
        if (handles != 0)
            spdlog::warn("CopyFreedHeld: {:X} freed immediately with {} handle(s) still outstanding -- the waiting list was full ({} slots)", aFormId, handles,
                         CopyRemovalPolicy::kMaxWaiting);

        RecentDeletes::Record(pActor, aFormId, handles, refWord, RecentDeletes::kOnServersWord);
        LogCopyLife("deleted (removed)", aFormId, pActor);
        pActor->Delete();
        CopyRegistry::ForgetOwnCopy(aFormId); // the game may give this id to an actor of its own next
        spdlog::info("\tDeleted actor {:X}", aFormId);
        if (!aRecordsDone)
            ReleaseGhostOf(aFormId, cDead);
    }
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    if (IsProcessExiting())
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    // Not while a load is up or just over: kept, never dropped, and done once it is over, on the same actor by its handle.
    // What is kept by id goes now, as the game may give the id to a new copy before the load is over.
    if (!CopyWorkSettled(s_removalLoadGate))
    {
        if (CopyLifetime::QueueRemoval(s_removalsAfterLoad, aFormId, pActor ? pActor->GetHandle().handle.iBits : 0))
        {
            ForgetCopyRecords(aFormId, pActor);
            spdlog::info("CopyLater: removal of {:X} asked for while a load is up or just over; done once it is over ({} kept)", aFormId, s_removalsAfterLoad.size());
        }
        return;
    }
    RemoveCopy(aFormId, pActor, false);
}

// Does the removals kept over a load, once it is over.
static void RunRemovalsAfterLoad() noexcept
{
    // Asked every pass, so that the gate follows the loads as they come, not only when a removal does.
    if (!CopyWorkSettled(s_removalLoadGate) || s_removalsAfterLoad.empty())
        return;

    TiltedPhoques::Vector<CopyLifetime::LaterRemoval> removals;
    removals.swap(s_removalsAfterLoad);
    for (const CopyLifetime::LaterRemoval& acRemoval : removals)
    {
        // By handle: the game reuses a freed form id, and an id lookup would then remove the actor that has it now.
        Actor* pActor = acRemoval.Handle ? Cast<Actor>(TESObjectREFR::GetByHandle(acRemoval.Handle)) : nullptr;
        if (!CopyLifetime::IsSameActorAfterLoad(acRemoval, pActor != nullptr, pActor ? pActor->formID : 0, pActor && pActor->IsDeleted()))
            pActor = nullptr;
        spdlog::info("CopyLater: removal of {:X} done now that the load is over{}", acRemoval.FormId, pActor ? "" : "; the copy is gone already, only its records go");
        RemoveCopy(acRemoval.FormId, pActor, true);
    }
}

// An actor without a cell when it was found is announced once it has one: discovery raises an actor only once.
void CharacterService::RunNotAnnounced() const noexcept
{
    if (s_notAnnounced.empty())
        return;

    static std::chrono::steady_clock::time_point s_next;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_next)
        return;
    s_next = cNow + 1s;

    TiltedPhoques::Vector<entt::entity> entities;
    entities.swap(s_notAnnounced);
    if (!m_transport.IsOnline())
        return;

    for (const entt::entity cEntity : entities)
    {
        if (!m_world.valid(cEntity) || !m_world.all_of<FormIdComponent>(cEntity) || m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(cEntity))
            continue;

        const Actor* pActor = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(cEntity).Id));
        if (!pActor)
            continue;

        const bool cWaitedForLeader = s_leaderSwitchedOnAt.contains(pActor->formID);
        if (WaitsForLeader(pActor, m_world.GetPartyService().IsLeader()) || !pActor->parentCell)
        {
            s_notAnnounced.push_back(cEntity);
            continue;
        }

        if (!cWaitedForLeader)
            spdlog::info("Actor {:X} has a cell now; announced to the server", pActor->formID);
        ProcessNewEntity(cEntity);
    }
}

void CharacterService::OnActorAdded(const ActorAddedEvent& acEvent) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));

    if (acEvent.FormId == 0x14)
    {
        pActor->GetExtension()->SetPlayer(true);
    }

    entt::entity entity;

    const auto view = m_world.view<RemoteComponent>();
    // Matches the id and, for a copy this client made, the base too: the game reuses ids (see ClaimCopyId).
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view, pActor](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            if (remoteComponent.CachedRefId != acEvent.FormId)
                return false;
            if (remoteComponent.CachedBaseId != 0 && (!pActor || !pActor->baseForm || pActor->baseForm->formID != remoteComponent.CachedBaseId))
                return false;
            return true;
        });

    if (it != std::end(view))
    {
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            pActor->GetExtension()->SetRemote(true);

        entity = *it;
    }
    else
        entity = m_world.create();

    m_world.emplace_or_replace<FormIdComponent>(entity, acEvent.FormId);
    m_world.emplace_or_replace<EarlyAnimationBufferComponent>(entity);

    ProcessNewEntity(entity);
}

void CharacterService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    if (Actor* pActor = acEvent.pActor)
    {
        // GetExtension() returns null for an actor this client did not allocate, and this runs during the game's teardown.
        if (ActorExtension* pExtension = pActor->GetExtension())
            pExtension->Reconciliation = ActorExtension::ReconciliationStage::None;
    }

    m_pendingLeveledConforms.erase(acEvent.FormId);
    m_conformEquipment.erase(acEvent.FormId);
    InterpolationSystem::Forget(acEvent.FormId);

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    const auto entityIt = Utils::FindByFormId(view, acEvent.FormId);

    if (!entityIt)
    {
        // Not an error: the game removes many actors this client never tracked, dozens at once on a cell change.
        spdlog::debug("Actor removed that was not tracked here: {:X}", acEvent.FormId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id, acEvent.pActor);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    VRBodySync::ClearRemotePose(acEvent.FormId);

    spdlog::info("Actor removed, form id: {:X}", acEvent.FormId);
}

void CharacterService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
#ifdef SKYRIMVR
    {
        PerfScope perfScope("CharacterService::SendClashes");
        SendClashes();
    }
#endif
    {
        PerfScope perfScope("CharacterService::RunSpawnUpdates");
        RunSpawnUpdates();
    }
    SendPlayerGraphEvents(m_world, m_transport.IsOnline());
    {
        PerfScope perfScope("CharacterService::RunLocalUpdates");
        RunLocalUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunEnemyMeterUpdates");
        RunEnemyMeterUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunRemotePlayerDiag");
        RunRemotePlayerDiag();
        RunOrphanedRemoteDiag();
    }
    {
        PerfScope perfScope("CharacterService::RunWeaponTouch");
        RunWeaponTouch();
    }
    RunRaisings();
    {
        PerfScope perfScope("CharacterService::RunCopyChecks");
        RunRemovalsAfterLoad();
        RunWaitingCopies();
        RunReleasingCopies();
        RunNotAnnounced();
        RunRevivalsOnSwitchOn();
        RunGhostChecks();
        SpawnReveal::Tick();
        RunFollowerReclaims();
        RunLeaderClaims();
        RunHoldClaims();
        RunTalkClaims();
    }
    {
        PerfScope perfScope("CharacterService::RunFollowerReports");
        RunFollowerReports();
    }
    {
        PerfScope perfScope("CharacterService::RunFollowerSweeps");
        FollowerCommands::Sweep();
        DragonSouls::Update();
        RunReleasedGhosts();
        RunPendingLines();
        RunSceneLines();
    }
    {
        PerfScope perfScope("Takeover::Run");
        Takeover::Run(m_world);
    }

    {
        PerfScope perfScope("CharacterService::RunFactionsUpdates");
        RunFactionsUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunRemoteUpdates");
        RunRemoteUpdates();
    }
    {
        PerfScope perfScope("CharacterService::RunExperienceUpdates");
        RunExperienceUpdates();
    }
    {
        PerfScope perfScope("CharacterService::ApplyCachedWeaponDraws");
        ApplyCachedWeaponDraws(acUpdateEvent);
        ApplyPendingInventories();
    }
    {
        PerfScope perfScope("CharacterService::ProcessLeveledConforms");
        ProcessLeveledConforms();
    }
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept
{
    // Go through all the forms that were previously detected
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);

        // A copy from the previous connection: the server sends its character again in a moment, so this one goes.
        // The game has been running it since the disconnect, so it is freed like a copy the server removes.
        if (const auto leftover = s_lastConnectionCopies.find(formIdComponent.Id); leftover != s_lastConnectionCopies.end())
        {
            const uint32_t cBaseId = leftover->second;
            s_lastConnectionCopies.erase(leftover);

            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor && !pActor->IsDeleted() && pActor->IsTemporary() && pActor->baseForm && pActor->baseForm->formID == cBaseId)
            {
                // Held since the disconnect, it is freed by its wait already.
                if (FindWaitingCopy(pActor))
                    continue;
                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                const uint32_t handles = refWord & 0x3FF;
                if (CopyRemovalPolicy::OnRemoved(handles, s_waitingCopies.size()) == CopyRemovalPolicy::Action::WaitDisabled)
                {
                    spdlog::info("Leftover copy {:X} from the last connection: {} handle(s) still on it, disabled and queued ({} of {} slots used)",
                                 formIdComponent.Id, handles, s_waitingCopies.size() + 1, CopyRemovalPolicy::kMaxWaiting);
                    FrameStartSwitchOff::Queue(pActor);
                    s_waitingCopies.push_back(WaitingCopy{formIdComponent.Id, pActor->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
                }
                else
                {
                    spdlog::info("Leftover copy {:X} from the last connection deleted on connect", formIdComponent.Id);
                    RecentDeletes::Record(pActor, formIdComponent.Id, handles, refWord, RecentDeletes::kOnConnect);
                    LogCopyLife("deleted (left from the last connection)", formIdComponent.Id, pActor);
                    pActor->Delete();
                }
                continue;
            }
        }

        // Only this client's own copies are deleted; a temporary actor the game made itself is kept and announced.
        // Deleting a game actor while its AI still runs crashes the game (SkyrimVR.exe+0x683850). See CopyRegistry.
        if (TESForm::IsTemporaryId(formIdComponent.Id) && !CopyRegistry::IsOwnCopy(formIdComponent.Id))
        {
            spdlog::info("Temporary actor {:X} was made by the game, not by this client; kept on connect", formIdComponent.Id);
            ProcessNewEntity(entity);
            continue;
        }

        if (TESForm::IsTemporaryId(formIdComponent.Id))
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor)
            {
                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                RecentDeletes::Record(pActor, formIdComponent.Id, refWord & 0x3FF, refWord, RecentDeletes::kOnConnect);
                LogCopyLife("deleted (on connect)", formIdComponent.Id, pActor);
                pActor->Delete();
            }
            CopyRegistry::ForgetOwnCopy(formIdComponent.Id);

            continue;
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) const noexcept
{
    s_raises.Clear();
    FollowerCommands::Reset();
    StuckArrows::Reset();

    // The disconnect that comes with quitting arrives while the game is tearing itself down.
    // Touching actors then fires equip events into plugins whose singletons are gone (e.g. Enchantment Art Extender).
    // The process is ending, so the actors are left as they are.
    if (IsProcessExiting())
    {
        spdlog::info("Disconnected while the game is exiting; actors are left alone");
        return;
    }

    DragonSouls::Reset();
    InterpolationSystem::ResetPoseSession(m_world);
    s_followerReports.clear();
    s_playerCopies.Clear();
    CopyRegistry::ForgetAllAwayFromOwner();

    // A ghost whose stand-in is a body comes back as a body, connected or not: the creature died, and this game's save keeps what it is.
    // Its lay-down is kept over the disconnect (a load of another save forgets it), except for one whose life followed its owner's game.
    std::erase_if(s_ghostsToLayDown, [](const GhostToLayDown& acEntry) { return s_lifeFollowsOwner.contains(acEntry.FormId); });
    // A copy dead at its owner may not be dead here yet: one without its 3D has no FormIdComponent, and its id is on the RemoteComponent.
    const auto formView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    const auto remoteCopyView = m_world.view<RemoteComponent>();
    for (const auto& [ghost, copy] : CopyRegistry::Ghosts())
    {
        if (!copy || s_lifeFollowsOwner.contains(ghost))
            continue;
        Actor* pCopy = Cast<Actor>(TESForm::GetById(copy));
        std::optional<entt::entity> entity = Utils::FindByFormId(formView, copy);
        if (!entity)
        {
            const auto cRemoteIt = std::find_if(
                std::begin(remoteCopyView), std::end(remoteCopyView),
                [&](const entt::entity aOther)
                {
                    const auto& cRemote = remoteCopyView.get<RemoteComponent>(aOther);
                    return cRemote.CachedRefId == copy && (!cRemote.CachedBaseId || (pCopy && pCopy->baseForm && pCopy->baseForm->formID == cRemote.CachedBaseId));
                });
            if (cRemoteIt != std::end(remoteCopyView))
                entity = *cRemoteIt;
        }
        const auto* pRemote = entity ? m_world.try_get<RemoteComponent>(*entity) : nullptr;
        const auto* pLocal = entity ? m_world.try_get<LocalComponent>(*entity) : nullptr;
        const bool cCopyGone = !pCopy || pCopy->IsDeleted();
        const bool cCopyDead = DeadByRunnersWord(pRemote, pLocal) || (!cCopyGone && pCopy->IsDead());
        if (GhostDeathPolicy::OnStandInGone(cCopyDead, IsFollowerCopyId(copy, pCopy)) == GhostDeathPolicy::Action::LayDown)
            LayDownWhenReleased(ghost, copy);
    }

    // A party joined later switches its actors on again, which marks them again.
    s_lifeFollowsOwner.clear();
    s_leaderSwitchedOnAt.clear();

    auto remoteView = m_world.view<FormIdComponent, RemoteComponent>();
    for (auto entity : remoteView)
    {
        auto& formIdComponent = remoteView.get<FormIdComponent>(entity);

        auto pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        // GetExtension() returns null for an actor this client did not allocate.
        ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension)
            continue;

        if (pExtension->IsRemotePlayer())
        {
            const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
            const uint32_t cHandles = refWord & 0x3FF;
            // One on a horse this game put it on is taken off it first, and deleted after that (MountService).
            if (MountService::ReleaseBeforeDelete(remoteView.get<RemoteComponent>(entity).Id, pActor))
                RecentDeletes::Record(pActor, formIdComponent.Id, cHandles, refWord, RecentDeletes::kOnDisconnect);
            // Freeing a copy the game still holds (a combat target, an NPC's package) is a use-after-free: it is switched off and freed
            // once the game lets go of it, as a copy the server removes (RunWaitingCopies).
            else if (pActor->IsTemporary() && !FindWaitingCopy(pActor) && CopyRemovalPolicy::OnRemoved(cHandles, s_waitingCopies.size()) == CopyRemovalPolicy::Action::WaitDisabled)
            {
                spdlog::info(
                    "Player copy {:X} held on disconnect: {} handle(s) still on it, disabled and queued ({} of {} slots used)", formIdComponent.Id, cHandles,
                    s_waitingCopies.size() + 1, CopyRemovalPolicy::kMaxWaiting);
                LogCopyLife("held (disconnect, to be switched off)", formIdComponent.Id, pActor);
                FrameStartSwitchOff::Queue(pActor);
                s_waitingCopies.push_back(WaitingCopy{formIdComponent.Id, pActor->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
            }
            else if (!FindWaitingCopy(pActor))
            {
                RecentDeletes::Record(pActor, formIdComponent.Id, cHandles, refWord, RecentDeletes::kOnDisconnect);
                LogCopyLife("deleted (disconnect)", formIdComponent.Id, pActor);
                pActor->Delete();
            }
        }
        else
            pExtension->SetRemote(false);
    }

    // A copy of another player's follower does not stay on as a second one, even one not loaded yet: this game's own comes back with the ghosts below.
    TiltedPhoques::Vector<uint32_t> followerCopies;
    for (const uint32_t cFormId : CopyRegistry::OwnCopies())
    {
        Actor* pCopy = Cast<Actor>(TESForm::GetById(cFormId));
        const ActorExtension* pExtension = pCopy ? pCopy->GetExtension() : nullptr;
        if (pExtension && pExtension->IsFollowerCopy())
            followerCopies.push_back(cFormId);
    }
    for (const uint32_t cFormId : followerCopies)
        DeleteTempActor(cFormId);

    // Every remaining copy now belongs to the game, so the copy registry is emptied; their ids will be reused.
    // The ones still standing are remembered for the clean-up on the next connect (s_lastConnectionCopies).
    // Added to, not replaced: a failed connection attempt also reports a disconnection, with no copies left in the registry.
    for (const uint32_t cFormId : CopyRegistry::OwnCopies())
    {
        const Actor* pActor = Cast<Actor>(TESForm::GetById(cFormId));
        if (pActor && !pActor->IsDeleted() && pActor->baseForm)
            s_lastConnectionCopies[cFormId] = pActor->baseForm->formID;
    }
    CopyRegistry::ForgetAllOwnCopies();
    s_handedAway.clear();
    s_pendingLines.clear();
    s_sceneLines.clear();
    s_silentScenes.clear();
    s_revivalsOnSwitchOn.clear();

    // Every part of a copy goes with its remote mark, as when the server removes it. Kept, a copy still waiting for its 3D would get
    // the old session's inventory, factions and death once its 3D came, and the copies' entities would never be freed.
    TiltedPhoques::Vector<entt::entity> remoteEntities;
    for (const auto cEntity : m_world.view<RemoteComponent>())
        remoteEntities.push_back(cEntity);
    for (const auto cEntity : remoteEntities)
        DeleteRemoteEntityComponents(cEntity);
    // An actor of this game's own waits for nothing from a session that is over.
    m_world.clear<WaitingFor3D>();

    m_world.clear<WaitingForAssignmentComponent, LocalComponent, RemoteComponent>();
    // One with nothing left (a copy whose 3D never came has no form id component) is freed; the others go when the game removes the actor.
    for (const auto cEntity : remoteEntities)
    {
        if (m_world.valid(cEntity) && m_world.orphan(cEntity))
            m_world.destroy(cEntity);
    }

    ReleaseAllGhosts();
    s_releasedGhosts.clear();
    s_followerCopies.clear();
    s_hiddenFollowers.clear();
    for (const auto& [formId, pickFormId] : m_pendingLeveledConforms)
    {
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)))
        {
            auto& stage = pActor->GetExtension()->Reconciliation;
            // Don't leave the actor disabled if we disconnect before re-enabling it.
            if (stage == ActorExtension::ReconciliationStage::WaitingForDisable && !pActor->IsDeleted())
                pActor->EnableImpl();

            stage = ActorExtension::ReconciliationStage::None;
        }
    }

    m_pendingLeveledConforms.clear();
    m_conformEquipment.clear();
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    StuckArrows::OnSpawnLimbs(acMessage.ServerId, acMessage.DismemberedLimbs);
    spdlog::debug("Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [view, cookie = acMessage.Cookie](auto entity) { return view.get<WaitingForAssignmentComponent>(entity).Cookie == cookie; });

    if (itor == std::end(view))
    {
        spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto cEntity = *itor;
    const bool isCancelled = view.get<WaitingForAssignmentComponent>(cEntity).Cancelled;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    // What the server now has, so that only a later change is reported.
    s_followerReports[acMessage.ServerId].Told = acMessage.FollowerOfPlayerId == m_transport.GetLocalPlayerId();

    if (isCancelled)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        return;
    }

    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored assignment for actor {:X} because the server returned an invalid ownership epoch", acMessage.ServerId);
        return;
    }

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        spdlog::warn("Discarded assignment for actor {:X} because the local entity no longer has a form", acMessage.ServerId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        spdlog::warn("Discarded assignment for actor {:X} because form {:X} is unavailable", acMessage.ServerId, formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    ActorData actorData{};
    actorData.InitialActorValues = acMessage.AllActorValues;
    actorData.InitialInventory = acMessage.CurrentInventory;
    actorData.IsDead = acMessage.IsDead;
    actorData.IsWeaponDrawn = acMessage.IsWeaponDrawn;
    actorData.RevivedSinceDeath = acMessage.RevivedSinceDeath;

    if (acMessage.Owner)
    {
        spdlog::debug("Received local actor, form id: {:X}", pActor->formID);

        // The server's data is only an echo of this game's own request, and may already be stale.
        // Applying it could Respawn an actor killed meanwhile; local changes reach the server through LocalComponent instead.
        if (pActor->IsDead() != acMessage.IsDead)
            spdlog::info("Local actor {:X} died or revived while its assignment was in flight (server: {}, local: {}), keeping local state", pActor->formID,
                         acMessage.IsDead, pActor->IsDead());
        m_weaponDrawUpdates.erase(pActor->formID);

        // This player's follower coming back: her gear here is the truth, and the server's copy is corrected instead.
        const bool cMyFollower = pActor->IsPlayerFollower();
        if (cMyFollower)
            SendInventoryDifference(pActor, acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentInventory);

        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);

        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        if (const auto* pFormId = m_world.try_get<FormIdComponent>(cEntity))
            spdlog::info("Entity: local actor {:X} has server id {:X}", pFormId->Id, acMessage.ServerId);
        localComponent.IsDead = acMessage.IsDead;
        localComponent.IsWeaponDrawn = acMessage.IsWeaponDrawn;
        pActor->GetExtension()->SetRemote(false);
    }
    else
    {
        spdlog::info("Received remote actor, form id: {:X}, isweapondrawn: {}", pActor->formID, acMessage.IsWeaponDrawn);

        // A temporary actor of this game's own scripts stands for the one another game spawned first for the same event (the server pairs them).
        // A copy of that one made here already goes, so this game's own actor, the one its scene knows, is the only one standing for it.
        if (pActor->IsTemporary() && !CopyRegistry::IsOwnCopy(pActor->formID))
        {
            spdlog::debug("ScriptSpawn: {:X} stands for server character {:X}, which another game spawned first", pActor->formID, acMessage.ServerId);
            NotifyRemoveCharacter copyGoes{};
            copyGoes.ServerId = acMessage.ServerId;
            copyGoes.Gone = false;
            OnRemoveCharacter(copyGoes);
            // The removal forgot the limbs this answer brought for the character, which this game's own actor now stands for.
            StuckArrows::OnSpawnLimbs(acMessage.ServerId, acMessage.DismemberedLimbs);
        }

        // The owner has another kind of creature here (the response carries the owner's base): a copy stands in for it.
        // The copy is set up through the deferred path a temporary actor takes; the local reference becomes a ghost.
        // Not for a body here that is alive at its owner (IsDeadAgainstOwner): it stays a body, see ReconcileActorData.
        // Another player's follower also gets a copy, whatever this game's own reference is like.
        const BaseMatch match = CompareBase(m_world, pActor, acMessage.BaseId, false);
        const bool cOtherFollower = acMessage.FollowerOfPlayerId && acMessage.FollowerOfPlayerId != m_transport.GetLocalPlayerId() && !pActor->IsTemporary();

        // She stands far from here, so no copy goes up; the server sends one if she comes into range.
        // This game's own reference stays this game's: hidden if she is this player's follower too, otherwise an ordinary NPC here that the server does not hear about.
        if (cOtherFollower && !IsPlaceLoadedHere(m_world, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position))
        {
            // Saves must not keep her hidden, so without the guard she is left as she is too.
            const bool cHide = pActor->IsPlayerFollower() && Actor::IsSaveGuarded();
            if (cHide)
            {
                CopyRegistry::SetStandIn(pActor->formID, 0);
                s_hiddenFollowers[acMessage.ServerId] = pActor->formID;
                // Only a loaded one: this can be a reference just released, with no 3D yet. RunGhostChecks hides it once it has its 3D.
                if (pActor->GetNiNode() && !pActor->IsDeleted() && !pActor->IsStillLoading())
                    HideGhost(pActor);
            }
            m_world.remove<EarlyAnimationBufferComponent>(cEntity);
            spdlog::info("Follower: {:X} (server id {:X}) is player {:X}'s, far from here; this game's own reference is {}", pActor->formID, acMessage.ServerId, acMessage.FollowerOfPlayerId,
                         cHide ? "hidden, as this player's follower too" : "left as it is");
            return;
        }

        Actor* pCopy = nullptr;
        if (cOtherFollower)
        {
            // As a copy made from a spawn is.
            CharacterSpawnRequest baseOnly{};
            baseOnly.ServerId = acMessage.ServerId;
            baseOnly.BaseId = acMessage.BaseId;
            baseOnly.LeveledNpcPickId = acMessage.LeveledNpcPickId;
            pCopy = FollowerCopy(pActor, BaseForCopy(m_world, baseOnly), acMessage.ServerId, acMessage.FollowerOfPlayerId);
        }
        // Not for a temporary actor: the ghost is kept under its id, which the game gives to another actor once it frees it, and that one
        // would then be hidden. It is adopted, and the owner's animation data is kept from its graph (MarkForeignGraph below).
        else if (
            match.pOwnerBase &&
            CopyLifetime::StandsInForForeign(match.Foreign, pActor->IsTemporary(), match.Foreign && IsDeadAgainstOwner(pActor, acMessage.IsDead, acMessage.RevivedSinceDeath)))
            pCopy = StandInForForeignCreature(pActor, match.pOwnerBase);
        // Without a copy (flat: no save guard) she is shown on this game's own reference, and is still another player's follower there.
        if (ActorExtension* pExtension = pActor->GetExtension(); pExtension && !pCopy)
            pExtension->SetOthersFollower(cOtherFollower);
        if (pCopy)
        {
            CharacterSpawnRequest spawn{};
            spawn.ServerId = acMessage.ServerId;
            // As a spawn for a reference has it: if this copy goes before it has its 3D, the next one stands in for the same reference.
            m_world.GetModSystem().GetServerModId(pActor->formID, spawn.FormId);
            spawn.BaseId = acMessage.BaseId;
            spawn.LeveledNpcPickId = acMessage.LeveledNpcPickId;
            spawn.Position = acMessage.Position;
            spawn.CellId = acMessage.CellId;
            spawn.InitialActorValues = acMessage.AllActorValues;
            spawn.InventoryContent = acMessage.CurrentInventory;
            spawn.FactionsContent = acMessage.FactionsContent;
            spawn.ActionsToReplay = acMessage.ActionsToReplay;
            spawn.PlayerId = acMessage.PlayerId;
            spawn.IsDead = acMessage.IsDead;
            spawn.RevivedSinceDeath = acMessage.RevivedSinceDeath;
            spawn.IsWeaponDrawn = acMessage.IsWeaponDrawn;
            spawn.FollowerOfPlayerId = acMessage.FollowerOfPlayerId;

            const entt::entity copyEntity = m_world.create();
            auto& copyRemote = m_world.emplace<RemoteComponent>(copyEntity, acMessage.ServerId, pCopy->formID, acMessage.OwnershipEpoch);
            copyRemote.DeadAtOwner = acMessage.IsDead;
            if (pCopy->baseForm)
                copyRemote.CachedBaseId = pCopy->baseForm->formID;
            ClaimCopyId(copyEntity, pCopy->formID);
            LogCopyLife("placed (stand-in)", pCopy->formID, pCopy);
            pCopy->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
            pCopy->SetActorValues(ValuesForCopy(acMessage.AllActorValues, acMessage.IsDead, pCopy));
            auto& copyInterpolation = InterpolationSystem::Setup(m_world, copyEntity);
            copyInterpolation.Position = acMessage.Position;
            AnimationSystem::Setup(m_world, copyEntity);
            m_world.emplace<WaitingFor3D>(copyEntity, spawn);
            return;
        }

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id, acMessage.OwnershipEpoch).DeadAtOwner = acMessage.IsDead;

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        // Where its owner has it, until its movement arrives: the leader's claim is judged by it (IsNearLocalPlayer).
        InterpolationSystem::Setup(m_world, cEntity).Position = acMessage.Position;
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world, m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);
        // This path adopted a levelled reference without ever seeing the owner's base; the response carries it now.
        MarkForeignGraph(m_world, cEntity, pActor, acMessage.BaseId);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        // This player's own follower, owned by another player (the server hands her over during a loading screen).
        // Her gear here is the truth: nothing from the server or the other game is applied, and she is not moved away.
        // Moving her would make the two games pull her back and forth; the other game's outfit for her is not ours.
        const bool cMyFollower = pActor->IsPlayerFollower();

        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, actorData, !cMyFollower, false);
        if (pActor->GetExtension()->IsRemotePlayer())
            InventoryService::ApplyHandEquipment(pActor, acMessage.CurrentInventory, true);

        if (cMyFollower)
        {
            // She is asked for only when near this player; a far game cannot run her and would hand her straight back.
            // One farther away is asked for by RunFollowerReclaims once she is near.
            const auto* pPlayer = PlayerCharacter::Get();
            const bool cHere = pActor->GetNiNode() && pPlayer && glm::distance(glm::vec3(pActor->position), glm::vec3(pPlayer->position)) < 2048.f;
            static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_nextFollowerClaim;
            const auto now = std::chrono::steady_clock::now();
            auto& next = s_nextFollowerClaim[pActor->formID];
            if (cHere && now >= next)
            {
                next = now + std::chrono::seconds(1);
                spdlog::info("Follower {:X} (server id {:X}) came back as another player's; kept as she is here and asked for", pActor->formID, acMessage.ServerId);
                RequestOwnership(pActor->formID, acMessage.ServerId, cEntity);
            }
            else if (!cHere)
                spdlog::info("Follower {:X} (server id {:X}) came back as another player's, not near this player; asked for once she is", pActor->formID,
                             acMessage.ServerId);
        }
        // As for a teleport (OnNotifyActorTeleport): not out to an exterior beyond the loaded area here.
        // The game would read that cell from disk in this frame.
        else if (!IsPlaceLoadedHere(m_world, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position) && (!IsActorLoadedHere(pActor) || acMessage.WorldSpaceId))
        {
            // A reference its owner took out of this interior goes with it, or it would stay here frozen.
            // The world space's persistent cell takes it with no cell read.
            auto* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.WorldSpaceId)));
            s_teleportsRefused.erase(pActor->formID);
            TESObjectCELL* pOutdoors = pWorldSpace && IsActorLoadedHere(pActor) && !pActor->IsTemporary() && !pActor->GetWorldSpace() ? pWorldSpace->GetPersistentCell() : nullptr;
            if (pOutdoors && pOutdoors->worldspace == pWorldSpace)
            {
                DiscoveryService::NoteMovedHere(pActor->formID);
                pActor->MoveTo(pOutdoors, acMessage.Position);
                spdlog::debug(
                    "Assignment: {:X} left this place with its owner (world space {:X}, position ({:.0f}))", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.Position);
            }
            else
            {
                spdlog::info(
                    "Assignment: {:X} not moved to its owner's place, beyond the loaded area here (world space {:X}, cell {:X}, position ({:.0f}, {:.0f}, {:.0f}))", pActor->formID,
                    acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);
                // Not where its owner has it: the owner's way into a seat is not replayed on it here (NpcFurniture::SkipSeatReplay).
                if (!acMessage.Owner)
                    CopyRegistry::NoteAwayFromOwner(pActor->formID, OwnerPlaceOf(m_world, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position));
            }
        }
        else
            MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

        // The owner's leveled pick rides the assignment response for actors we discovered ourselves
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId, &acMessage.CurrentInventory);
    }
}

// Logs what the owner's appearance turned into here, from both places that make a player copy.
// TESNPC::Create starts from this player's own race and loads the owner's over it.
// A load that did not take leaves a copy of this player's race with no name and no face (e.g. differing mod lists).
static void LogPlayerCopyAppearance(const CharacterSpawnRequest& acMessage, const TESNPC* apNpc) noexcept
{
    if (!acMessage.IsPlayer || !apNpc)
        return;
    const auto* pRace = reinterpret_cast<const TESForm*>(apNpc->raceForm.race); // TESRace derives from TESForm first
    const TESNPC* pOwnBase = Cast<TESNPC>(PlayerCharacter::Get()->baseForm);
    const bool cOwnRace = pOwnBase && reinterpret_cast<const TESForm*>(pOwnBase->raceForm.race) == pRace;
    spdlog::info("Player copy appearance: {} bytes, change flags {:X}, {} face tints; race {:X}{}, name '{}'", acMessage.AppearanceBuffer.size(), acMessage.ChangeFlags,
                 acMessage.FaceTints.Entries.size(), pRace ? pRace->formID : 0, cOwnRace ? " (this player's own race: the owner's may not have loaded)" : "",
                 apNpc->fullName.value.AsAscii());
}

/**
 * Make or update this game's copy of a character another game runs, from the server's spawn message.
 * A character already known here is corrected where it went stale.
 * Otherwise its copy is picked or made now (the reference itself, a stand-in or a new actor) and finished once it has its 3D.
 *
 * @param acMessage The spawn message.
 */
void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept
{
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored spawn for actor {:X} because the ownership epoch is invalid", acMessage.ServerId);
        return;
    }
    // An older character of a player, sent after the newer one: the newer one stays that player's copy here.
    if (acMessage.IsPlayer)
    {
        const uint32_t cNewest = s_playerCopies.NewestGeneration(acMessage.PlayerId);
        if (PlayerCopyRules::OnPlayerSpawn(cNewest, acMessage.SpawnGeneration) == PlayerCopyRules::Spawn::kStale)
        {
            spdlog::debug(
                "Player copy: ignored player {}'s character {:X} (generation {}), older than the one seen here ({})", acMessage.PlayerId, acMessage.ServerId,
                acMessage.SpawnGeneration, cNewest);
            return;
        }
        s_playerCopies.SawGeneration(acMessage.PlayerId, acMessage.SpawnGeneration);
    }
    StuckArrows::OnSpawnLimbs(acMessage.ServerId, acMessage.DismemberedLimbs);

    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = Utils::FindById<RemoteComponent>(remoteView, acMessage.ServerId);

    if (remoteItor)
    {
        const entt::entity cExisting = *remoteItor;
        const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cExisting);
        const bool hasActor = m_world.all_of<WaitingFor3D>(cExisting) || (pFormIdComponent && TESForm::GetById(pFormIdComponent->Id));

        if (hasActor)
        {
            // The actor is here, so it is not respawned, but its ownership epoch is refreshed from the re-sent spawn.
            // The server refuses a claim whose epoch does not match its own, so a stale epoch blocks every later claim.
            auto& remoteComponent = remoteView.get<RemoteComponent>(cExisting);
            if (remoteComponent.OwnershipEpoch != acMessage.OwnershipEpoch)
            {
                spdlog::info("Character {:X} is already spawned; its ownership epoch moved {} -> {}", acMessage.ServerId, remoteComponent.OwnershipEpoch, acMessage.OwnershipEpoch);
                remoteComponent.OwnershipEpoch = acMessage.OwnershipEpoch;
                // Told as a change, so that what was decided for the copy under the old epoch is withdrawn (CopyWork.h).
                m_world.patch<RemoteComponent>(cExisting);
            }
            else
                // The server re-sends every character in range at each cell crossing, so this is routine.
                spdlog::debug("Character with remote id {:X} is already spawned.", acMessage.ServerId);

            // The server's word on whether the owner's actor is dead; the copy itself is only changed below, when stale.
            remoteComponent.DeadAtOwner = acMessage.IsDead;
            // A death or a revive the owner has taken replaces one seen here before the hand-off; a plain life may come before the killing blow lands.
            if (acMessage.IsDead || acMessage.RevivedSinceDeath)
                remoteComponent.DeadHere = false;
            // Its mark too, kept for an announcement from here (BuildActorData).
            if (pFormIdComponent)
                if (Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)); pActor && pActor->GetExtension())
                    pActor->GetExtension()->RevivedSinceDeath = acMessage.RevivedSinceDeath;

            // Position, cell and death state normally arrive through the movement and death paths, and are left alone.
            // The server withholds updates for characters outside the loaded grid, and re-sends the spawn on return.
            // So the spawn's state is applied only when no update has arrived for this character for a while.
            if (auto* pInterpolation = m_world.try_get<InterpolationComponent>(cExisting))
            {
                constexpr uint64_t cStaleAfterMs = 2000;
                const uint64_t currentTick = m_transport.GetClock().GetCurrentTick();
                const uint64_t newestTick = pInterpolation->TimePoints.empty() ? 0 : pInterpolation->TimePoints.back().Tick;
                const bool stale = newestTick == 0 || (currentTick > newestTick && currentTick - newestTick > cStaleAfterMs);

                if (stale && pFormIdComponent)
                {
                    if (Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)))
                    {
                        const glm::vec3 wanted{acMessage.Position.x, acMessage.Position.y, acMessage.Position.z};
                        if (newestTick == 0)
                            spdlog::debug(
                                "Character {:X} had no update yet and the server has re-sent it; moving the copy to where the server says ({:.0f})", acMessage.ServerId, wanted);
                        else
                            spdlog::debug(
                                "Character {:X} was {} ms without an update and the server has re-sent it; moving the copy to where the server says ({:.0f})", acMessage.ServerId,
                                currentTick - newestTick, wanted);

                        // The buffered points predate the silence and would drag the copy back through where it was.
                        pInterpolation->TimePoints.clear();
                        pInterpolation->PositionError = {};
                        pInterpolation->IsExtrapolating = false;
                        pInterpolation->Position = wanted;
                        pActor->ForcePosition(NiPoint3(wanted));
                        CopyRegistry::ForgetAwayFromOwner(pActor->formID);

                        // Actor values and weapon state are also sent in range only, so they are stale too.
                        // Under this staleness gate nothing fresher arrives to conflict with the spawn's values.
                        pActor->SetActorValues(SpawnValues(acMessage, pActor));
                        if (pActor->actorState.IsWeaponDrawn() != acMessage.IsWeaponDrawn)
                            pActor->SetWeaponDrawnEx(acMessage.IsWeaponDrawn);

                        // A death is broadcast to all, but a copy with a stale epoch drops it (ownership transfers are range-filtered).
                        // The spawn is then the only place that still carries the death.
                        if (IsDeadAgainstOwner(pActor, acMessage.IsDead, acMessage.RevivedSinceDeath))
                            SendKillingBlowToOwner(m_transport, pActor, acMessage.ServerId);
                        else if (pActor->IsDead() != acMessage.IsDead)
                        {
                            spdlog::info("Character {:X} came back {} and this copy had it the other way; correcting", acMessage.ServerId, acMessage.IsDead ? "dead" : "alive");
                            acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
                        }

                        // Faction changes are range-filtered too; a copy in the wrong faction squares up to someone and never swings.
                        if (auto* pCache = m_world.try_get<CacheComponent>(cExisting))
                        {
                            if (!(pCache->FactionsContent == acMessage.FactionsContent))
                            {
                                spdlog::info("Character {:X} came back in different factions from the server's; applying them", acMessage.ServerId);
                                pCache->FactionsContent = acMessage.FactionsContent;
                                pActor->SetFactions(pCache->FactionsContent);
                            }
                        }

                        // Inventory is only compared, not applied: SetActorInventory on every return can flood refused Unequip replays.
                        if (!(pActor->GetActorInventory() == acMessage.InventoryContent))
                            spdlog::info("Character {:X} also came back with a different inventory from the server's; not applied, only noticed", acMessage.ServerId);

                        // Its worn armor is put right, though: a piece taken off or changed out of range would stay on the copy, waiting at the
                        // range edge, until its owner changed something again. Only the pieces that differ change (NpcEquipmentCheck::ApplyWornItems).
                        // Not this player's own follower, whose gear here is the truth.
                        if (pActor->GetExtension()->IsRemote() && pActor->HasLoadedProcess() && !pActor->IsPlayerFollower())
                            NpcEquipmentCheck::ApplyWornItems(pActor, acMessage.InventoryContent);
                    }
                }
            }

            // A copy still waiting for its 3D is made from its spawn request, so the server's newer state goes there.
            // Otherwise what the owner looted, dropped or wore since the first spawn is undone when the 3D comes.
            if (auto* pWaiting = m_world.try_get<WaitingFor3D>(cExisting))
            {
                pWaiting->SpawnRequest.InventoryContent = acMessage.InventoryContent;
                pWaiting->SpawnRequest.FactionsContent = acMessage.FactionsContent;
                pWaiting->SpawnRequest.InitialActorValues = acMessage.InitialActorValues;
                pWaiting->SpawnRequest.IsDead = acMessage.IsDead;
                pWaiting->SpawnRequest.RevivedSinceDeath = acMessage.RevivedSinceDeath;
                pWaiting->SpawnRequest.IsWeaponDrawn = acMessage.IsWeaponDrawn;
                pWaiting->SpawnRequest.OwnershipEpoch = acMessage.OwnershipEpoch;
                pWaiting->SpawnRequest.FollowerOfPlayerId = acMessage.FollowerOfPlayerId;
            }

            return;
        }

        // The server still counts this character as spawned here, but the local copy is gone, so it is spawned again.
        spdlog::info("Character with remote id {:X} has no actor anymore, spawning it again", acMessage.ServerId);
        DeleteRemoteEntityComponents(cExisting);
        // The limbs went with the old copy's components; the new copy still loses them.
        StuckArrows::OnSpawnLimbs(acMessage.ServerId, acMessage.DismemberedLimbs);
        if (m_world.orphan(cExisting))
            m_world.destroy(cExisting);
    }

    Actor* pActor = nullptr;

    std::optional<entt::entity> entity;

    // This game's own reference of it is dead, and the owner's alive (IsDeadAgainstOwner): it stays a body here.
    bool deadHere = false;

    // One of this game's own actors, coming back from the player it was given to. See s_handedAway.
    if (acMessage.FormId == GameId{})
    {
        if (const auto handed = s_handedAway.find(acMessage.ServerId); handed != s_handedAway.end())
        {
            const uint32_t cOwnId = handed->second.FormId;
            s_handedAway.erase(handed);

            Actor* pOwn = Cast<Actor>(TESForm::GetById(cOwnId));
            TESNPC* pSentBase = acMessage.BaseId != GameId{} ? Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId))) : nullptr;
            const bool cSameKind = pOwn && pSentBase && pOwn->baseForm == pSentBase;

            const auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
            const auto itor = Utils::FindByFormId(view, cOwnId);

            if (pOwn && !pOwn->IsDeleted() && cSameKind)
            {
                pActor = pOwn;
                entity = itor ? *itor : m_world.create();
                spdlog::info("HandedAway: server character {:X} is this game's own actor {:X} come back; it is that actor again and no copy is made", acMessage.ServerId, cOwnId);
            }
            else
            {
                spdlog::info("HandedAway: server character {:X} came back, but the actor it was here ({:X}) is {}; a copy is made", acMessage.ServerId, cOwnId,
                             !pOwn ? "not loaded" : pOwn->IsDeleted() ? "deleted" : "another creature now");
                // The actor now carrying that id was held back from the server because of the record, so it is processed now.
                if (pOwn && itor)
                    ProcessNewEntity(*itor);
            }
        }
    }

    if (pActor)
    {
        // Taken back above.
    }
    // Custom forms
    else if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            pNpc = BaseForCopy(m_world, acMessage);

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
            LogPlayerCopyAppearance(acMessage, pNpc);
        }

        pActor = RememberOwnCopy(Actor::Create(pNpc));
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

        auto waitingView = m_world.view<FormIdComponent, WaitingForAssignmentComponent>();
        const auto waitingItor = Utils::FindByFormId(waitingView, cActorId);

        if (waitingItor)
        {
            spdlog::info("Character with form id {:X} already has a spawn request in progress.", cActorId);
            return;
        }

        auto* const pForm = TESForm::GetById(cActorId);
        pActor = Cast<Actor>(pForm);

        // Moving a reference that is still loading crashes; discovery asks for its assignment once it has loaded.
        if (pActor && pActor->IsStillLoading())
        {
            spdlog::info("Skipped spawn for actor {:X} because it is still loading, server id: {:X}", cActorId, acMessage.ServerId);
            return;
        }

        if (!pActor)
        {
            // The reference is not loaded here, yet its owner drives it and it can walk up and attack unseen.
            // A copy of the owner's base stands in, registered as a ghost so the local reference is disabled if it loads.
            TESNPC* pOwnerBase = acMessage.BaseId != GameId{} ? Cast<TESNPC>(TESForm::GetById(World::Get().GetModSystem().GetGameId(acMessage.BaseId))) : nullptr;
            Actor* pCopy = pOwnerBase ? RememberOwnCopy(Actor::Create(pOwnerBase)) : nullptr;
            if (!pCopy)
            {
                spdlog::error("Failed to retrieve Actor {:X}, it will not be spawned, possibly missing mod", cActorId);
                spdlog::error("\tForm : {:X}", pForm ? pForm->formID : 0);
                return;
            }
            CopyRegistry::SetStandIn(cActorId, pCopy->formID);
            if (acMessage.FollowerOfPlayerId && acMessage.FollowerOfPlayerId != m_transport.GetLocalPlayerId())
                MarkFollowerCopy(pCopy);
            spdlog::info("Stand-in: reference {:X} is not loaded here; copy {:X} of the owner's {} ({:X}) stands in for it", cActorId, pCopy->formID, pOwnerBase->fullName.value.AsAscii(),
                         pOwnerBase->formID);
            pActor = pCopy;
            entity = m_world.create();
        }

        // Another player's follower is shown on a copy, whatever this game's own reference is like.
        if (pActor && !entity && acMessage.FollowerOfPlayerId && acMessage.FollowerOfPlayerId != m_transport.GetLocalPlayerId() && !pActor->IsTemporary())
        {
            if (Actor* pCopy = FollowerCopy(pActor, BaseForCopy(m_world, acMessage), acMessage.ServerId, acMessage.FollowerOfPlayerId))
            {
                pActor = pCopy;
                entity = m_world.create();
            }
        }
        // Without a copy (flat: no save guard) she is shown on this game's own reference, and is still another player's follower there.
        if (pActor && pActor->GetExtension() && !pActor->IsTemporary())
            pActor->GetExtension()->SetOthersFollower(acMessage.FollowerOfPlayerId && acMessage.FollowerOfPlayerId != m_transport.GetLocalPlayerId());

        // A body here that is alive in its owner's game stays this game's body, not a living stand-in.
        deadHere = IsDeadAgainstOwner(pActor, acMessage.IsDead, acMessage.RevivedSinceDeath);

        // Another kind of creature than the owner's: a copy of the owner's stands in, the local one becomes a ghost.
        // The local reference keeps its own entity (a bare FormIdComponent that ProcessNewEntity skips).
        if (const BaseMatch match = CompareBase(m_world, pActor, acMessage.BaseId, false); match.Foreign && match.pOwnerBase && !deadHere)
        {
            if (Actor* pCopy = StandInForForeignCreature(pActor, match.pOwnerBase))
            {
                pActor = pCopy;
                entity = m_world.create();
            }
        }

        if (!entity)
        {
            // The reference itself takes the character now, so an old stand-in's record must not keep disabling it.
            CopyRegistry::ForgetGhost(cActorId);
            StopHiding(pActor);

            const auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
            const auto itor = Utils::FindByFormId(view, cActorId);

            if (itor)
                entity = *itor;
            else
                entity = m_world.create();
        }
    }

    if (!pActor)
    {
        spdlog::error("Actor object {:X} could not be created.", acMessage.ServerId);
        return;
    }

    // Its character came, so the wait for the leader's game is over.
    s_leaderSwitchedOnAt.erase(pActor->formID);

    // The base name lets two players' logs be compared: a leveled creature can resolve to a different animal on each side.
    const auto* pSpawnedBase = Cast<TESNPC>(pActor->baseForm);
    spdlog::info("CharacterSpawnRequest, server id: {:X}, form id: {:X}, local base {:X} ({})", acMessage.ServerId, pActor->formID, pActor->baseForm ? pActor->baseForm->formID : 0,
                 pSpawnedBase ? pSpawnedBase->fullName.value.AsAscii() : "?");

    // Pending reconciliation re-enables the actor after applying the owner's pick.
    if (pActor->IsDisabled() && pActor->GetExtension()->Reconciliation != ActorExtension::ReconciliationStage::WaitingForDisable)
    {
        spdlog::warn("Disabled actor is being re-enabled: {:X}", pActor->formID);
        pActor->EnableImpl();
    }

    SetUpCopyFromSpawn(m_world, *entity, pActor, acMessage, acMessage.Position);

    if (acMessage.IsPlayer)
        RetireOlderPlayerCopy(acMessage.PlayerId, acMessage.ServerId, *entity, pActor);

    if (deadHere)
        SendKillingBlowToOwner(m_transport, pActor, acMessage.ServerId);
    else
        MatchOwnersLife(pActor, acMessage.IsDead);

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    // Taken from every spawn, as a reference of the game's own can be a summon's copy once and not the next time.
    // A body a spell is raising stays that spell's (Actor::IsBeingRaised), whatever its owner calls it.
    // The old mark is cleared first, as IsBeingRaised reads it.
    ActorExtension* pSpawnedExtension = pActor->GetExtension();
    pSpawnedExtension->CommandedAsSummonCopy = false;
    const bool cRaisedHere = pActor->IsBeingRaised();
    pSpawnedExtension->CommandedAsSummonCopy = acMessage.IsPlayerSummon && !cRaisedHere;
    if (acMessage.IsPlayerSummon)
    {
        // Prevents remote summons agroing other players.
        pActor->SetCommandingActor(PlayerCharacter::Get()->GetHandle());
    }

    // Static references arrive with their own locally rolled leveled pick; conform to the owner's.
    // Not a body that stays dead here: the rebuild re-rolls the reference, which brings it back.
    if (acMessage.FormId != GameId{} && !deadHere)
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId, &acMessage.InventoryContent);

    auto& newRemote = m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);
    newRemote.DeadAtOwner = acMessage.IsDead;
    // Kept now too: a reference handed back to this game before its 3D comes keeps the mark.
    if (ActorExtension* pExtension = pActor->GetExtension())
        pExtension->RevivedSinceDeath = acMessage.RevivedSinceDeath;
    if (CopyRegistry::IsOwnCopy(pActor->formID) && pActor->baseForm)
        newRemote.CachedBaseId = pActor->baseForm->formID;
    ClaimCopyId(*entity, pActor->formID);

    auto& interpolationComponent = InterpolationSystem::Setup(m_world, *entity);
    interpolationComponent.Position = acMessage.Position;

    AnimationSystem::Setup(m_world, *entity);
    MarkForeignGraph(m_world, *entity, pActor, acMessage.BaseId);

    m_world.emplace_or_replace<WaitingFor3D>(*entity, acMessage);

    auto& remoteAnimationComponent = m_world.get<RemoteAnimationComponent>(*entity);

    AnimationSystem::AddActionsForReplay(m_world, remoteAnimationComponent, acMessage.ActionsToReplay);

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(*entity, acMessage.ActionsToReplay);
#endif
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        const auto itor = Utils::FindById<RemoteComponent>(view, serverId);

        if (!itor)
            continue;

        InterpolationSystem::BindPoseIdentity(m_world, *itor);
        auto& interpolationComponent = view.get<InterpolationComponent>(*itor);
        const auto& movement = update.UpdatedMovement;

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;
        point.VRPoseData = update.UpdatedVRPose;
        // A dead body's pose is also held on the copy, whatever its playback makes of it: one sent with its spawn comes before
        // the copy's ragdoll does (VRBodySync::PlaceCorpse).
        if (view.get<RemoteComponent>(*itor).DeadAtOwner && update.UpdatedVRPose.HasData && !update.UpdatedVRPose.NoBones)
            interpolationComponent.RestPose = update.UpdatedVRPose;
        GameExtension::ReadPose(serverId, view.get<RemoteComponent>(*itor).OwnershipEpoch, point.Tick, point.Position, point.Rotation.z, point.VRPoseData);
#ifdef SKYRIMVR
        VRBodySync::NoteReceivedPose(view.get<RemoteComponent>(*itor).CachedRefId, acMessage.Tick, point.VRPoseData);
#endif

        InterpolationSystem::AddPoint(interpolationComponent, point, m_transport.GetMessageReceiveTime());
    }
}

void CharacterService::OnNotifyActionEvents(const NotifyActionEvents& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, actions] : acMessage.Actions)
    {
        const auto itor = Utils::FindById<RemoteComponent>(view, serverId);

        if (!itor)
            continue;

        auto& animationComponent = view.get<RemoteAnimationComponent>(*itor);

        // Each action plays once the actor's playback reaches its tick, in step with its movement.
        for (ActionEvent action : actions)
        {
            AnimationSystem::ResolveModIds(m_world, action);
            animationComponent.TimePoints.push_back(std::move(action));
        }

        // Bounds the replay queue: an actor plays one action a frame, so an unbounded queue falls minutes behind.
        if (const auto* pFormId = m_world.try_get<FormIdComponent>(*itor))
            AnimationSystem::TrimBacklog(animationComponent, pFormId->Id);
        else
            AnimationSystem::TrimBacklog(animationComponent, 0);
    }
}

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    auto view = m_world.view<LocalAnimationComponent, FormIdComponent>();
    const auto itor = Utils::FindByFormId(view, acActionEvent.ActorId);

    ActionEvent action = acActionEvent;
    AnimationSystem::AddModIds(m_world, action);

    if (itor)
    {
        auto& localComponent = view.get<LocalAnimationComponent>(*itor);

        localComponent.Append(action);
    }
    else if (m_transport.IsOnline())
    {
        // A `LocalAnimationComponent` is not attached yet, but the actor already exists and is running animations

        auto view = m_world.view<FormIdComponent, EarlyAnimationBufferComponent>();
        const auto itor = Utils::FindByFormId(view, acActionEvent.ActorId);

        if (itor)
        {
            view.get<EarlyAnimationBufferComponent>(*itor).Actions.push_back(action);
        }
    }
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    // Every remote actor, not only those with a CacheComponent: copies made from a server spawn never get one (live-factions).
    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = Utils::FindById<RemoteComponent>(view, id);

        if (!itor)
            continue;

        auto* const pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*itor).Id));
        if (!pActor)
            continue;

        if (auto* pCache = m_world.try_get<CacheComponent>(*itor))
            pCache->FactionsContent = factions;

        pActor->SetFactions(factions);
        spdlog::info("Factions of remote actor {:X} ({:X}) updated: {} faction(s) from its owner", pActor->formID, id, factions.NpcFactions.size() + factions.ExtraFactions.size());
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored ownership update for actor {:X} because the epoch is invalid", acMessage.ServerId);
        return;
    }

    auto entity = Utils::FindEntityByServerId(acMessage.ServerId);
    if (entity && !m_world.any_of<LocalComponent, RemoteComponent>(*entity))
        entity.reset();

    uint32_t currentEpoch = 0;
    if (entity)
    {
        if (const auto* pLocalComponent = m_world.try_get<LocalComponent>(*entity))
            currentEpoch = pLocalComponent->OwnershipEpoch;
        else if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*entity))
            currentEpoch = pRemoteComponent->OwnershipEpoch;
    }

    if (currentEpoch != 0 && acMessage.OwnershipEpoch <= currentEpoch)
    {
        spdlog::debug("Ignored stale ownership update for actor {:X} at epoch {}; current epoch is {}", acMessage.ServerId, acMessage.OwnershipEpoch, currentEpoch);
        return;
    }

    const bool isLocalOwner = acMessage.OwnerPlayerId == m_transport.GetLocalPlayerId();
    if (!entity)
    {
        // A transfer lacks the form data to recreate an unknown actor.
        // Declining lets the server try another loaded client.
        if (isLocalOwner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
        else
            spdlog::debug("Ignored ownership update for unknown actor {:X} at epoch {}", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    const entt::entity cEntity = *entity;
    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    Actor* pActor = pFormIdComponent ? Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)) : nullptr;

    // Preserve the accepted epoch's pick for actors that still need to be created.
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(cEntity))
        pWaitingFor3D->SpawnRequest.LeveledNpcPickId = acMessage.LeveledNpcPickId;

    // A reference that rolled as another creature here is ghosted behind a copy of the owner's creature.
    // The character is run here on that copy, which every game sees as the same creature.
    // Bound to the ghost itself, it is declined and the server keeps looking: running the ghost would flip the creature for everyone.
    // A living follower's copy is declined too: another player's follower is run by her own player's game.
    uint32_t standInFor = 0;
    if (isLocalOwner && pFormIdComponent)
    {
        const uint32_t cBoundId = pFormIdComponent->Id;
        const auto ghost = CopyRegistry::StandInEntryOf(cBoundId);
        Actor* pBoundActor = Cast<Actor>(TESForm::GetById(cBoundId));
        ActorExtension* pBoundExtension = pBoundActor ? pBoundActor->GetExtension() : nullptr;
        const bool cOnCopy = ghost && ghost->second == cBoundId;
        const bool cLivingFollower = pBoundExtension && pBoundExtension->IsFollowerCopy() && !pBoundActor->IsDead();
        if (ghost && (!cOnCopy || cLivingFollower))
        {
            const auto* pCopy = Cast<Actor>(TESForm::GetById(ghost->second));
            const Actor* pLocal = Cast<Actor>(TESForm::GetById(ghost->first));
            const auto* pLocalBase = pLocal ? Cast<TESNPC>(pLocal->baseForm) : nullptr;
            const auto* pCopyBase = pCopy ? Cast<TESNPC>(pCopy->baseForm) : nullptr;
            spdlog::info("Hand-off of {:X} (server id {:X}) declined: {} reference {:X} here ({} here, the owner's kind is {})", pFormIdComponent->Id, acMessage.ServerId,
                         cOnCopy ? "it is another player's follower, shown on a copy standing in for" : "it is bound to the hidden", ghost->first,
                         pLocalBase ? pLocalBase->fullName.value.AsAscii() : "not loaded", pCopyBase ? pCopyBase->fullName.value.AsAscii() : "?");
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
        if (cOnCopy)
            standInFor = ghost->first;
    }

    if (isLocalOwner)
    {
        if (!pFormIdComponent || !pActor || !pActor->GetNiNode())
        {
            uint32_t cachedRefId = pFormIdComponent ? pFormIdComponent->Id : 0;
            if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
                cachedRefId = pRemoteComponent->CachedRefId;

            if (pActor)
                pActor->GetExtension()->SetRemote(true);

            // Run here and seen dying, with a death the grant has not taken: its copy is still a body if it goes.
            const auto* pWasLocal = m_world.try_get<LocalComponent>(cEntity);
            const bool cRefusesBlow = pActor && (pActor->IsProtected() || pActor->IsEssential());
            const bool cDeadHere = pWasLocal && GhostDeathPolicy::KilledBeforeHandOff(pWasLocal->IsDead, acMessage.CurrentActorData.IsDead, cRefusesBlow);
            m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
            if (m_world.all_of<RemoteComponent>(cEntity))
                m_world.patch<RemoteComponent>(cEntity, [&](RemoteComponent& aRemote) { aRemote.OwnershipEpoch = acMessage.OwnershipEpoch; });
            else if (cachedRefId != 0)
                m_world.emplace<RemoteComponent>(cEntity, acMessage.ServerId, cachedRefId, acMessage.OwnershipEpoch);
            if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
            {
                pRemoteComponent->DeadAtOwner = acMessage.CurrentActorData.IsDead;
                pRemoteComponent->DeadHere = cDeadHere;
            }

            spdlog::warn("Declined ownership of actor {:X} at epoch {} because the actor is not ready", acMessage.ServerId, acMessage.OwnershipEpoch);
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        Takeover::NoteGainedMoving(pActor, m_world.try_get<InterpolationComponent>(cEntity));
        Takeover::WatchGainedLegs(pActor);
        Takeover::LogGain(pActor, m_world.try_get<InterpolationComponent>(cEntity));

        // Reconcile while hooks still treat the actor as remote/non-authoritative.
        pActor->GetExtension()->SetRemote(true);
        m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        // This player's own follower coming back: her gear here is the truth (see OnAssignCharacter).
        const bool cMyFollower = pActor->IsPlayerFollower();
        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, acMessage.CurrentActorData, !cMyFollower, true);
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);
        CarryOnFromLastOwner(m_world, pActor, acMessage);

        DeleteRemoteEntityComponents(cEntity);
        CacheSystem::Setup(m_world, cEntity, pActor);
        m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);
        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        localComponent.IsDead = acMessage.CurrentActorData.IsDead;
        localComponent.IsWeaponDrawn = acMessage.CurrentActorData.IsWeaponDrawn;

        // LocalComponent is installed only after canonical reconciliation is complete.
        pActor->GetExtension()->SetRemote(false);
        // Run here now: no longer another player's follower shown here.
        pActor->GetExtension()->SetOthersFollower(false);
        InterpolationSystem::Forget(pActor->formID);
        // A dragon taken over in the air flies on: its flight state is set again through the game, so that its character
        // controller is the flying one whatever the copy had, and its AI carries the flight on as it would alone.
        if (pActor->IsDragon() && !pActor->IsDead())
        {
            const uint32_t cFly = FlyState::Of(pActor->actorState.flags1);
            // Without a movement controller the game's setter does nothing, and the cleared bits would put it on the ground.
            if (FlyState::InTheAir(cFly) && pActor->movementDrivenControl)
            {
                pActor->actorState.flags1 &= ~FlyState::kMask;
                pActor->SetFlyState(cFly);
            }
            // Every dragon, on the ground too: a copy whose flight the handlers here missed reads as on the ground.
            spdlog::debug(
                "Gained dragon {:X} (server id {:X}), flight state {}{}", pActor->formID, acMessage.ServerId, cFly,
                FlyState::InTheAir(cFly) ? ": in the air, it flies on here" : "");
        }
        // Its last owner's bones stop here: left in place, they would hold the body in that pose.
        VRBodySync::ClearRemotePose(pActor->formID);
        spdlog::info("Gained ownership of actor {:X} (form {:X}) at epoch {}", acMessage.ServerId, pActor ? pActor->formID : 0, acMessage.OwnershipEpoch);
        if (pActor)
            CopyRegistry::ForgetAwayFromOwner(pActor->formID);
        if (standInFor)
            spdlog::info("Hand-off of {:X} (server id {:X}) taken on the copy standing in for reference {:X} here", pActor->formID, acMessage.ServerId, standInFor);
        // Its last owner's target was only followed by the head here; its own AI takes the fight on now.
        m_world.GetCombatService().ForgetCopyFight(cEntity, pActor, "this game took it over");
        m_world.GetCombatService().CarryOnFight(pActor, acMessage.ServerId, acMessage.CombatTarget);
        if (cMyFollower)
            SendInventoryDifference(pActor, acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentActorData.InitialInventory);

        // Asked for after this player's killing blow (ClaimForKillingBlow): it dies here, with this player as the killer.
        if (const auto blow = m_killingBlows.find(acMessage.ServerId); blow != m_killingBlows.end())
        {
            const bool cInTime = std::chrono::steady_clock::now() < blow->second;
            m_killingBlows.erase(blow);
            if (cInTime && !pActor->IsDead())
            {
                pActor->Kill(PlayerCharacter::Get());
                spdlog::info("KillingBlow: {:X} (server id {:X}) killed here by this player, its killer (now dead: {})", pActor->formID, acMessage.ServerId, pActor->IsDead());
            }
            else
                spdlog::info("KillingBlow: {:X} (server id {:X}) came too late or already dead; kept as it is", pActor->formID, acMessage.ServerId);
        }

        // Asked for by ReviveActor; the death sync takes it from here.
        if (const auto revival = m_revivals.find(acMessage.ServerId); revival != m_revivals.end())
        {
            const bool cInTime = std::chrono::steady_clock::now() < revival->second;
            m_revivals.erase(revival);
            if (cInTime && pActor->IsDead())
                pActor->Respawn();
            spdlog::info("Revive: {:X} (server id {:X}) came to this game {}; now dead: {}", pActor->formID, acMessage.ServerId, cInTime ? "in time" : "too late", pActor->IsDead());
        }

        // Asked for by ClaimToRaise: raised here, by this player, as the game would, if it came in time (on the main thread).
        s_raises.HandedOver(acMessage.ServerId, pActor->IsDead(), RaiseClaim::Clock::now());
        return;
    }

    // Killed here while the grant has it alive: the server has not taken its death (the death sync reports every 250 ms,
    // and a report that crossed the grant is dropped as an old epoch). Its new owner is sent a killing blow, as for a body
    // lying dead here (DeathWins), and it stays dead here: a unique one stood up again. A protected or essential one refuses
    // a blow with no killer, so it is left as before.
    const auto* pLocalComponent = m_world.try_get<LocalComponent>(cEntity);
    const bool cKilledHere =
        pLocalComponent && pActor && GhostDeathPolicy::KilledBeforeHandOff(pActor->IsDead(), acMessage.CurrentActorData.IsDead, pActor->IsProtected() || pActor->IsEssential());
    if (cKilledHere)
        SendKillingBlowToOwner(m_transport, pActor, acMessage.ServerId);

    if (pActor)
        pActor->GetExtension()->SetRemote(true);

    m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);

    if (pFormIdComponent)
    {
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        // The new owner's snapshots carry on from its own view of the actor, so the previous owner's would only drag it back.
        // Playback starts over from where the actor is now.
        const auto* pOldInterpolation = m_world.try_get<InterpolationComponent>(cEntity);
        const glm::vec3 cPosition = pActor ? glm::vec3(pActor->position) : (pOldInterpolation ? pOldInterpolation->Position : glm::vec3{});
        const bool cForeignGraph = pOldInterpolation && pOldInterpolation->ForeignGraph;
        auto& interpolation = InterpolationSystem::Setup(m_world, cEntity);
        interpolation.Position = cPosition;
        interpolation.ForeignGraph = cForeignGraph;
        if (!m_world.all_of<RemoteAnimationComponent>(cEntity))
            AnimationSystem::Setup(m_world, cEntity);
    }
    else if (m_world.all_of<RemoteComponent>(cEntity))
    {
        m_world.patch<RemoteComponent>(cEntity, [&](RemoteComponent& aRemote) { aRemote.OwnershipEpoch = acMessage.OwnershipEpoch; });
    }
    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
    {
        pRemoteComponent->DeadAtOwner = acMessage.CurrentActorData.IsDead;
        pRemoteComponent->DeadHere = cKilledHere;
    }

    // This player's own follower given to somebody else (the server does that during a loading screen): her gear here is kept.
    const bool cMyFollower = pActor && pActor->IsPlayerFollower();
    ActorData actorData = acMessage.CurrentActorData;
    actorData.IsDead = actorData.IsDead || cKilledHere;
    ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, actorData, pActor && pActor->GetNiNode() && !cMyFollower, false);
    if (pActor)
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId, &acMessage.CurrentActorData.InitialInventory);

    spdlog::info("Actor {:X} is now owned by player {:X} at epoch {}", acMessage.ServerId, acMessage.OwnerPlayerId, acMessage.OwnershipEpoch);
    // She is asked for back only while loaded and near the player; this game cannot run one left behind and would hand her back.
    // One that catches up later is asked for then (OnAssignCharacter, RunFollowerReclaims).
    if (cMyFollower)
    {
        const auto* pPlayer = PlayerCharacter::Get();
        const bool cHere = pActor->GetNiNode() && pPlayer && glm::distance(glm::vec3(pActor->position), glm::vec3(pPlayer->position)) < 2048.f;
        spdlog::info("Follower {:X} (server id {:X}) was given to player {:X}; {}", pActor->formID, acMessage.ServerId, acMessage.OwnerPlayerId,
                     cHere ? "asked for back" : "not near this player, asked for once she is");
        if (cHere)
            RequestOwnership(pActor->formID, acMessage.ServerId, cEntity);
    }
}

void CharacterService::RunFollowerReclaims() const noexcept
{
    // Once a second, asks back each follower of this player that another game runs, once she is loaded and near.
    static std::chrono::steady_clock::time_point s_next;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_next || !m_transport.IsConnected())
        return;
    s_next = cNow + std::chrono::seconds(1);

    // Out of a party the server refuses it anyway, and it would only be asked again every few seconds.
    const auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !m_world.GetPartyService().IsInParty())
        return;

    static TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_asked;
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    for (const auto cEntity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
        if (!pActor || !pActor->IsPlayerFollower() || pActor->IsDead() || !pActor->GetNiNode())
            continue;
        if (glm::distance(glm::vec3(pActor->position), glm::vec3(pPlayer->position)) >= 2048.f)
            continue;
        auto& asked = s_asked[pActor->formID];
        if (cNow < asked)
            continue;
        asked = cNow + std::chrono::seconds(5);
        const auto& remote = view.get<RemoteComponent>(cEntity);
        spdlog::info("Follower {:X} (server id {:X}) is near this player again; asked for", pActor->formID, remote.Id);
        RequestOwnership(pActor->formID, remote.Id, cEntity);
    }
}

void CharacterService::RunFollowerReports() const noexcept
{
    // Once a second. A change counts after two checks: a script can switch the flag off and on again, and each report rebuilds her copy for the others.
    constexpr uint8_t cChecksToReport = 2;
    static std::chrono::steady_clock::time_point s_next;
    static uint64_t s_visit = 0;
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_next || !m_transport.IsConnected())
        return;
    s_next = cNow + std::chrono::seconds(1);
    ++s_visit;

    const auto report = [this](const entt::entity aEntity, const uint32_t aServerId)
    {
        const auto* pFormId = m_world.try_get<FormIdComponent>(aEntity);
        Actor* pActor = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        // Only one loaded here and alive (a grant for one with no 3D is declined, and a body is nobody's follower),
        // and never another player's follower shown here (FollowerCommand::MayReportFollower).
        if (!pExtension)
            return;
        FollowerCommand::ReportedActor reported{};
        reported.Temporary = pActor->IsTemporary();
        reported.IsThisPlayer = pActor->formID == 0x14;
        reported.RemotePlayer = pExtension->IsRemotePlayer();
        reported.OthersFollower = pExtension->IsOthersFollower();
        reported.Loaded = pActor->GetNiNode() != nullptr;
        reported.Dead = pActor->IsDead();
        if (!FollowerCommand::MayReportFollower(reported))
            return;

        auto& state = s_followerReports[aServerId];
        state.Visit = s_visit;
        const bool cFollower = pActor->IsPlayerFollower();
        if (cFollower == state.Told)
        {
            state.Differs = 0;
            return;
        }
        if (++state.Differs < cChecksToReport)
            return;

        state.Told = cFollower;
        state.Differs = 0;
        RequestFollowerState request{};
        request.ServerId = aServerId;
        request.IsFollower = cFollower;
        m_transport.Send(request);
        spdlog::info("Follower: told the server that {:X} (server id {:X}) {} this player's follower", pActor->formID, aServerId, cFollower ? "is" : "is no longer");
    };

    const auto localView = m_world.view<LocalComponent>();
    for (const auto cEntity : localView)
        report(cEntity, localView.get<LocalComponent>(cEntity).Id);
    const auto remoteView = m_world.view<RemoteComponent>();
    for (const auto cEntity : remoteView)
        report(cEntity, remoteView.get<RemoteComponent>(cEntity).Id);

    for (auto it = s_followerReports.begin(); it != s_followerReports.end();)
        it = it->second.Visit == s_visit ? std::next(it) : s_followerReports.erase(it);
}

void CharacterService::RunReleasedGhosts() const noexcept
{
    if (!s_pendingGhostEnables.empty() && !IsProcessExiting())
    {
        const auto cNow = std::chrono::steady_clock::now();
        std::erase_if(s_pendingGhostEnables, [cNow](const PendingGhostEnable& acPending)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(acPending.FormId));
            ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
            if (!pExtension || pActor->IsDeleted())
                return true;
            // Hidden again meanwhile: that one wants the disable, and a save still has it enabled.
            if (CopyRegistry::IsGhost(acPending.FormId))
            {
                pExtension->SetHiddenHere(true);
                return true;
            }
            // A levelled NPC being rebuilt is disabled on purpose, and the rebuild enables it itself.
            if (pExtension->Reconciliation != ActorExtension::ReconciliationStage::None)
            {
                pExtension->SetHiddenHere(false);
                return true;
            }
            if (pActor->IsStillLoading() || !pActor->IsDisabled())
            {
                if (cNow < acPending.GiveUpAt)
                    return false;
                pExtension->SetHiddenHere(false);
                return true;
            }
            pExtension->SetHiddenHere(false);
            pActor->EnableImpl();
            spdlog::info("Ghost: reference {:X} enabled, its disable having landed after it was released", acPending.FormId);
            return true;
        });
    }

    // A reference whose stand-in died is killed once it has its 3D, and only then announced, so that it reaches the server as a body.
    // One not in memory now is kept: it is killed when its cell loads it again, unless the cell may have reset since (GhostDeathPolicy).
    if (!s_ghostsToLayDown.empty() && !IsProcessExiting())
    {
        const auto formView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        const auto cNow = std::chrono::steady_clock::now();
        const float cDaysPassed = CurrentDaysPassed();
        std::erase_if(
            s_ghostsToLayDown,
            [this, &formView, cNow, cDaysPassed](const GhostToLayDown& acEntry)
            {
                const uint32_t cFormId = acEntry.FormId;
                Actor* pActor = Cast<Actor>(TESForm::GetById(cFormId));

                GhostDeathPolicy::Waiting waiting{};
                waiting.InMemory = pActor != nullptr;
                waiting.Expired = GhostDeathPolicy::HasExpired(acEntry.DaysPassed, cDaysPassed);
                waiting.Deleted = pActor && pActor->IsDeleted();
                // One the game is disposing of is not read any further.
                if (pActor && !waiting.Deleted)
                {
                    const auto cEntity = Utils::FindByFormId(formView, cFormId);
                    const ActorExtension* pExtension = pActor->GetExtension();
                    const auto* pBase = Cast<TESNPC>(pActor->baseForm);
                    waiting.Dead = pActor->IsDead();
                    waiting.TakenOver = CopyRegistry::IsGhost(cFormId) || (cEntity && m_world.any_of<LocalComponent, RemoteComponent, WaitingForAssignmentComponent>(*cEntity));
                    waiting.HiddenHere = pExtension && pExtension->IsHiddenHere();
                    waiting.Disabled = pActor->IsDisabled();
                    waiting.EnableMayLand = cNow - acEntry.Since < cEnableLandsWithin;
                    waiting.Unique = pBase && pBase->actorData.IsUnique();
                    waiting.LifeFollowsOwner = s_lifeFollowsOwner.contains(cFormId);
                    waiting.Has3D = pActor->GetNiNode() && !pActor->IsStillLoading();
                }

                switch (GhostDeathPolicy::OnWaitingFrame(waiting))
                {
                case GhostDeathPolicy::Step::Wait: return false;
                case GhostDeathPolicy::Step::Forget: return true;
                case GhostDeathPolicy::Step::GiveUp:
                    spdlog::debug("Ghost: reference {:X} is no longer laid down: its cell may have reset since the owner's creature it stood in for died", cFormId);
                    if (pActor)
                        s_releasedGhosts.push_back(cFormId);
                    return true;
                case GhostDeathPolicy::Step::Announce: s_releasedGhosts.push_back(cFormId); return true;
                case GhostDeathPolicy::Step::Kill:
                    pActor->Kill();
                    s_releasedGhosts.push_back(cFormId);
                    spdlog::debug(
                        "Ghost: reference {:X} {}, as the owner's creature it stood in for died", cFormId,
                        pActor->IsDead() ? "laid down as a body" : "could not be killed and stays alive");
                    return true;
                }
                return false;
            });
    }

    if (s_releasedGhosts.empty())
        return;

    TiltedPhoques::Vector<uint32_t> released;
    released.swap(s_releasedGhosts);

    const auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const uint32_t cFormId : released)
    {
        // Announced once it is a body (above).
        if (IsWaitingToLayDown(cFormId))
            continue;
        if (const auto itor = Utils::FindByFormId(view, cFormId))
            ProcessNewEntity(*itor);
    }
}

void CharacterService::RetireOlderPlayerCopy(const uint32_t aPlayerId, const uint32_t aServerId, const entt::entity aNewEntity, Actor* apNew) const noexcept
{
    const uint32_t cNewFormId = apNew->formID;
    std::optional<PlayerCopyRules::Record> recorded = s_playerCopies.Remember({aPlayerId, cNewFormId, apNew->GetHandle().handle.iBits, aServerId});

    // Another server character of this player still here: removed as the server's removal of it would.
    Vector<std::pair<uint32_t, uint32_t>> olderSeen; // server id, form id
    const auto cPlayerView = m_world.view<PlayerComponent, RemoteComponent>();
    for (const auto cEntity : cPlayerView)
    {
        if (cEntity == aNewEntity || cPlayerView.get<PlayerComponent>(cEntity).Id != aPlayerId)
            continue;
        // A copy still waiting for its 3D has its id on the RemoteComponent only.
        const auto* pFormId = m_world.try_get<FormIdComponent>(cEntity);
        const auto& cRemote = cPlayerView.get<RemoteComponent>(cEntity);
        olderSeen.push_back({cRemote.Id, pFormId ? pFormId->Id : cRemote.CachedRefId});
    }
    for (const auto& [cServerId, cFormId] : olderSeen)
    {
        if (PlayerCopyRules::OnPlayerCopyMade(cFormId ? cFormId : cServerId, cNewFormId, true, true) != PlayerCopyRules::Older::kRemoveSeen)
            continue;
        spdlog::debug("Player copy: player {} has a new copy {:X}; the older one {:X} (server id {:X}) goes", aPlayerId, cNewFormId, cFormId, cServerId);
        // One on a horse this game put it on is taken off it first, and deleted after that.
        if (Actor* pOlder = cFormId && CopyRegistry::IsOwnCopy(cFormId) ? Cast<Actor>(TESForm::GetById(cFormId)) : nullptr; pOlder && !pOlder->IsDeleted())
            ReleaseThenRetire(cServerId, pOlder);
        NotifyRemoveCharacter removal{};
        removal.ServerId = cServerId;
        removal.Gone = false;
        OnRemoveCharacter(removal);
        if (recorded && cFormId == recorded->FormId)
            recorded.reset();
    }

    // The copy made before, no longer any server character's here but still standing: deleted as the copy this game made.
    // Only that same actor, a remote player's copy this game made: its form id can be given to another reference once it went.
    if (!recorded)
        return;
    Actor* pOlder = Cast<Actor>(TESForm::GetById(recorded->FormId));
    PlayerCopyRules::Found found{};
    if (pOlder && pOlder->IsTemporary() && !pOlder->IsDeleted())
    {
        found.Handle = pOlder->GetHandle().handle.iBits;
        found.OwnCopy = CopyRegistry::IsOwnCopy(recorded->FormId) && pOlder->GetExtension() && pOlder->GetExtension()->IsRemotePlayer();
        const auto cFormView = m_world.view<FormIdComponent, RemoteComponent>();
        found.Tracked = found.OwnCopy && Utils::FindByFormId(cFormView, recorded->FormId).has_value();
    }
    if (PlayerCopyRules::OnRememberedCopy(*recorded, aPlayerId, cNewFormId, found) == PlayerCopyRules::Older::kDelete)
    {
        spdlog::debug("Player copy: player {} has a new copy {:X}; the older one {:X}, no longer the server's here, is deleted", aPlayerId, cNewFormId, recorded->FormId);
        ReleaseThenRetire(recorded->ServerId, pOlder);
        DeleteTempActor(recorded->FormId);
    }
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept
{
    s_pendingLines.erase(acMessage.ServerId);
    s_sceneLines.erase(acMessage.ServerId);

    // Nothing told for it waits here any more, copy or not; announced again, its spawn data carries its limbs.
    uint32_t retiredForm = 0;
    if (const auto entity = Utils::FindEntityByServerId(acMessage.ServerId))
        if (const auto* pForm = m_world.try_get<FormIdComponent>(*entity))
            retiredForm = pForm->Id;
    StuckArrows::OnRetired(acMessage.ServerId, retiredForm);

    // The follower that kept this player's own one hidden is gone, or may no longer be another player's.
    // Announced again, it is hidden again if she still is.
    if (const auto hidden = s_hiddenFollowers.find(acMessage.ServerId); hidden != s_hiddenFollowers.end())
    {
        const uint32_t cFormId = hidden->second;
        s_hiddenFollowers.erase(hidden);
        // Only while no copy of hers stands in: deleting that copy brings the reference back itself.
        if (CopyRegistry::StandInFor(cFormId) == 0u)
        {
            CopyRegistry::ForgetGhost(cFormId);
            EnableGhost(cFormId);
        }
    }

    // A character gone from the server goes to every player, in range or not, so a record kept for one nobody has any more ends here.
    // One that only left this player's view is still on the server, and comes back as the same actor.
    if (const auto handed = s_handedAway.find(acMessage.ServerId); acMessage.Gone && handed != s_handedAway.end())
    {
        const uint32_t cOwnId = handed->second.FormId;
        s_handedAway.erase(handed);
        spdlog::info("HandedAway: server character {:X} is gone from the server; actor {:X} is an ordinary actor of this game again", acMessage.ServerId, cOwnId);

        // If it stands here unannounced, it is announced now.
        const auto formView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        if (const auto itor = Utils::FindByFormId(formView, cOwnId))
            ProcessNewEntity(*itor);
    }

    // A character the leader switched off leaves the server while its owner still runs it.
    // Its owner lets go of it here, so that the actor can be announced again when it is switched back on.
    if (acMessage.Gone)
    {
        const auto localView = m_world.view<LocalComponent>();
        const auto localIt = Utils::FindById<LocalComponent>(localView, acMessage.ServerId);
        if (localIt)
        {
            const auto* pFormId = m_world.try_get<FormIdComponent>(*localIt);
            const uint32_t cFormId = pFormId ? pFormId->Id : 0;
            const bool cDeadHere = localView.get<LocalComponent>(*localIt).IsDead;
            m_world.remove<LocalAnimationComponent, LocalComponent>(*localIt);
            // A copy of ours that this game ran is nothing to announce again: it goes, and a reference it stood in for comes back.
            if (cFormId && CopyRegistry::IsOwnCopy(cFormId))
            {
                // Freed by the game already, or marked deleted, the copy no longer says whether it died; this game's word, from when it ran it, does.
                // A copy still standing is read by DeleteTempActor itself.
                Actor* pCopy = Cast<Actor>(TESForm::GetById(cFormId));
                const bool cCopyGone = !pCopy || pCopy->IsDeleted();
                const bool cDeadCopy = cCopyGone && cDeadHere;
                if (GhostDeathPolicy::OnStandInGone(cDeadCopy, IsFollowerCopyId(cFormId, pCopy)) == GhostDeathPolicy::Action::LayDown)
                {
                    if (const auto cEntry = CopyRegistry::StandInEntryOf(cFormId); cEntry && cEntry->second == cFormId)
                        LayDownWhenReleased(cEntry->first, cFormId);
                }
                spdlog::info("Character {:X} is gone from the server; copy {:X} this game ran goes too", acMessage.ServerId, cFormId);
                CharacterService::DeleteTempActor(cFormId);
            }
            return;
        }
    }

    auto view = m_world.view<RemoteComponent>();

    const auto itor = Utils::FindById<RemoteComponent>(view, acMessage.ServerId);

    if (itor)
    {
        const entt::entity cEntity = *itor;

        // A copy whose 3D has not loaded yet has no FormIdComponent; the spawn put its id on the RemoteComponent.
        // Left behind, it would be taken for an actor of this game's own, and stay without its gear.
        const auto& cRemote = view.get<RemoteComponent>(*itor);
        const bool cDeadAtOwner = cRemote.DeadAtOwner;
        uint32_t formId = 0;
        if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor))
            formId = pFormIdComponent->Id;
        else if (Actor* pCopy = Cast<Actor>(TESForm::GetById(cRemote.CachedRefId)); pCopy && pCopy->IsTemporary() &&
                 (!cRemote.CachedBaseId || (pCopy->baseForm && pCopy->baseForm->formID == cRemote.CachedBaseId)))
        {
            formId = cRemote.CachedRefId;
            spdlog::info("Removed before loading: server character {:X} is gone before its copy {:X} loaded; the copy goes too", acMessage.ServerId, formId);
        }
        // Its copy is gone already, and with it what would have told which reference it hid.
        else if (const auto* pWaiting = m_world.try_get<WaitingFor3D>(*itor); pWaiting && pWaiting->SpawnRequest.FormId != GameId{})
        {
            const uint32_t cRefId = m_world.GetModSystem().GetGameId(pWaiting->SpawnRequest.FormId);
            const bool cFollowerCopy = pWaiting->SpawnRequest.FollowerOfPlayerId && pWaiting->SpawnRequest.FollowerOfPlayerId != m_transport.GetLocalPlayerId();
            if (CopyRegistry::ForgetGhost(cRefId))
            {
                if (GhostDeathPolicy::OnStandInGone(cDeadAtOwner, cFollowerCopy) == GhostDeathPolicy::Action::LayDown)
                    LayDownWhenReleased(cRefId, 0);
                EnableGhost(cRefId);
            }
        }

        // A copy dead at its owner may not be dead here yet: one without its 3D is killed only once it has it.
        // One killed here before its hand-off is a body too, while its new owner's death has not come.
        if (formId && (cDeadAtOwner || cRemote.DeadHere) && TESForm::IsTemporaryId(formId))
        {
            if (const auto cEntry = CopyRegistry::StandInEntryOf(formId); cEntry && cEntry->second == formId)
            {
                Actor* pCopy = Cast<Actor>(TESForm::GetById(formId));
                if (GhostDeathPolicy::OnStandInGone(true, IsFollowerCopyId(formId, pCopy)) == GhostDeathPolicy::Action::LayDown)
                    LayDownWhenReleased(cEntry->first, formId);
            }
        }

        bool announceGameActor = false;
        if (formId)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
            if (TESForm::IsTemporaryId(formId))
            {
                // Only a copy this client made is deleted. A temporary actor this game's own scripts made can be remote here (it stands
                // for the character another game spawned first for the same event): it is the game's, and its scene may still run it.
                const bool cDeleted = pActor && pActor->IsDeleted();
                const bool cStands = pActor && !cDeleted && pActor->GetNiNode() && !pActor->IsDisabled();
                const auto cWhat = CopyLifetime::OnServerRemoval(pActor != nullptr, CopyRegistry::IsOwnCopy(formId), cDeleted, acMessage.Gone, cDeadAtOwner, cStands);
                if (cWhat == CopyLifetime::ServerRemoval::RemoveCopy)
                    CharacterService::DeleteTempActor(formId);
                else
                {
                    if (ActorExtension* pExtension = pActor->GetExtension())
                        pExtension->SetRemote(false);
                    VRBodySync::ClearRemotePose(formId);
                    // Still that server character, only out of this player's view: it comes back as this same actor, and is not
                    // announced as a new one meanwhile.
                    const bool cRemembered = cWhat == CopyLifetime::ServerRemoval::KeepAsHandedAway && pActor->baseForm;
                    if (cRemembered)
                        s_handedAway[acMessage.ServerId] = HandedAway{formId, pActor->baseForm->formID};
                    announceGameActor = cWhat == CopyLifetime::ServerRemoval::AnnounceAgain;
                    const char* cpThen = announceGameActor ? "; announced again from here" : "";
                    if (cRemembered)
                        cpThen = "; remembered as handed away";
                    spdlog::info("Character {:X} removed by the server; {:X} is this game's own temporary actor, left to the game{}", acMessage.ServerId, formId, cpThen);
                }
            }
            else if (pActor)
            {
                if (acMessage.Gone && !cDeadAtOwner)
                {
                    Takeover::NoteGainedMoving(pActor, m_world.try_get<InterpolationComponent>(cEntity));
                    Takeover::WatchGainedLegs(pActor);
                    // TEMPORARY: the action trail of an NPC gained because its owner left.
                    Takeover::LogTrailAtGain(pActor, " (its owner left)");
                }
                pActor->GetExtension()->SetRemote(false);
                VRBodySync::ClearRemotePose(formId);
            }
        }

        DeleteRemoteEntityComponents(cEntity);

        if (announceGameActor && m_world.all_of<FormIdComponent>(cEntity))
        {
            // As for a placed reference announced again below: nothing kept for it as another game's goes on.
            s_teleportsRefused.erase(formId);
            InterpolationSystem::Forget(formId);
            ProcessNewEntity(cEntity);
            return;
        }

        // Gone for everybody while it stands here, so this game runs it now, and unannounced nobody else would see it.
        // A follower's own game can drop her at a gate while her reference walks on in the other game.
        // One dead at its owner stays out of this: announced from a copy that missed the death, it would come back alive.
        // One being rebuilt for a pick has no 3D for a moment, and discovery will not bring it back.
        Actor* pReference = Cast<Actor>(TESForm::GetById(formId));
        const bool cOwnReference = pReference && !pReference->IsTemporary();
        const bool cRebuilding = cOwnReference && pReference->GetExtension()->Reconciliation != ActorExtension::ReconciliationStage::None;
        if (acMessage.Gone && !cDeadAtOwner && cOwnReference && (cRebuilding || (pReference->GetNiNode() && !pReference->IsDisabled())) && m_world.all_of<FormIdComponent>(cEntity))
        {
            spdlog::info("Character {:X} is gone from the server while {:X} stands here; announced again from this game", acMessage.ServerId, formId);
            s_teleportsRefused.erase(formId);
            InterpolationSystem::Forget(formId);
            // A pick not started on belonged to the owner that is gone; this game's own stands now.
            if (!cRebuilding)
            {
                m_pendingLeveledConforms.erase(formId);
                m_conformEquipment.erase(formId);
            }
            ProcessNewEntity(cEntity);
            return;
        }

        // A reference standing here that its owner took through a door: announced again, it comes back with its owner's place.
        // Its assignment then moves it out (OnAssignCharacter).
        if (const auto cRefused = s_teleportsRefused.find(formId); cRefused != s_teleportsRefused.end())
        {
            const bool cJustNow = std::chrono::steady_clock::now() - cRefused->second < std::chrono::seconds(2);
            // This player's own follower stays a live actor here: an assignment would not move her, and she would stand frozen.
            const Actor* pReference = Cast<Actor>(TESForm::GetById(formId));
            if (cJustNow && pReference && !pReference->IsPlayerFollower() && m_world.all_of<FormIdComponent>(cEntity))
                ProcessNewEntity(cEntity); // the mark stays for its assignment
            else
                s_teleportsRefused.erase(cRefused);
        }
    }
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = Utils::FindById<RemoteComponent>(view, acMessage.ActorId);

    if (!entityIt)
    {
        spdlog::error("Actor to respawn not found in: {:X}", acMessage.ActorId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);

    CancelServerAssignment(*entityIt, formIdComponent.Id, Cast<Actor>(TESForm::GetById(formIdComponent.Id)));

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;

    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto it = Utils::FindByFormId(view, 0x14);

    // The player's entity may not exist yet, or may already be torn down.
    if (!it)
    {
        spdlog::warn("{}: the player has no entity right now; beast form change not sent", __FUNCTION__);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*it);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();

    m_transport.Send(request);
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto actorIt = Utils::FindByFormId(view, acEvent.ActorId);

    if (!actorIt)
        return;

    const entt::entity cActorEntity = *actorIt;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = Utils::FindById<RemoteComponent>(remoteView, acMessage.ActorId);

    if (!remoteIt)
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);
    if (!pActor)
    {
        // The copy is gone or the form is no actor any more: there is nothing to give the package to.
        spdlog::warn("Actor for package with remote id {:X} is no longer an actor here.", acMessage.ActorId);
        return;
    }

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);
    if (!pPackage)
    {
        // The id resolved to a form that is not a package (a mod mismatch); SetPackage must not get null.
        spdlog::warn("Form {:X} is not a package; leaving actor {:X} on the package it has", cPackageFormId, pActor->formID);
        return;
    }

    pActor->SetPackage(pPackage);
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

/**
 * Send a line a shared character speaks in this game to the other players, with its scene when it has one.
 * The same line spoken twice within a second goes once.
 *
 * @param acEvent The spoken line.
 */
void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // The game often speaks a line twice in a row, and each copy sent made the other game stop the line and start it again.
    static uint32_t s_lastActor = 0;
    static String s_lastFile;
    static std::chrono::steady_clock::time_point s_lastAt{};
    const auto cNow = std::chrono::steady_clock::now();
    if (acEvent.ActorID == s_lastActor && acEvent.VoiceFile == s_lastFile && cNow - s_lastAt < 1s)
        return;
    s_lastActor = acEvent.ActorID;
    s_lastFile = acEvent.VoiceFile;
    s_lastAt = cNow;

    const auto cServerId = SpeakerServerId(m_world, acEvent.ActorID, __FUNCTION__);
    if (!cServerId)
        return;

    DialogueRequest request{};
    request.ServerId = *cServerId;
    request.SoundFilename = acEvent.VoiceFile;
    if (acEvent.SceneID && m_world.GetModSystem().GetServerModId(acEvent.SceneID, request.SceneId))
        request.ScenePhase = acEvent.ScenePhase;

    // Logged so that a line the other player did not hear can be traced.
    spdlog::info("Dialogue: {:X} (server id {:X}) says {}{}; sent", acEvent.ActorID, request.ServerId, acEvent.VoiceFile.c_str(),
                 acEvent.SceneID ? fmt::format(" in scene {:X} phase {}", acEvent.SceneID, acEvent.ScenePhase) : std::string());
    m_transport.Send(request);
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    s_sceneLines.erase(acMessage.ServerId);
    s_pendingLines.erase(acMessage.ServerId);

    const auto cNow = PendingVoice::Clock::now();
    PendingLine line{acMessage.SoundFilename, acMessage.SceneId, acMessage.ScenePhase, {cNow}};
    if (!PlayOrDeferLine(acMessage.ServerId, line, cNow) && s_pendingLines.size() < kMaxPendingLines)
        s_pendingLines.emplace(acMessage.ServerId, std::move(line));
}

/**
 * Send a subtitle a shared character shows in this game to the other players, with its scene when it has one.
 *
 * @param acEvent The subtitle.
 */
void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const auto cServerId = SpeakerServerId(m_world, acEvent.SpeakerID, __FUNCTION__);
    if (!cServerId)
        return;

    SubtitleRequest request{};
    request.ServerId = *cServerId;
    request.Text = acEvent.Text;
    request.TopicFormId = acEvent.TopicFormID;
    if (acEvent.SceneID && m_world.GetModSystem().GetServerModId(acEvent.SceneID, request.SceneId))
        request.ScenePhase = acEvent.ScenePhase;

    m_transport.Send(request);
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
        return;

    // The local copy of the scene shows its own subtitle, see OnNotifyDialogue.
    // Only the actor's own voice line ending hides a subtitle, so a remote one arriving after the local line ended would stay up.
    if (IsLineVoicedByLocalScene(pActor, acMessage.SceneId, acMessage.ScenePhase))
        return;

    // This is only for fallout 4
    TESTopicInfo* pInfo = nullptr;
    pInfo = Cast<TESTopicInfo>(TESForm::GetById(acMessage.TopicFormId));

    SubtitleManager::Get()->ShowSubtitle(pActor, acMessage.Text.c_str(), pInfo);
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

    // A reference hidden behind a stand-in stays where this game's save has it; the stand-in follows the owner's reports.
    if (CopyRegistry::IsGhost(cActorId))
    {
        spdlog::debug("Teleport of {:X} not made: a stand-in shows it here", cActorId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor)
    {
        // A reference not in memory here, its cell not being loaded, or of a plugin this game lacks: there is nothing to move.
        // Routine after a join, when the other game's NPCs far from here are told: its assignment places it if it comes into view.
        spdlog::debug("Teleport of {:X} (server form {:X}:{:X}) not made: no such actor in memory here", cActorId, acMessage.FormId.ModId, acMessage.FormId.BaseId);
        return;
    }

    // A teleport this player cannot see is not made: an actor not loaded here, going to a place not loaded here.
    // Moving it there would load a far exterior cell, whose streaming can still run once the game has dropped the cell.
    // Nor is one out to the exterior beyond the loaded area: the game reads that cell from disk at once, in this frame.
    // The server keeps where the actor is; an actor that comes into view here later is placed by its assignment.
    const bool cPlaceLoaded = IsPlaceLoadedHere(m_world, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
    const bool cActorLoaded = IsActorLoadedHere(pActor);
    if (!cPlaceLoaded && (!cActorLoaded || acMessage.WorldSpaceId))
    {
        spdlog::debug(
            "Teleport of actor {:X} not made: {} (world space {:X}, cell {:X}, position ({:.0f}))", pActor->formID,
            cActorLoaded ? "it goes out beyond the loaded area here" : "it is not loaded here, nor is the place it goes to", acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId,
            acMessage.Position);
        if (cActorLoaded && pActor->GetExtension() && pActor->GetExtension()->IsRemote())
            CopyRegistry::NoteAwayFromOwner(pActor->formID, OwnerPlaceOf(m_world, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position));
        // Only from an interior, to a world this game has: the assignment moves nothing else, and a copy left here unmoved would stand frozen.
        const auto* pTargetWorld = Cast<TESWorldSpace>(TESForm::GetById(modSystem.GetGameId(acMessage.WorldSpaceId)));
        if (cActorLoaded && !pActor->GetWorldSpace() && pTargetWorld && pTargetWorld->GetPersistentCell())
        {
            if (s_teleportsRefused.size() > 256)
                s_teleportsRefused.clear();
            s_teleportsRefused[pActor->formID] = std::chrono::steady_clock::now();
        }
        return;
    }

    if (MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position))
        spdlog::debug(
            "Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({})", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId,
            acMessage.Position);
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Takes ownership of all actors
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

bool CharacterService::MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
    }
    // In case of lazy-loading of exterior cells
    else
    {
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error(__FUNCTION__ ": failed to fetch cell to teleport, actor: {:X}, worldspace: {:X}, cell: {:X}, position: {}", apActor->formID, acWorldSpaceId.BaseId, acCellId.BaseId, acPosition);
        return false;
    }

    // MoveTo drops the actor's 3D for a frame; this keeps discovery from taking that for a remote copy leaving.
    DiscoveryService::NoteMovedHere(apActor->formID);
    if (apActor->IsTemporary())
        LogCopyLife("moved (teleport)", apActor->formID, apActor);
    apActor->MoveTo(pCell, acPosition);
    CopyRegistry::ForgetAwayFromOwner(apActor->formID);
    return true;
}

// The party leader's game takes an NPC near its own player, and leaves one farther away with the game that runs it.
// Taken at any distance, an NPC at the edge of both games' loaded areas went back and forth: claimed here, given up as it left this game's area, announced again by the other.
constexpr float cLeaderClaimDistance = 6000.f;

// A placed NPC alive at its owner: a temporary actor is not the same one in another game, and a body stays with its owner.
static bool IsLeaderClaimable(const Actor* apActor, const RemoteComponent& acRemote) noexcept
{
    return !apActor->IsTemporary() && !apActor->IsMount() && !apActor->IsDead() && !acRemote.DeadAtOwner;
}

// Measured where its owner last reported it, which can be far from where this game's reference was.
static bool IsNearLocalPlayer(const Actor* apActor, const InterpolationComponent* apInterpolation) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    // A whole interior is loaded, so it has no edge to go back and forth across: inside the player's cell every NPC is near, as before.
    const TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    if (!pWorldSpace)
        return apActor->parentCell == pPlayer->parentCell;
    if (apActor->GetWorldSpace() != pWorldSpace)
        return false;

    glm::vec3 position(apActor->position);
    if (apInterpolation)
        position = apInterpolation->TimePoints.empty() ? apInterpolation->Position : apInterpolation->TimePoints.back().Position;
    return glm::distance(position, glm::vec3(pPlayer->position)) <= cLeaderClaimDistance;
}

void CharacterService::RunLeaderClaims() const noexcept
{
    static std::chrono::steady_clock::time_point s_nextCheck{};
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_nextCheck)
        return;
    s_nextCheck = cNow + std::chrono::seconds(1);

    if (!m_transport.IsOnline() || !m_world.GetPartyService().IsLeader())
        return;

    auto view = m_world.view<FormIdComponent, RemoteComponent>(entt::exclude<WaitingFor3D>);
    for (const auto cEntity : view)
    {
        auto& remoteComponent = view.get<RemoteComponent>(cEntity);
        if (!remoteComponent.ClaimWhenNear)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
        if (!pActor || pActor->IsDisabled() || !pActor->GetNiNode() || !IsLeaderClaimable(pActor, remoteComponent) || !IsNearLocalPlayer(pActor, m_world.try_get<InterpolationComponent>(cEntity)))
            continue;

        // One try, as on arrival: a claim refused here is not repeated every second.
        remoteComponent.ClaimWhenNear = false;
        spdlog::info("Sending ownership claim for actor {:X} with server id {:X}; it came within {:.0f} units of this player", pActor->formID, remoteComponent.Id, cLeaderClaimDistance);
        RequestOwnership(pActor->formID, remoteComponent.Id, cEntity);
    }
}

void CharacterService::ProcessNewEntity(entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": actor for new entity not found, form id: {:X}", formIdComponent.Id);
        return;
    }

    // A ghost: a copy of the owner's creature is bound to the server in its place (see CopyRegistry).
    // One that was not loaded when the copy was made turns up here, enabled, once its cell loads, and is disabled now.
    if (CopyRegistry::IsGhost(formIdComponent.Id))
    {
        // Only a loaded one, as StandInForForeignCreature: this also runs for a reference just released, which may have no 3D yet.
        if (pActor && !pActor->IsDisabled() && pActor->GetNiNode() && !pActor->IsDeleted() && !pActor->IsStillLoading() && !IsProcessExiting())
        {
            HideGhost(pActor);
            spdlog::info("Ghost: reference {:X} loaded after its stand-in; disabled", formIdComponent.Id);
        }
        return;
    }

    // A reference whose stand-in died is announced once it is a body (RunReleasedGhosts): announced alive, it would be a living creature at the server.
    if (IsWaitingToLayDown(formIdComponent.Id))
        return;

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // TODO(cosideci): don't just take all actors (i.e. from other parties),
        // maybe check it server side, add a variable to the request.
        // A body stays with its owner: a hand on it is what asks for it (IsRemoteBodyGrabbedHere).
        if (m_world.GetPartyService().IsLeader() && IsLeaderClaimable(pActor, *pRemoteComponent))
        {
            // A player's own follower is claimed at any distance, as before: the server decides that claim by its follower rules.
            if (pActor->IsPlayerFollower() || IsNearLocalPlayer(pActor, m_world.try_get<InterpolationComponent>(aEntity)))
            {
                pRemoteComponent->ClaimWhenNear = false;
                spdlog::info("Sending ownership claim for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);

                RequestOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
                return;
            }

            pRemoteComponent->ClaimWhenNear = true;
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}; farther than {:.0f} units from this player, claimed if it comes nearer", pActor->formID,
                         pRemoteComponent->Id, cLeaderClaimDistance);
            return;
        }

        spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);
        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    // A removed copy waits, switched on if it had no 3D, until the game lets go of it (RunWaitingCopies).
    // Its cell loading again gives it 3D; announced, it would become this game's NPC and a nameless copy in the other game.
    // It is not announced, it is switched off at the start of a frame, and its wait starts again now that the game runs it.
    if (WaitingCopy* pWaiting = pActor->IsTemporary() ? FindWaitingCopy(pActor) : nullptr)
    {
        spdlog::info("OwnCopy: {:X} came back with its cell after its removal; not announced, to be switched off", pActor->formID);
        pWaiting->QueuedAt = std::chrono::steady_clock::now();
        FrameStartSwitchOff::Queue(pActor);
        return;
    }

    // A copy whose removal waits for a load to end (DeleteTempActor) is not announced either; it goes once the load is over.
    if (pActor->IsTemporary() && !s_removalsAfterLoad.empty())
    {
        const uint32_t cHandle = pActor->GetHandle().handle.iBits;
        if (std::any_of(
                s_removalsAfterLoad.begin(), s_removalsAfterLoad.end(),
                [pActor, cHandle](const CopyLifetime::LaterRemoval& acKept) { return acKept.FormId == pActor->formID && acKept.Handle == cHandle; }))
        {
            spdlog::info("OwnCopy: {:X} came back with its cell while its removal waits for the load to end; not announced", pActor->formID);
            return;
        }
    }

    // One of this game's own actors given to another player while out of view (see s_handedAway).
    // The server still has it and sends it when in range; announcing it here would duplicate it for everybody.
    if (uint32_t serverId = 0; IsHandedAway(pActor, serverId))
    {
        const auto remoteView = m_world.view<RemoteComponent>();
        const bool cCopyStands = std::any_of(std::begin(remoteView), std::end(remoteView), [remoteView, serverId](entt::entity aOther) { return remoteView.get<RemoteComponent>(aOther).Id == serverId; });
        if (cCopyStands)
            spdlog::warn("HandedAway: actor {:X} is back in view after the server sent character {:X}, so a copy of it stands here too; not handled, both stay", pActor->formID,
                         serverId);
        else
            spdlog::info("HandedAway: actor {:X} is back in view and is still server character {:X}; not announced as a new one, the server sends it when it is in range",
                         pActor->formID, serverId);
        return;
    }

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    RequestServerAssignment(aEntity);
}

void CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return;

    // A home's mannequin keeps each player's own display, unless the server shares homes whole (SharedHomes).
    if (pActor->race && pActor->race->formID == SharedHomes::kMannequinRace && !m_world.GetServerSettings().SyncPlayerHomes && ObjectService::IsPlayerHome(pActor->parentCell))
    {
        spdlog::debug("Mannequin {:X} in a player home stays this game's own; not announced", pActor->formID);
        return;
    }

    // Switched on here by the leader's game a moment ago: that game announces it, and this one's actor becomes its copy.
    // Announced from here first, it stood where this game had it, not where the leader's plugin put it, and was this game's.
    if (WaitsForLeader(pActor, m_world.GetPartyService().IsLeader()))
    {
        if (std::find(s_notAnnounced.begin(), s_notAnnounced.end(), aEntity) == s_notAnnounced.end())
            s_notAnnounced.push_back(aEntity);
        spdlog::info("Actor {:X} was switched on by the leader's game; waiting up to {} s for its character before announcing it here", pActor->formID,
                     std::chrono::duration_cast<std::chrono::seconds>(cWaitForLeader).count());
        return;
    }

    AssignCharacterRequest message{};

    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::error("Server reference id not found for form id {:X}", formIdComponent.Id);
        return;
    }

    // An actor the game holds outside any cell (taken out of the world, or not placed yet) is not announced.
    if (!pActor->parentCell)
    {
        spdlog::info("Actor {:X} has no cell; not announced to the server", pActor->formID);
        if (std::find(s_notAnnounced.begin(), s_notAnnounced.end(), aEntity) == s_notAnnounced.end())
            s_notAnnounced.push_back(aEntity);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pActor->parentCell->formID);
        return;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    // Serialize the base form
    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->IsTemporary();

    if (isPlayer)
    {
        pNpc->MarkChanged(0x2000800);
    }

    const auto changeFlags = pNpc->GetChangeFlags();

    if (isPlayer || changeFlags != 0)
    {
        message.ChangeFlags = changeFlags;
        pNpc->Serialize(&message.AppearanceBuffer);
    }

    if (isPlayer)
        message.FaceTints = PlayerLook::FaceTints();

    if (isPlayer)
    {
        auto& questLog = message.QuestContent.Entries;
        auto& modSystem = m_world.GetModSystem();

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            auto* pQuest = objective.instance->quest;
            if (!pQuest)
                continue;

            if (!QuestService::IsNonSyncableQuest(pQuest))
            {
                GameId id{};

                if (modSystem.GetServerModId(pQuest->formID, id))
                {
                    auto& entry = questLog.emplace_back();
                    entry.Stage = pQuest->currentStage;
                    entry.Id = id;
                }
            }
        }

        // remove duplicates
        const auto ip = std::unique(questLog.begin(), questLog.end());
        questLog.resize(std::distance(questLog.begin(), ip));
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    message.IsPlayerSummon = pActor->GetCommandingActor() && pActor->GetCommandingActor()->formID == 0x14;
    message.IsFollower = !isPlayer && pActor->IsPlayerFollower() && !pActor->IsDead();

    if (const TESNPC* pPick = pActor->GetLeveledPick())
    {
        const uint32_t pickFormId = pPick->formID;
        if (m_world.GetModSystem().GetServerModId(pickFormId, message.LeveledNpcPickId))
            spdlog::info("Captured leveled NPC pick {:X} for actor {:X} (base {:X})", pickFormId, pActor->formID, pNpc->formID);
        else
            spdlog::warn("Leveled NPC pick {:X} has no server id, identity sync skipped", pickFormId);
    }
    else if (pNpc->IsTemporary())
    {
        spdlog::info("No leveled pick recoverable for temp base {:X} (actor {:X}), identity sync unavailable", pNpc->formID, pActor->formID);
    }

    if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    // The base form is sent for every actor but the player (the server rejects it there), not only temporary ones.
    // A levelled reference can roll as another creature on each machine; the base lets the other side tell them apart.
    if (pNpc && !isPlayer)
    {
        if (!m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId) && isTemporary)
        {
            spdlog::error("Server NPC form id not found for form id {:X}", pNpc->formID);
            return;
        }
    }

    // Serialize actions
    auto* const pExtension = pActor->GetExtension();

    message.LatestAction = pExtension->LatestAnimation;
    pActor->SaveAnimationVariables(message.LatestAction.Variables);
    AnimationSystem::AddModIds(m_world, message.LatestAction);

    spdlog::debug("Request id: {:X}, cookie: {:X}, entity: {:X}", formIdComponent.Id, sCookieSeed, to_integral(aEntity));

    if (m_transport.Send(message))
    {
        m_world.emplace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);

        sCookieSeed++;
    }
}

// apActor is the actor itself, null once the game has freed it: its id may already name another object.
void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId, Actor* apActor) const noexcept
{
    // Gone from the game, so its id may be given to an actor of the game's own next. See s_lastConnectionCopies.
    s_lastConnectionCopies.erase(aFormId);

    // TEMPORARY: logs what a temporary actor was when its removal was noticed (checked by live-copies-left, live-seeker-copy-*).
    const bool cTemporaryId = TESForm::IsTemporaryId(aFormId);
    const bool cGameMade = !CopyRegistry::IsOwnCopy(aFormId);
    // A copy marked deleted as its cell unloads, or freed by the game already, no longer says whether it died.
    // The game running its character does: the owner, or this game for a copy it took on.
    const auto* pDeadRemote = m_world.try_get<RemoteComponent>(aEntity);
    const auto* pDeadLocal = m_world.try_get<LocalComponent>(aEntity);
    const bool cRunnersWord = DeadByRunnersWord(pDeadRemote, pDeadLocal);
    const bool cCopyGone = !apActor || apActor->IsDeleted();
    const bool cFollowerCopy = IsFollowerCopyId(aFormId, apActor);
    s_followerCopies.erase(aFormId);
    const bool cDeadCopy = cTemporaryId && GhostDeathPolicy::OnStandInGone(GhostDeathPolicy::CopyWasDead(cCopyGone, apActor && apActor->IsDead(), cRunnersWord), cFollowerCopy) ==
                                               GhostDeathPolicy::Action::LayDown;
    if (cTemporaryId)
    {
        Actor* pSeen = apActor;
        spdlog::info("CopyGone: {:X} removed as {}; the actor is {}; made by this client: {}", aFormId,
                     m_world.all_of<RemoteComponent>(aEntity) ? "remote" : m_world.all_of<LocalComponent>(aEntity) ? "local" : m_world.all_of<WaitingForAssignmentComponent>(aEntity) ? "waiting" : "untracked",
                     pSeen ? (pSeen->IsDeleted() ? "still there, marked deleted" : pSeen->GetNiNode() ? "still there, with 3D" : "still there, no 3D") : "gone from the game already",
                     CopyRegistry::IsOwnCopy(aFormId) ? "yes" : "no");

        // TEMPORARY: why a dead copy near the player loses its 3D, the body vanishing until it is spawned again.
        if (pSeen && m_world.all_of<RemoteComponent>(aEntity) && pSeen->IsDead() && !pSeen->GetNiNode())
        {
            const TESObjectCELL* pCell = pSeen->GetParentCellEx();
            const PlayerCharacter* pPlayer = PlayerCharacter::Get();
            spdlog::info("CopyGone: dead copy {:X} lost its 3D {:.0f} units from the player; its cell {:X}, attached {}, disabled {}, form flags {:X}", aFormId,
                         pPlayer ? glm::distance(static_cast<glm::vec3>(pSeen->position), static_cast<glm::vec3>(pPlayer->position)) : -1.f, pCell ? pCell->formID : 0,
                         pCell && pCell->IsAttached(), pSeen->IsDisabled(), pSeen->flags);
        }
    }

    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        Actor* pActor = apActor;

        if (pActor)
        {
            if (pActor->IsTemporary() && !IsProcessExiting() && !CopyRegistry::IsOwnCopy(aFormId))
            {
                // The game's own temporary actor, remote only because the server gave it to somebody else.
                // The game is disposing of it and will finish; only the record of it is ours. See CopyRegistry.
                spdlog::info("Temporary remote {:X} was made by the game, not by this client; left to the game (form flags {:X})", aFormId, pActor->flags);
                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                RecentDeletes::Record(pActor, aFormId, refWord & 0x3FF, refWord, RecentDeletes::kLeftToGame);
                if (ActorExtension* pExtension = pActor->GetExtension())
                    pExtension->SetRemote(false);

                const uint32_t cServerId = m_world.get<RemoteComponent>(aEntity).Id;
                s_handedAway[cServerId] = HandedAway{aFormId, pActor->baseForm ? pActor->baseForm->formID : 0};
                spdlog::info("HandedAway: actor {:X} is server character {:X}, another player's now; remembered, so that it is the same actor when it comes back", aFormId,
                             cServerId);
            }
            else if (pActor->IsTemporary() && !IsProcessExiting())
            {
                // This runs while the game is disposing of the actor; freeing a copy it still holds is a use-after-free.
                // Who still has a claim on it is recorded first; the outstanding BSPointerHandle count is what matters.
                uint32_t claims = 0;
                if (const PlayerCharacter* pPlayer = PlayerCharacter::Get())
                {
                    if (pPlayer->GetCombatTarget() == pActor)
                        claims |= RecentDeletes::kPlayerCombatTarget;
                }
                if (pActor->currentProcess)
                    claims |= RecentDeletes::kHasProcess;
                if (pActor->GetNiNode())
                    claims |= RecentDeletes::kHas3D;
                if (pActor->actorState.IsDeadOrDying())
                    claims |= RecentDeletes::kDeadOrDying;

                // Anyone else fighting it, among the actors this mod tracks (a short loop).
                {
                    auto combatView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
                    for (auto other : combatView)
                    {
                        Actor* pOther = Cast<Actor>(TESForm::GetById(combatView.get<FormIdComponent>(other).Id));
                        if (pOther && pOther != pActor && pOther->GetCombatTarget() == pActor)
                        {
                            claims |= RecentDeletes::kOtherCombatTarget;
                            break;
                        }
                    }
                }

                const uint32_t refWord = static_cast<uint32_t>(pActor->handleRefObject.refCount);
                const uint32_t handles = refWord & 0x3FF;
                RecentDeletes::Record(pActor, aFormId, handles, refWord, claims);

                if (CopyRemovalPolicy::OnRemoved(handles, s_waitingCopies.size()) == CopyRemovalPolicy::Action::WaitDisabled)
                {
                    // Each outcome (held, freed, forced out) logs its own line; session-report.py counts them.
                    spdlog::info("Temporary Remote Held {:X}: {} handle(s) still on it, disabled and queued ({} of {} slots used)",
                                 aFormId, handles, s_waitingCopies.size() + 1, CopyRemovalPolicy::kMaxWaiting);

                    // Disabled, not freed: silent, no AI, not drawn, and nothing dangling.
                    LogCopyLife("held (the game let go of it, to be switched off)", aFormId, pActor);
                    FrameStartSwitchOff::Queue(pActor);
                    s_waitingCopies.push_back(WaitingCopy{aFormId, pActor->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
                    ReleaseGhostOf(aFormId, cDeadCopy);
                    DeleteRemoteEntityComponents(aEntity);

                    // The server still counts this character as spawned here; announcing the cell again makes it re-send it.
                    if (m_transport.IsConnected())
                        DiscoveryService::RequestCellReannounce();
                    return;
                }

                // Nothing holds it, but a load is up or just over: it waits, switched on, and its wait frees it once the load is over.
                if (CopyLifetime::WaitsOutLoad(CopyWorkSettled(s_removalLoadGate), s_waitingCopies.size(), CopyRemovalPolicy::kMaxWaiting))
                {
                    spdlog::info(
                        "Temporary Remote Held {:X}: {} handle(s) still on it, kept until the load is over ({} of {} slots used)", aFormId, handles, s_waitingCopies.size() + 1,
                        CopyRemovalPolicy::kMaxWaiting);
                    LogCopyLife("held (the game let go of it, until the load is over)", aFormId, pActor);
                    s_waitingCopies.push_back(WaitingCopy{aFormId, pActor->GetHandle().handle.iBits, std::chrono::steady_clock::now()});
                    ReleaseGhostOf(aFormId, cDeadCopy);
                    DeleteRemoteEntityComponents(aEntity);
                    if (m_transport.IsConnected())
                        DiscoveryService::RequestCellReannounce();
                    return;
                }

                // Nothing holds it, or the waiting list is full: free it here.
                // The bound keeps disabled copies from piling up while the game keeps its claims on them.
                if (handles == 0)
                {
                    spdlog::info("Temporary Remote Deleted {:X}: nothing held it", aFormId);
                }
                else
                {
                    // Freed while still held, because the list is full: if this is common, kMaxWaiting needs raising.
                    spdlog::warn("CopyFreedHeld: {:X} freed immediately with {} handle(s) still outstanding -- the waiting list was full ({} slots)",
                                 aFormId, handles, CopyRemovalPolicy::kMaxWaiting);
                }

                LogCopyLife("deleted (the game let go of it)", aFormId, pActor);
                pActor->Delete();
                CopyRegistry::ForgetOwnCopy(aFormId);
                ReleaseGhostOf(aFormId, cDeadCopy);

                // The cell unloaded under the copy, but the server still counts this character as spawned here.
                // Announcing the cell again makes the server re-send everything in it.
                if (m_transport.IsConnected())
                    DiscoveryService::RequestCellReannounce();
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
            }
        }
        // Gone from the game already: what it stood in for is not left hidden, and the game may give its id to an actor of its own next.
        else if (cTemporaryId)
        {
            CopyRegistry::ForgetOwnCopy(aFormId);
            ReleaseGhostOf(aFormId, cDeadCopy);
        }

        DeleteRemoteEntityComponents(aEntity);

        return;
    }

    // Not remote: a copy of ours that this client now runs is let go by the game like its own actors, not deleted here.
    // So its id leaves the copy registry.
    CopyRegistry::ForgetOwnCopy(aFormId);

    // Keep the cookie until the server response arrives so awarded ownership can be relinquished.
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);
        waitingComponent.Cancelled = true;
        return;
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        // A character run here on the copy standing in for a reference: the reference comes back with it.
        if (cTemporaryId)
            ReleaseGhostOf(aFormId, cDeadCopy);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;
        request.OwnershipEpoch = localComponent.OwnershipEpoch;
        request.Reason = OwnershipReleaseReason::Relinquish;

        // A temporary actor marked deleted while its cell is still loaded was deleted on purpose, as by a script.
        // Handed to another player it would live on there, so it is released as deleted instead.
        // One already freed is let go as before: the game also frees those of a cell left by a door or a teleport, for a player still there to take on.
        const TESObjectCELL* pCell = apActor ? apActor->GetParentCellEx() : nullptr;
        const bool cDeletedHere = cTemporaryId && apActor && apActor->IsDeleted() && pCell && pCell->IsAttached() && !IsProcessExiting();
        if (cDeletedHere)
            request.Reason = OwnershipReleaseReason::Deleted;

        if (Actor* pActor = apActor)
        {
            if (!pActor->IsTemporary())
            {
                auto& modSystem = m_world.GetModSystem();

                if (TESWorldSpace* pWorldSpace = pActor->GetWorldSpace())
                {
                    if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
                        spdlog::error("World space id not found, despite having a world space, {:X}", pWorldSpace->formID);
                }

                if (TESObjectCELL* pCell = pActor->GetParentCellEx())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }

        if (cDeletedHere)
            spdlog::info("Deleted: local actor {:X}, server id {:X}, was deleted here; released as deleted", aFormId, request.ServerId);
        else
            spdlog::info(
                "Transferring ownership of local actor, server id: {:X}, epoch: {}, worldspace: {:X}, cell: {:X}, position: "
                "({})",
                request.ServerId, request.OwnershipEpoch, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position);

        m_transport.Send(request);

        // One of this game's own temporary actors, given up because its cell unloaded (see s_handedAway).
        // If another player is there, the server gives it to them under this same server id.
        // Its base is read from the actor itself: an id lookup after the game freed it finds nothing, or the actor the game gave the id to.
        // One already freed is not recorded; it cannot come back as itself, and a record without a base would match any later actor on the id.
        if (cTemporaryId && cGameMade && !cDeletedHere && !IsProcessExiting())
        {
            if (apActor && apActor->baseForm)
            {
                s_handedAway[request.ServerId] = HandedAway{aFormId, apActor->baseForm->formID};
                spdlog::info(
                    "HandedAway: actor {:X} was server character {:X} and is given up; remembered, in case the server gives it to another player and sends it back", aFormId,
                    request.ServerId);
            }
            else
                spdlog::info("HandedAway: actor {:X} was server character {:X} and is given up; already freed by the game, so not remembered", aFormId, request.ServerId);
        }

        m_world.remove<LocalAnimationComponent, LocalComponent>(aEntity);
    }
}

/**
 * Make the copy for a character whose spawn waits for this game, from the spawn message it keeps.
 * A placed reference whose stand-in copy is gone gets a new stand-in.
 *
 * @param aEntity The character's entity, waiting for its 3D.
 * @return The copy, or null when it cannot be made yet; it is asked again every five seconds.
 */
Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    PerfCounterScope perfScope(PerfCounter::kActorSpawn);
    auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity);
    auto* pInterpolationComponent = m_world.try_get<InterpolationComponent>(aEntity);

    if (!pWaitingFor3D || !pInterpolationComponent)
    {
        spdlog::error(__FUNCTION__ ": could not find WaitingFor3D or InterpolationComponent");
        return nullptr;
    }

    auto& acMessage = pWaitingFor3D->SpawnRequest;

    Actor* pActor = nullptr;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            pNpc = BaseForCopy(m_world, acMessage);

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
            LogPlayerCopyAppearance(acMessage, pNpc);
        }

        pActor = RememberOwnCopy(Actor::Create(pNpc));
    }
    else
    {
        // A placed reference whose stand-in copy is gone: one made outside the loaded cells is deleted by the next grid shift.
        // This runs once the character is inside the loaded grid, so the new stand-in is made where it can stay.
        const uint32_t cRefId = m_world.GetModSystem().GetGameId(acMessage.FormId);
        TESNPC* pOwnerBase = nullptr;
        if (acMessage.LeveledNpcPickId != GameId{})
            pOwnerBase = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));
        if (!pOwnerBase && acMessage.BaseId != GameId{})
            pOwnerBase = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId)));

        const bool cFollowerCopy = acMessage.FollowerOfPlayerId && acMessage.FollowerOfPlayerId != m_transport.GetLocalPlayerId();
        // As the first copy of her was made: a pick with another name than her own base would rename her.
        if (cFollowerCopy)
            pOwnerBase = BaseForCopy(m_world, acMessage);

        if (pOwnerBase && cRefId != 0)
        {
            pActor = RememberOwnCopy(Actor::Create(pOwnerBase));
            if (pActor)
            {
                CopyRegistry::SetStandIn(cRefId, pActor->formID);
                if (cFollowerCopy)
                    MarkFollowerCopy(pActor);
                // Only a loaded one, as StandInForForeignCreature: disabling one the game is disposing of crashes.
                if (Actor* pLocal = Cast<Actor>(TESForm::GetById(cRefId)); pLocal && !pLocal->IsDisabled() && pLocal->GetNiNode() && !pLocal->IsDeleted() && !IsProcessExiting())
                    HideGhost(pLocal);
                spdlog::info("Stand-in: the copy for reference {:X} (server id {:X}) was gone; copy {:X} of the owner's {} ({:X}) stands in again, inside the loaded cells",
                             cRefId, m_world.get<RemoteComponent>(aEntity).Id, pActor->formID, pOwnerBase->fullName.value.AsAscii(), pOwnerBase->formID);
            }
        }
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        // Once per character: RunSpawnUpdates asks again every five seconds.
        static std::unordered_set<uint32_t> s_said;
        if (s_said.size() > 1024)
            s_said.clear();
        if (s_said.insert(remoteComponent.Id).second)
            spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X} ({}); asking again every 5 s", remoteComponent.Id,
                          acMessage.FormId != GameId{} ? "a placed reference whose copy is gone" : "no base to build it from");
        return nullptr;
    }

    SetUpCopyFromSpawn(m_world, aEntity, pActor, acMessage, pInterpolationComponent->Position);
    MatchOwnersLife(pActor, acMessage.IsDead);
    remoteComponent.DeadAtOwner = acMessage.IsDead;

    spdlog::info("Spawned character for entity, server id: {:X}", remoteComponent.Id);

    return pActor;
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();
    // A revive this game saw or was told of goes with the character, so a body of it in a game joining later does not kill it (IsDeadAgainstOwner).
    actorData.RevivedSinceDeath = !actorData.IsDead && apActor->GetExtension() && apActor->GetExtension()->RevivedSinceDeath;

    return actorData;
}

void CharacterService::ApplyLeveledNpcPick(Actor* apActor, const GameId& acPickId, const Inventory* apOwnerEquipment) const noexcept
{
    if (acPickId == GameId{})
        return;

    // On VR: SetLeveledCreature is AE 20231 -> VR 0x1402b8f10, CreateTemplateActorBase is AE 14375 -> VR 0x19c0c0.
    // cmpayc/TiltedEvolutionVR derived the same CreateTemplateActorBase address (thiscall and fastcall match on x64).
    // GarbageCollector::Add is not called (it crashed); see LeveledNpcSystem::ApplyPick.

    TESNPC* pBase = Cast<TESNPC>(apActor->baseForm);
    if (!pBase)
        return;

    if (!LeveledNpcSystem::GetOriginalBase(apActor))
        {
        spdlog::warn("Leveled pick {:x}:{:x} received for actor {:X} without an original leveled base, keeping local base", acPickId.ModId, acPickId.BaseId, apActor->formID);
            return;
        }

    const uint32_t cPickId = World::Get().GetModSystem().GetGameId(acPickId);
    if (cPickId == 0)
    {
        spdlog::warn("Leveled NPC pick {:X}:{:X} not resolvable, possibly missing mod, keeping local pick", acPickId.ModId, acPickId.BaseId);
        return;
    }

    TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickId));
    if (!pPick)
    {
        spdlog::warn("Leveled NPC pick {:X} is not an NPC, keeping local pick", cPickId);
        return;
    }

    const TESNPC* pLocalPick = apActor->GetLeveledPick();
    const uint32_t localPickId = pLocalPick ? pLocalPick->formID : 0;
    if (m_ownerLeveledPicks.size() > 4096)
        m_ownerLeveledPicks.clear();
    m_ownerLeveledPicks[apActor->formID] = cPickId;

    // A body keeps the look it has: changing its pick disables and rebuilds it, so it would vanish from under a hand that takes it, and can come back standing.
    // Only a body with no pick yet has no look to keep.
    if (apActor->IsDead() && (apActor->GetNiNode() || !LeveledNpcSystem::IsLeveledNpcBase(pBase)) && m_pendingLeveledConforms.find(apActor->formID) == m_pendingLeveledConforms.end())
        return;

    // Even a pick matching the current base must supersede pending work.
    if (pBase->IsTemporary() && localPickId == cPickId && m_pendingLeveledConforms.find(apActor->formID) == m_pendingLeveledConforms.end())
    {
        spdlog::info("Leveled actor {:X} already matches owner's pick {:X}", apActor->formID, cPickId);
        return;
    }

    spdlog::info("Queued leveled NPC reconciliation for actor {:X}, base: {:X}, local pick: {:X}, owner's pick: {:X}",
        apActor->formID, pBase->formID, localPickId, cPickId);

    // Defer reference changes to the service update: cell attach may still own the actor here.
    // Queueing to the runner from a drained task would re-lock its drain mutex.
    // Preserve the stage when a newer pick arrives during a disable or rebuild.
    m_pendingLeveledConforms[apActor->formID] = cPickId;
    // The rebuild re-equips the creature's outfit, which a copy refuses, so the owner's weapons are put back after it.
    if (apOwnerEquipment)
        m_conformEquipment[apActor->formID] = *apOwnerEquipment;
}

// A copy rebuilt for the owner's pick loses its weapons: the game re-equips its outfit, and a copy refuses the game's own equips.
// The owner's hand items, kept when the rebuild was queued, are put back the way a player copy's are.
// Only when the owner's list holds a weapon, so an NPC whose server copy is older than its drawn weapon keeps what it has.
void CharacterService::RestoreConformedWeapons(Actor* apActor) noexcept
{
    const auto found = m_conformEquipment.find(apActor->formID);
    if (found == m_conformEquipment.end())
        return;
    const Inventory cEquipment = std::move(found->second);
    m_conformEquipment.erase(found);

    const ActorExtension* pExtension = apActor->GetExtension();
    if (!pExtension || !pExtension->IsRemote())
        return;

    auto& modSystem = m_world.GetModSystem();
    const bool cHasWeapon = std::any_of(cEquipment.Entries.begin(), cEquipment.Entries.end(),
                                        [&modSystem](const auto& acEntry)
                                        {
                                            const TESForm* pForm = acEntry.IsWorn() ? TESForm::GetById(modSystem.GetGameId(acEntry.BaseId)) : nullptr;
                                            return pForm && pForm->formType == FormType::Weapon;
                                        });
    if (!cHasWeapon)
        return;

    InventoryService::ApplyHandEquipment(apActor, cEquipment, true);
    const TESForm* pLeft = apActor->GetEquippedWeapon(0);
    const TESForm* pRight = apActor->GetEquippedWeapon(1);
    spdlog::info("Leveled copy {:X} rebuilt: the owner's weapons put back, now holding left {:X}, right {:X}", apActor->formID, pLeft ? pLeft->formID : 0,
                 pRight ? pRight->formID : 0);
}

void CharacterService::ProcessLeveledConforms() noexcept
{
    using ReconciliationStage = ActorExtension::ReconciliationStage;

    // Never touch references while the loading screen is up: the cell attach owns them, and changing them crashes the loader.
    UI* pUI = UI::Get();
    if (pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")))
        return;

    // A pick that matched when it arrived can stop matching: the game re-rolls a leveled actor when it builds it.
    // So the owner's picks are checked again once a second.
    static std::chrono::steady_clock::time_point s_nextPickCheck;
    if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextPickCheck)
    {
        s_nextPickCheck = cNow + std::chrono::seconds(1);
        for (auto it = m_ownerLeveledPicks.begin(); it != m_ownerLeveledPicks.end();)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
            const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
            if (!pActor || pActor->IsDeleted() || !pExtension || !pExtension->IsRemote())
            {
                it = m_ownerLeveledPicks.erase(it);
                continue;
            }
            const TESNPC* pLocalPick = pActor->GetLeveledPick();
            // A body keeps the look it has (ApplyLeveledNpcPick).
            if (pLocalPick && pLocalPick->formID != it->second && !pActor->IsDead() && m_pendingLeveledConforms.find(it->first) == m_pendingLeveledConforms.end())
            {
                // Twice at most per actor, or a reconciliation that never matches would rebuild the creature every second.
                static std::unordered_map<uint32_t, int> s_rerolls;
                if (s_rerolls.size() > 4096)
                    s_rerolls.clear();
                if (int& tries = s_rerolls[it->first]; tries < 2)
                {
                    ++tries;
                    spdlog::info("Leveled actor {:X} was rolled again by the game as {:X}; the owner's pick is {:X}, reconciling again ({} of 2)", it->first,
                                 pLocalPick->formID, it->second, tries);
                    m_pendingLeveledConforms[it->first] = it->second;
                }
            }
            ++it;
        }
    }

    if (m_pendingLeveledConforms.empty())
        return;

    for (auto it = m_pendingLeveledConforms.begin(); it != m_pendingLeveledConforms.end();)
    {
        const uint32_t cPickFormId = it->second;

        Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
        TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickFormId));
        if (!pActor || pActor->IsDeleted() || !pPick)
        {
            if (pActor)
                pActor->GetExtension()->Reconciliation = ReconciliationStage::None;

            it = m_pendingLeveledConforms.erase(it);
            continue;
        }

        auto& stage = pActor->GetExtension()->Reconciliation;
        if (stage == ReconciliationStage::WaitingFor3D)
        {
            const auto* pCell = pActor->GetParentCellEx();
            if (!pCell || !pCell->IsAttached())
            {
                spdlog::info("Abandoning leveled NPC reconciliation for actor {:X} because its cell is not attached, pick: {:X}, cell state: {}, disabled: {}",
                    it->first, cPickFormId, pCell ? static_cast<int>(pCell->cellState) : -1, pActor->IsDisabled());
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            if (pActor->IsDisabled() || !pActor->GetNiNode())
            {
                ++it;
                continue;
            }

            if (pActor->baseForm && pActor->baseForm->IsTemporary() && pActor->GetLeveledPick() == pPick)
            {
                spdlog::info("Completed leveled NPC reconciliation for actor {:X}, base: {:X}, pick: {:X}", it->first, pActor->baseForm->formID, cPickFormId);
                RestoreConformedWeapons(pActor);
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // A newer pick arrived during the rebuild; start its disable now.
            stage = ReconciliationStage::None;
        }

        if (stage == ReconciliationStage::WaitingForDisable)
        {
            if (!pActor->IsDisabled() || pActor->GetNiNode())
            {
                spdlog::debug("Waiting for leveled actor {:X} to finish disabling before applying pick {:X}, disabled: {}, has 3D: {}",
                    it->first, cPickFormId, pActor->IsDisabled(), pActor->GetNiNode() != nullptr);
                ++it;
                continue;
            }

            if (!LeveledNpcSystem::ApplyPick(pActor, pPick))
            {
                spdlog::warn("Could not rebuild leveled actor {:X} from its original base and pick {:X}, keeping local base", it->first, cPickFormId);
            pActor->EnableImpl();
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // Recompute the graph descriptor after changing picks; stale variable indices can cause out-of-bounds writes.
            pActor->GetExtension()->GraphDescriptorHash = 0;

            // A copy of ours is known by its base too (see ClaimCopyId), and the rebuild gave it a new one.
            // Left on the old base, the copy counted as gone: a second copy was made and this one was announced as this game's own actor.
            auto remoteView = m_world.view<RemoteComponent>();
            for (const auto cEntity : remoteView)
            {
                auto& remoteComponent = remoteView.get<RemoteComponent>(cEntity);
                if (remoteComponent.CachedRefId == pActor->formID && remoteComponent.CachedBaseId != 0)
                    remoteComponent.CachedBaseId = pActor->baseForm->formID;
            }

            // Enable can return before the rebuilt 3D is available to discovery.
            stage = ReconciliationStage::WaitingFor3D;
            pActor->EnableImpl();
            spdlog::info("Re-enabled conformed leveled actor {:X}, base: {:X}, pick: {:X}, waiting for 3D",
                it->first, pActor->baseForm->formID, cPickFormId);
            ++it;
            continue;
        }

        if (!pActor->loadedState && !LeveledNpcSystem::IsLeveledNpcBase(Cast<TESNPC>(pActor->baseForm)))
        {
            // Wait for distant actors to load 3D; newer picks replace pending work and disconnects clear it.
            // Unresolved shells bypass this wait because they need a pick before they can load a model.
            ++it;
            continue;
        }

        // DisableImpl() is asynchronous: it only queues a request to disable this actor.
        // Wait for the disabled flag and old 3D removal before changing the base.
        RecentModelChanges::Note(pActor, RecentModelChanges::Kind::Disable);
        pActor->DisableImpl();
        stage = ReconciliationStage::WaitingForDisable;
        ++it;
    }
}

void CharacterService::RunLocalUpdates() const noexcept
{
    // The local player is sent at ~30 Hz for smooth VR head and hand movement, mounts and actors in combat too, other actors at 10 Hz.
    // Actions of every actor go at each send.
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 100ms;
    // On a grid with slack (SendGrid.h): with a plain 33 ms wait, the Quest's 33-37 ms frames sent only every other one.
    static SendGrid::Schedule s_playerSends{SendGrid::cMovementInterval, SendGrid::cMovementSlack};
    // The other actors' snapshot, on its own grid. It is asked only on a player send, so a send up to one player interval
    // before its slot takes it: a plain 100 ms wait often took a fourth send, and a smaller slack let the slot slip.
    static SendGrid::Schedule s_fullSends{cDelayBetweenSnapshots, SendGrid::cMovementInterval};

    const auto now = std::chrono::steady_clock::now();
    if (!s_playerSends.IsDue(now))
        return;

    // Logs how long this client was silent, with the open menus, when it starts sending again.
    // The server drops a silent client and hands its actors away, so the cause of a gap is worth knowing.
    if (lastSendTimePoint.time_since_epoch().count() != 0)
    {
        const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSendTimePoint).count();
        if (silence >= 250)
        {
            UI* pUI = UI::Get();
            std::string menus;
            if (pUI)
            {
                for (IMenu* pMenu : pUI->menuStack)
                {
                    if (!pMenu)
                        continue;
                    if (BSFixedString* pName = pUI->LookupMenuNameByInstance(pMenu))
                        menus += fmt::format("{}{}", menus.empty() ? "" : ", ", pName->AsAscii());
                }
            }
            spdlog::log(silence >= 1000 ? spdlog::level::warn : spdlog::level::info,
                        "Silence: sent nothing for {} ms; menus [{}]. The server drops a client that stops talking, and a drop hands its actors away.", silence,
                        menus.empty() ? "none" : menus);
        }
    }

    lastSendTimePoint = now;

#ifdef SKYRIMVR
    // The arrow on the string is an animation attachment the other side never gets, so it is sent from the attack state.
    // Each attack state change is logged (session-report.py reads it).
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        static uint32_t s_lastAttackState = 0xFFFFFFFFu;
        const uint32_t attackState = pPlayer->actorState.AttackState();
        if (attackState != s_lastAttackState)
        {
            s_lastAttackState = attackState;
            spdlog::info("VRArchery: local attack state {} (8 bow draw, 9 arrow on the string, 10 drawn, 11 releasing, 12 released), weapon drawn {}", attackState,
                         pPlayer->actorState.IsWeaponDrawn());
        }

        // Measurement: whether the heading sent for this player follows the headset or the body.
        VRBodySync::NoteHeading(pPlayer);

        // Measurement: where the game keeps this player's own arrow, in the hand and on the string, and whether it is drawn.
        if (pPlayer->actorState.IsWeaponDrawn())
            VRBodySync::LogLocalArrowNodes(pPlayer, attackState);

        // Sent to the others when the arrow goes on and comes off, so their copy of this player shows it.
        static bool s_nockedSent = false;
        // Nocked from state 9 (the arrow on the string, pulled or not) until the bow is shot or put away (0).
        // A VR draw goes 8, 9, 10 (pulled), and back to 9 and 8 while it still sits on the string, so 8 keeps it.
        const bool cGameNocked = attackState >= 9 || (s_nockedSent && attackState >= 8);
        // Before that, an arrow held in the hand: taken from the quiver, or ready after a shot.
        // Sent when it comes and goes, and again when the hand holds it differently (at most once a second).
        const VRBodySync::ArrowHold cGameHold = !cGameNocked && pPlayer->actorState.IsWeaponDrawn() ? VRBodySync::LocalArrowHold() : VRBodySync::ArrowHold{};
        static VRBodySync::ArrowHold s_holdSent{};
        static std::chrono::steady_clock::time_point s_holdSentAt{};
        // The game has no arrow in the hand from when it touches the bow until the draw, and for a moment now and then: it is still held there.
        // A PCVR draw never leaves attack state 0, so an arrow that left the hand at the bow is on the string until this player shoots it.
        static ArrowNocking::Filter s_arrowFilter;
        static uint32_t s_arrowsShotSeen = 0;
        const bool cShot = m_localArrowsShot != s_arrowsShotSeen;
        s_arrowsShotSeen = m_localArrowsShot;
        const ArrowNocking::Arrow cArrow = s_arrowFilter.Update(cGameNocked, cGameHold.InHand, pPlayer->actorState.IsWeaponDrawn(), now, cShot, VRBodySync::LocalArrowHandsApart());
        const bool cNocked = cArrow.Nocked;
        const VRBodySync::ArrowHold cHold = cArrow.InHand && !cGameHold.InHand ? s_holdSent : cGameHold;
        // A dot product of the two rotations under 0.996 is a turn of more than about 10 degrees.
        const bool cHoldMoved =
            cGameHold.InHand && s_holdSent.InHand && now - s_holdSentAt >= 1s &&
            (cHold.Side != s_holdSent.Side || glm::distance(cHold.Offset, s_holdSent.Offset) > 3.f || std::abs(glm::dot(cHold.Rotation, s_holdSent.Rotation)) < 0.996f);
        if ((cNocked != s_nockedSent || cHold.InHand != s_holdSent.InHand || cHoldMoved) && m_transport.IsConnected())
        {
            ArrowNockedRequest request{};
            request.Nocked = cNocked;
            request.InHand = cHold.InHand;
            request.HandSide = cHold.Side;
            request.HandOffset = cHold.Offset;
            request.HandRotation = cHold.Rotation;
            if (m_transport.Send(request))
            {
                s_nockedSent = cNocked;
                s_holdSent = cHold;
                s_holdSentAt = now;
                spdlog::info("ArrowNocked: sent this player's arrow {}{}", cNocked ? "on the string" : cHold.InHand ? "in the hand" : "gone",
                             cHold.InHand ? fmt::format(" ({} hand, its head at ({:.0f}) in the hand bone's space)", cHold.Side ? "right" : "left", cHold.Offset)
                                          : std::string());
            }
        }
    }
#endif

    const bool fullSnapshot = s_fullSends.IsDue(now);

    // Actions go reliably in their own message, since a lost one would leave the copy out of step until the next.
    // They go first: the movement after them can then come in any order.
    RequestActionEvents actionsMessage;

    // Who moves this time: the player, mounts and actors in combat at each send, the others ten times a second.
    struct Mover
    {
        uint32_t ServerId;
        Actor* pActor;
    };
    Vector<Mover> movers;

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        AnimationSystem::SerializeActions(actionsMessage, localComponent, animationComponent);

        if (!fullSnapshot && formIdComponent.Id != 0x14 && !pActor->IsMount() && !pActor->IsInCombat())
            continue;

        // The player first: the first message of a frame starts a packet, and each one after may fall across two.
        if (formIdComponent.Id == 0x14)
            movers.insert(movers.begin(), Mover{localComponent.Id, pActor});
        else
            movers.push_back(Mover{localComponent.Id, pActor});
    }

    if (!actionsMessage.Actions.empty())
        m_transport.Send(actionsMessage);

    // Movement goes unreliably: a lost message is not sent again, and the next one replaces it.
    // Each message is kept within one network packet's size, so a lost packet costs only the actors it carried.
    // A player's VR pose fills much of a packet, so messages are cut by size rather than by number of actors.
    TiltedPhoques::Buffer scratch(1 << 14);
    MovementPacker<ClientReferencesMoveRequest> packer(m_transport.GetClock().GetCurrentTick(), scratch);

    // Measurement: how big the movement messages are, once a minute.
    static uint32_t s_movementMessages = 0;
    static size_t s_largestBits = 0;
    static uint32_t s_oversized = 0;
    static std::chrono::steady_clock::time_point s_nextSizeSaid = now + 60s;
    const auto sendMovement = [&](const ClientReferencesMoveRequest& acMessage, const TiltedPhoques::EPacketFlags aFlags)
    {
        const size_t cBits = MovementPacker<ClientReferencesMoveRequest>::WrittenBits(scratch, acMessage);
        if (m_transport.Send(acMessage, aFlags))
        {
            ++s_movementMessages;
            s_largestBits = std::max(s_largestBits, cBits);
            if (cBits > MovementPacker<ClientReferencesMoveRequest>::kMaxBits)
                ++s_oversized;
        }
    };

    ClientReferencesMoveRequest full;
    for (const auto& [cServerId, pActor] : movers)
    {
        if (pActor->formID != 0x14)
            NoteDownedTravel(pActor->formID);

        ReferenceUpdate update;
        AnimationSystem::SerializeMovement(m_world, pActor, update);

        const size_t cEntryBits = packer.EntryBits(cServerId, update);
        if (packer.Add(cServerId, std::move(update), cEntryBits, full))
            sendMovement(full, TiltedPhoques::kUnreliable);
    }

    // The last movement message goes out at once, and everything queued before it this frame with it.
    // An empty one still goes ten times a second: the server hands the actors of a silent player away.
    if (fullSnapshot || !packer.Empty())
        sendMovement(packer.Current(), TiltedPhoques::kUnreliableNoNagle);

    if (now >= s_nextSizeSaid)
    {
        s_nextSizeSaid = now + 60s;
        if (s_movementMessages > 0)
            spdlog::info("MovementSend: {} movement messages in the last minute, the largest {} bytes, {} over {} bytes (one actor alone)", s_movementMessages,
                         (s_largestBits + 7) / 8, s_oversized, MovementPacker<ClientReferencesMoveRequest>::kMaxBits / 8);
        s_movementMessages = 0;
        s_largestBits = 0;
        s_oversized = 0;
    }
}

// Shows the other player's name and health on the game's enemy meter: above their head on VR (WSEnemyMeters), at the top
// of the screen on SE (the HUD's enemy health bar). There is only one meter, so the game's own fresh target wins it.
// Otherwise the other player's bar shows when you look at them, or whenever they are hurt (PlayerMeterRules).
void CharacterService::RunEnemyMeterUpdates() noexcept
{
    static std::chrono::steady_clock::time_point s_next;
    static uint32_t s_shownHandle = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 250ms; // the meter drops a target that is not hostile after 2 s, so this is a refresh as much as a decision

    const auto clear = [&]()
    {
        if (s_shownHandle)
        {
            UI::SetEnemyMeterTarget(0, 0);
            s_shownHandle = 0;
        }
    };

    if (!m_transport.IsConnected())
    {
        clear();
        return;
    }
    // The game pointed the meter at its own target (a hit on an enemy): leave it alone, but only briefly.
    // The game re-points it on every hit, so a long hold-off would hide the other player's bar for a whole fight.
    if (now - UI::LastGameEnemyMeterTargetAt() < 2s)
    {
        s_shownHandle = 0;
        return;
    }
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    Actor* pBest = nullptr;
    float bestScore = 0.f;

    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->GetNiNode() || pActor->actorState.IsDeadOrDying())
            continue;

        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > PlayerMeterRules::kRange)
            continue;

        NiPoint3 chest = pActor->position;
        chest.z += 100.f; // the head and chest, not the feet: that is what you look at
#ifdef SKYRIMVR
        const float angle = VRBodySync::HeadsetAngleTo(chest);
#else
        NiPoint3 screen{};
        const float angle = HUDMenuUtils::WorldPtToScreenPt3(chest, screen) ? PlayerMeterRules::ScreenAngle(screen.x, screen.y, screen.z) : 180.f;
#endif

        const uint32_t handle = pActor->GetHandle().handle.iBits;
        const float maxHealth = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        const float fraction = maxHealth > 0.f ? pActor->GetActorValue(ActorValueInfo::kHealth) / maxHealth : 1.f;
        const float score = PlayerMeterRules::Score(distance, angle, fraction, handle != 0 && handle == s_shownHandle);
        if (score > bestScore)
        {
            bestScore = score;
            pBest = pActor;
        }
    }

    if (!pBest)
    {
        clear();
        return;
    }
    const uint32_t handle = pBest->GetHandle().handle.iBits;
    if (!handle)
        return;
    UI::SetEnemyMeterTarget(handle, pBest->GetLevel());
    s_shownHandle = handle;
}

#ifdef SKYRIMVR
// Every 10 s, counts actors still flagged remote that have no RemoteComponent, and warns if there are any.
// HookActorProcess suppresses the AI of anything flagged remote, and nothing would clear the flag on such an actor.
void CharacterService::RunOrphanedRemoteDiag() noexcept
{
#ifdef SKYRIMVR
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 10s;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    uint32_t orphans = 0;
    uint32_t firstId = 0;
    const char* pFirstName = "?";

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;

        const ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension || !pExtension->IsRemote())
            continue;

        // Flagged remote with nothing to say it should be.
        if (m_world.all_of<RemoteComponent>(entity))
            continue;

        ++orphans;
        if (!firstId)
        {
            firstId = pActor->formID;
            if (const TESNPC* pBase = Cast<TESNPC>(pActor->baseForm))
                pFirstName = pBase->fullName.value.AsAscii();
        }
    }

    if (orphans)
        spdlog::warn("OrphanDiag: {} actors are still flagged remote with no remote component; first is {:X} ({}). Their AI is being suppressed and nothing is left to turn it back on.", orphans,
                     firstId, pFirstName);
#endif
}

// Runs every frame, because a haptic pulse that arrives late does not feel like contact.
// The cost is one form lookup and a distance test per tracked actor; anything beyond arm's reach is skipped early.
void CharacterService::RunWeaponTouch() noexcept
{
#ifdef SKYRIMVR
    VRBodySync::BeginWeaponTouch();

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (auto entity : view)
        VRBodySync::ConsiderForWeaponTouch(Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id)));

    VRBodySync::EndWeaponTouch();
#endif
}

// Clears invisibility on remote player copies.
void CharacterService::RunRemotePlayerDiag() noexcept
{
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + 2s;
    if (!m_transport.IsConnected())
        return;
    auto view = m_world.view<PlayerComponent, FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor)
            continue;
        // Whatever made the copy invisible (a synced value, a magic effect it ran), it is cleared here and logged.
        const float invisibility = pActor->GetActorValue(ActorValueInfo::kInvisibility);
        if (invisibility != 0.f)
        {
            spdlog::info("Remote player {:X} copy was invisible ({:.2f}); cleared", pActor->formID, invisibility);
            pActor->SetActorValue(ActorValueInfo::kInvisibility, 0.f);
        }

        // The node's fade (+0x158) is not forced: a copy beyond the fade distance is correctly faded out by the game.
    }
}
#else
void CharacterService::RunRemotePlayerDiag() noexcept {}
void CharacterService::RunOrphanedRemoteDiag() noexcept {}
void CharacterService::RunWeaponTouch() noexcept {}
#endif

#ifdef SKYRIMVR
/**
 * Take the health off the dead bodies near the player, and lay down one that has stood still over two looks.
 * A dead body with health gets back up, and stands with no ragdoll while the game still lets a player search it; a save keeps it so.
 * A body a spell is raising is left alone: it counts as dead for its first moments, with the health the spell gave it back.
 * From the start of a frame on the main thread: no job is rebuilding a body then.
 */
void CharacterService::LayDownStandingBodies() noexcept
{
    constexpr float cStandingHeight = 50.f; // a pelvis over the feet: about 60 standing, up to 43 lying on a slope
    constexpr float cStillDistance = 3.f;
    constexpr float cNearPlayer = 4096.f;
    constexpr uint32_t cBodiesAtATime = 24;
    constexpr uint32_t cTries = 3;

    // Not while a load is up or just over. This runs on the main thread, and the client's update may run beside it on another thread
    // during a load and change the entities walked here; and a body put into its ragdoll as its cell unloads may leave its physics
    // bodies to an actor the load then frees.
    static CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate))
        return;

    static std::chrono::steady_clock::time_point s_nextLook{};
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_nextLook)
        return;
    s_nextLook = cNow + std::chrono::seconds(2);

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    struct Standing
    {
        glm::vec3 Pelvis{};
        uint32_t Tries = 0;
    };
    static TiltedPhoques::Map<uint32_t, Standing> s_standing;
    if (s_standing.size() > 256)
        s_standing.clear();

    // Bodies past the limit are looked at on the next round.
    static uint32_t s_skip = 0;
    uint32_t seen = 0;
    uint32_t looked = 0;
    for (const auto cEntity : m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(cEntity).Id));
        if (!pActor || pActor == pPlayer || !pActor->GetNiNode())
            continue;
        if (glm::distance(glm::vec3(pActor->position), glm::vec3(pPlayer->position)) > cNearPlayer || !pActor->IsDead() || pActor->IsBeingRaised())
            continue;
        if (seen++ < s_skip)
            continue;
        if (++looked > cBodiesAtATime)
            break;

        if (const float cHealth = pActor->GetActorValue(ActorValueInfo::kHealth); cHealth > 0.f)
        {
            spdlog::info("BodyStand: dead {:X} had {:.0f} health; taken", pActor->formID, cHealth);
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, 0.f);
        }

        float height = 0.f;
        glm::vec3 pelvis{};
        if (!VRBodySync::BodyPelvisHeight(pActor, height, pelvis))
            continue;

        auto found = s_standing.find(pActor->formID);
        if (height < cStandingHeight)
        {
            if (found != s_standing.end())
                s_standing.erase(found);
            continue;
        }
        if (found == s_standing.end())
        {
            s_standing[pActor->formID].Pelvis = pelvis;
            continue;
        }
        Standing& standing = s_standing[pActor->formID];
        const bool cStill = glm::distance(pelvis, standing.Pelvis) < cStillDistance;
        standing.Pelvis = pelvis;
        if (!cStill || standing.Tries >= cTries)
            continue;

        ++standing.Tries;
        const auto* pNpc = Cast<TESNPC>(pActor->baseForm);
        LayDownBody(pActor);
        spdlog::info("BodyStand: dead {:X} ({}) stands, its pelvis {:.0f} over its feet, with no ragdoll; laid down (try {})", pActor->formID,
                     pNpc ? pNpc->fullName.value.AsAscii() : "?", height, standing.Tries);
    }
    s_skip = looked > cBodiesAtATime ? seen - 1 : 0;
}
#endif

/**
 * Run the copies of other games' characters: their places, actions and faces, and the 3D step of the ones whose 3D just came.
 * The 3D step gives a copy its inventory, factions and life as in its owner's game, and clears its wait for that step (ActorExtension::AwaitingFirst3D).
 */
void CharacterService::RunRemoteUpdates() noexcept
{
    InterpolationSystem::TickPresentation();
    // Each actor plays at its own delay, from how its updates arrive (InterpolationSystem, PlaybackClock).
    // The same tick drives the reference, its skeleton and its replayed actions, so a fast-moving body stays on its reference.
    const double cNow = m_transport.GetLastPollTime();
    // For an actor with no update yet: the fixed delay used before the playback clock.
    constexpr uint64_t cFallbackDelay = 225;
    const uint64_t nowTick = m_transport.GetClock().GetCurrentTick();
    const uint64_t fallbackTick = nowTick > cFallbackDelay ? nowTick - cFallbackDelay : 0;

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();

#ifndef SKYRIMVR
    // The game says what the player holds with the grab key, so a body grabbed here is known at once, with no motion to watch.
    static GrabbedBody s_grabbedBody;
    {
        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        const TESObjectREFR* pHeld = pPlayer && pPlayer->grabbedObjectHandle ? TESObjectREFR::GetByHandle(pPlayer->grabbedObjectHandle) : nullptr;
        const uint32_t cHeld = pHeld ? pHeld->formID : 0;
        s_grabbedBody.Update(cHeld, cHeld && Utils::FindByFormId(m_world.view<FormIdComponent, RemoteComponent>(), cHeld));
    }
#endif

    const auto updateInterpolation = [&](const entt::entity entity)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        InterpolationSystem::BindPoseIdentity(m_world, entity);
        const auto* pAnimation = m_world.try_get<RemoteAnimationComponent>(entity);
        InterpolationSystem::Update(
            pActor, interpolationComponent, cNow, interpolatedEntities.get<RemoteComponent>(entity).DeadAtOwner, pAnimation ? pAnimation->LastRanAction.Tick : 0,
            pAnimation ? pAnimation->LastRanAction.State1 : 0);

#ifdef SKYRIMVR
        // Cheap: it returns at once unless this is a dead body, or a living NPC in its ragdoll, within arm's reach.
        // A body another player owns that is being moved here is asked for, since only the owner sends a body's motion.
        if (VRBodySync::IsRemoteBodyGrabbedHere(pActor, interpolationComponent.Position) && pFormIdComponent)
        {
            const auto& remoteComponent = interpolatedEntities.get<RemoteComponent>(entity);
            spdlog::info("Body {:X} (server id {:X}) is being moved here; asking for it, so that this side sends it", pFormIdComponent->Id, remoteComponent.Id);
            if (!RequestOwnership(pFormIdComponent->Id, remoteComponent.Id, entity, false, !pActor->actorState.IsDead()))
                spdlog::info("Body {:X}: not asked after all (ownership epoch {} known here)", pFormIdComponent->Id, remoteComponent.OwnershipEpoch);
        }
#else
        // A body another player owns, picked up here with the grab key: asked for, so that this side sends how it is dragged.
        if (pActor && pFormIdComponent && s_grabbedBody.ShouldAsk(pActor->formID) && (pActor->actorState.IsDead() || pActor->actorState.IsInKnockRagdoll()) &&
            !pActor->GetExtension()->IsRemotePlayer() && !pActor->IsPlayerSummon())
        {
            const auto& remoteComponent = interpolatedEntities.get<RemoteComponent>(entity);
            if (RequestOwnership(pFormIdComponent->Id, remoteComponent.Id, entity, false, !pActor->actorState.IsDead()))
            {
                s_grabbedBody.Asked(pActor->formID);
                spdlog::debug("Body {:X} (server id {:X}) is grabbed here; asking for it, so that this side sends it", pFormIdComponent->Id, remoteComponent.Id);
            }
        }
#endif
    };

    // A rider seated on its horse (MountService) is placed after the horse, at the horse's playback tick, so that the two
    // are shown at the same instant of their owner's instead of on two clocks that drift apart with jitter.
    static Vector<entt::entity> s_riders;
    s_riders.clear();
    // Let off its horse: it glides from where it sat to where its owner has it, never below that place.
    const auto letOff = [](InterpolationComponent& aInterpolation)
    {
        if (!aInterpolation.Pinned)
            return;
        aInterpolation.Pinned = false;
        aInterpolation.SeatKnown = false;
        aInterpolation.SeatedSince = -1.0;
        aInterpolation.Placed = false;
        aInterpolation.GroundGlide = true;
    };
    for (auto entity : interpolatedEntities)
    {
        auto& interpolation = interpolatedEntities.get<InterpolationComponent>(entity);
        if (const auto* form = m_world.try_get<FormIdComponent>(entity); form && GameExtension::Reserved(form->Id))
        {
            interpolation.PlaceOnMount = false;
            interpolation.PinTo = nullptr;
            letOff(interpolation);
            continue;
        }
        interpolation.PlaceOnMount = false;
        interpolation.PinTo = nullptr;
        if (interpolation.SeatedOn != entt::null && interpolation.SeatedOn != entity && m_world.valid(interpolation.SeatedOn) &&
            m_world.all_of<InterpolationComponent>(interpolation.SeatedOn))
        {
            s_riders.push_back(entity);
            continue;
        }
        letOff(interpolation);
        updateInterpolation(entity);
    }
    // After every horse, so that each rider is held on its horse as the horse is drawn this frame.
    for (const auto entity : s_riders)
    {
        auto& interpolation = m_world.get<InterpolationComponent>(entity);
        const auto& mount = m_world.get<InterpolationComponent>(interpolation.SeatedOn);
        const auto* pRiderId = m_world.try_get<FormIdComponent>(entity);
        const auto* pHorseId = m_world.try_get<FormIdComponent>(interpolation.SeatedOn);
        Actor* pRider = pRiderId ? Cast<Actor>(TESForm::GetById(pRiderId->Id)) : nullptr;
        Actor* pHorse = pHorseId ? Cast<Actor>(TESForm::GetById(pHorseId->Id)) : nullptr;
        const bool cOnHorse = pRider && pRider->IsOnMount();
        const bool cHorseReady = cOnHorse && pHorse && !pHorse->IsDeleted() && pHorse->GetNiNode();
        if (cHorseReady && mount.Clock.IsRunning() && mount.Placed && mount.Clock.GetRenderTick() > 0.0)
        {
            interpolation.PlaceOnMount = true;
            interpolation.PlaceAtTick = mount.Clock.GetRenderTick();
            interpolation.PlaceCorrection = mount.Correction;
        }
        if (cHorseReady)
        {
            interpolation.PinTo = pHorse;
            interpolation.Pinned = true;
        }
        else
        {
            letOff(interpolation);
            // Off its horse: on its own clock again.
            if (pRider && !cOnHorse)
                interpolation.SeatedOn = entt::null;
        }
        updateInterpolation(entity);
        interpolation.PinTo = nullptr;
    }

    auto animatedView = m_world.view<RemoteComponent, RemoteAnimationComponent, FormIdComponent>();

    for (auto entity : animatedView)
    {
        auto& animationComponent = animatedView.get<RemoteAnimationComponent>(entity);
        auto& formIdComponent = animatedView.get<FormIdComponent>(entity);

        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        const auto* pInterpolation = m_world.try_get<InterpolationComponent>(entity);
        const uint64_t cActorTick = pInterpolation ? InterpolationSystem::GetPlaybackTick(*pInterpolation) : 0;
        const uint64_t cPlayTick = cActorTick ? cActorTick : fallbackTick;
        // A burst of the owner's actions in one of its frames (a draw, a swing and a block) plays as it came, in one frame,
        // instead of one a frame and frames late. Actions from later frames of the owner's, and a spawn's replayed history,
        // still play one a frame.
        for (uint32_t played = 0;;)
        {
            const size_t cQueued = animationComponent.TimePoints.size();
            const uint64_t cPlayedTick = cQueued ? animationComponent.TimePoints.front().Tick : 0;
            const bool cReplaying = animationComponent.ReplayCount > 0;
            AnimationSystem::Update(m_world, pActor, animationComponent, cPlayTick);
            ++played;
            const bool cNextDue = !animationComponent.TimePoints.empty() && animationComponent.TimePoints.front().Tick <= cPlayTick;
            const bool cSameOwnerFrame = cNextDue && animationComponent.TimePoints.front().Tick == cPlayedTick;
            if (!RemoteSmoothing::PlayAnotherAction(played, animationComponent.TimePoints.size() < cQueued, cNextDue, cSameOwnerFrame, cReplaying))
                break;
        }
    }

    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();

    for (auto entity : facegenView)
    {
        auto& formIdComponent = facegenView.get<FormIdComponent>(entity);
        auto& faceGenComponent = facegenView.get<FaceGenComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        FaceGenSystem::Update(m_world, pActor, faceGenComponent);
    }

    auto waitingView = m_world.view<FormIdComponent, WaitingFor3D>();

    Vector<entt::entity> readyEntities;
    for (auto entity : waitingView)
    {
        auto& formIdComponent = waitingView.get<FormIdComponent>(entity);
        auto& waitingFor3D = waitingView.get<WaitingFor3D>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || !pActor->GetNiNode())
            continue;

        // By now, the actor has materialized in the world and is ready for further setup

        pActor->SetActorInventory(waitingFor3D.SpawnRequest.InventoryContent);
        NpcEquipmentCheck::NoteOwnerEquipment(pActor, waitingFor3D.SpawnRequest.InventoryContent);
        // On VR the inventory apply above does not put weapons and torches in a remote player's hands, so they are applied here.
        // Players only: an NPC's server inventory can predate its AI drawing a weapon.
        if (pActor->GetExtension()->IsRemotePlayer())
            InventoryService::ApplyHandEquipment(pActor, waitingFor3D.SpawnRequest.InventoryContent, true);
        pActor->SetFactions(waitingFor3D.SpawnRequest.FactionsContent);

        if (!waitingFor3D.SpawnRequest.ActionsToReplay.Actions.empty())
        {
            pActor->LoadAnimationVariables(waitingFor3D.SpawnRequest.ActionsToReplay.Actions[0].Variables);
        }

        m_weaponDrawUpdates[pActor->formID] = {waitingFor3D.SpawnRequest.IsWeaponDrawn};
        if (waitingFor3D.SpawnRequest.IsPlayer)
            spdlog::info("Player copy {:X} came with its weapon {}", pActor->formID, waitingFor3D.SpawnRequest.IsWeaponDrawn ? "drawn" : "sheathed");

        // A body here that is alive at its owner stays a body (IsDeadAgainstOwner); its owner was sent the killing blow at the spawn.
        // One its owner revived since its last death, before or while it waited, is no such body: the owner knows of that death.
        const bool cStaysBody = IsDeadAgainstOwner(pActor, waitingFor3D.SpawnRequest.IsDead, waitingFor3D.SpawnRequest.RevivedSinceDeath);
        // The server's mark, kept for an announcement from here (BuildActorData); the 3D step is reached, so a death from here on is not an early one.
        if (ActorExtension* pExtension = pActor->GetExtension())
        {
            pExtension->RevivedSinceDeath = waitingFor3D.SpawnRequest.RevivedSinceDeath;
            pExtension->AwaitingFirst3D = false;
        }
        if (pActor->IsDead() != waitingFor3D.SpawnRequest.IsDead && !cStaysBody)
        {
            if (waitingFor3D.SpawnRequest.IsDead)
                spdlog::info("Character {:X} ({:X}) came dead; killed now that it has its 3D", waitingFor3D.SpawnRequest.ServerId, pActor->formID);
            waitingFor3D.SpawnRequest.IsDead ? pActor->Kill() : pActor->Respawn();
        }
        // Dead here already, so nothing above plays a death, and the 3D it just got stands.
        else if (pActor->IsDead())
        {
            spdlog::info("Character {:X} ({:X}) came back as a body; laid down now that it has its 3D", waitingFor3D.SpawnRequest.ServerId, pActor->formID);
            LayDownBody(pActor);
        }

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        readyEntities.push_back(entity);

        // A player's copy is hidden until the interpolation has put it where its owner is (SpawnReveal).
        // Not an NPC's: one standing still sends no movement, so it would appear late.
        if (waitingFor3D.SpawnRequest.IsPlayer)
            SpawnReveal::HideUntilPlaced(pActor);

        // An NPC copy shown here for the first time while its owner walks it gets no start of its own (Takeover).
        if (!waitingFor3D.SpawnRequest.IsPlayer)
            Takeover::WatchNewCopyLegs(entity, pActor);

        // Another player's summon cast just before comes out of its portal (SummonPortal); one only come near does not.
        if (waitingFor3D.SpawnRequest.IsPlayerSummon && !pActor->IsDead())
            MagicService::PlaySummonPortal(pActor);

        spdlog::info("Applied 3D for actor, form id: {:X}", pActor->formID);
    }

    for (auto entity : readyEntities)
    {
        m_world.remove<WaitingFor3D>(entity);

        // Reprocess the remote actor now that an ownership grant can be accepted without immediately declining it.
        ProcessNewEntity(entity);
    }
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static Throttle s_sendThrottle(2000ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        const auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        // Check if cached factions and current factions are identical
        auto factions = pActor->GetFactions();

        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;

        // If not send the current factions and replace the cached factions
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    // No copy is made or moved while a load is up or just over: the client's update can run beside the main thread then, which loads
    // and unloads the cells a copy is put in. The copies still to make wait for the next pass after it.
    static CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate))
        return;

    auto invisibleView = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(entt::exclude<FormIdComponent>);
    Vector<entt::entity> entities(invisibleView.begin(), invisibleView.end());

    for (const auto entity : entities)
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(entity);
        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);

        if (const auto pWorldSpace = PlayerCharacter::Get()->GetWorldSpace())
        {
            float characterX = interpolationComponent.Position.x;
            float characterY = interpolationComponent.Position.y;
            const auto characterCoords = GridCellCoords::CalculateGridCellCoords(characterX, characterY);
            const TES* pTES = TES::Get();
            const auto playerCoords = GridCellCoords(pTES->centerGridX, pTES->centerGridY);

            // A dragon can fight well outside the 5x5 grid, so it is created at the server's wide range too.
            // Not a stand-in (FormId set): outside the loaded cells the game deletes it at once, and it would be remade every frame.
            const auto& cSpawnRequest = m_world.get<WaitingFor3D>(entity).SpawnRequest;
            const bool isDragon = cSpawnRequest.IsDragon && cSpawnRequest.FormId == GameId{};
            if (GridCellCoords::IsCellInGridCell(characterCoords, playerCoords, isDragon))
            {
                // The id lookup can return a freed copy (EngineFixesVR's FormCaching), and casting it faults.
                // So the object is checked to be a live game object first; a dead one counts as no copy.
                TESForm* pCached = TESForm::GetById(remoteComponent.CachedRefId);
                if (pCached && !IsLiveGameObject(pCached))
                {
                    spdlog::warn("Cached copy {:X} of remote character {:X} no longer resolves to a live object; making a fresh one",
                                 remoteComponent.CachedRefId, remoteComponent.Id);
                    remoteComponent.CachedRefId = 0;
                    pCached = nullptr;
                }

                // A copy whose base changed is no longer ours: the game reused its id for one of its own actors (see ClaimCopyId).
                if (pCached && remoteComponent.CachedBaseId != 0)
                {
                    const auto* pCachedActor = Cast<Actor>(pCached);
                    if (!pCachedActor || !pCachedActor->baseForm || pCachedActor->baseForm->formID != remoteComponent.CachedBaseId)
                    {
                        spdlog::warn("Copy {:X} of server character {:X} is gone; its id now names another actor (base {:X}, the copy's was {:X}). A new copy is made",
                                     remoteComponent.CachedRefId, remoteComponent.Id, pCachedActor && pCachedActor->baseForm ? pCachedActor->baseForm->formID : 0,
                                     remoteComponent.CachedBaseId);
                        remoteComponent.CachedRefId = 0;
                        remoteComponent.CachedBaseId = 0;
                        pCached = nullptr;
                    }
                }

                auto* pActor = Cast<Actor>(pCached);
                if (!pActor)
                {
                    // A character that cannot be built here is retried every 5 s, not every frame.
                    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextAttempt;
                    const auto cNow = std::chrono::steady_clock::now();
                    auto& nextAttempt = s_nextAttempt[remoteComponent.Id];
                    if (cNow < nextAttempt)
                        continue;
                    if (s_nextAttempt.size() > 1024)
                        s_nextAttempt.clear();

                    // A copy gone within two seconds of being made would be removed again, so it waits like a failed one.
                    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_madeAt;
                    if (s_madeAt.size() > 1024)
                        s_madeAt.clear();
                    if (const auto cMade = s_madeAt.find(remoteComponent.Id); cMade != s_madeAt.end() && cNow - cMade->second < std::chrono::seconds(2))
                    {
                        spdlog::info("Copy of server character {:X} made {} ms ago is gone again; next try in 5 s", remoteComponent.Id,
                                     std::chrono::duration_cast<std::chrono::milliseconds>(cNow - cMade->second).count());
                        s_madeAt.erase(cMade);
                        s_nextAttempt[remoteComponent.Id] = cNow + std::chrono::seconds(5); // not nextAttempt: the map may have been cleared above
                        continue;
                    }

                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor)
                    {
                        s_nextAttempt[remoteComponent.Id] = cNow + std::chrono::seconds(5);
                        continue;
                    }
                    s_madeAt[remoteComponent.Id] = cNow;
                    LogCopyLife("made", pActor->formID, pActor);

                    remoteComponent.CachedRefId = pActor->formID;
                    remoteComponent.CachedBaseId = CopyRegistry::IsOwnCopy(pActor->formID) && pActor->baseForm ? pActor->baseForm->formID : 0;
                    ClaimCopyId(entity, pActor->formID);
                }

                // A copy waiting for its 3D is moved every frame, so its move is logged once, and again the first time after each load.
                static TiltedPhoques::Map<uint32_t, uint32_t> s_moveLoggedAt; // form id -> loading screens counted when its move was last logged
                if (s_moveLoggedAt.size() > 512)
                    s_moveLoggedAt.clear();
                const uint32_t cScreens = UI::LoadingScreensQueued();
                const auto cLogged = s_moveLoggedAt.find(pActor->formID);
                if (CopyLifetime::LogsWaitingMove(cLogged != s_moveLoggedAt.end(), cLogged != s_moveLoggedAt.end() ? cLogged->second : 0, cScreens))
                {
                    s_moveLoggedAt[pActor->formID] = cScreens;
                    LogCopyLife("moved (waiting for its 3D)", pActor->formID, pActor);
                }
                pActor->MoveTo(PlayerCharacter::Get()->parentCell, interpolationComponent.Position);
            }
        }
    }
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static Throttle s_sendThrottle(1000ms);
    if (!s_sendThrottle.TakeTurn())
        return;

    if (m_cachedExperience == 0.f)
        return;

    if (!World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;

    m_cachedExperience = 0.f;

    m_transport.Send(message);

    spdlog::debug("Sending over experience {}", message.Experience);
}

void CharacterService::ApplyPendingInventories() noexcept
{
    // Offline the server's inventories no longer apply: the actors are the game's own again.
    if (!m_transport.IsConnected())
    {
        if (!m_pendingInventories.empty())
        {
            spdlog::info("{} waiting inventories dropped: not connected", m_pendingInventories.size());
            m_pendingInventories.clear();
        }
        return;
    }

    for (auto it = m_pendingInventories.begin(); it != m_pendingInventories.end();)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
        if (!pActor || !IsSameCopy(it->first, it->second))
        {
            spdlog::info("CopyInventory: waiting inventory of {:X} dropped: the copy it came for is gone or changed hands", it->first);
            it = m_pendingInventories.erase(it);
            continue;
        }
        if (!pActor->HasLoadedProcess())
        {
            ++it;
            continue;
        }
        const Inventory& cContent = it->second.Content;
        const Inventory current = pActor->GetActorInventory();
        if (current.Entries != cContent.Entries || current.CurrentMagicEquipment != cContent.CurrentMagicEquipment)
            pActor->SetActorInventory(cContent);
        NpcEquipmentCheck::NoteOwnerEquipment(pActor, cContent);
        spdlog::info("Inventory of {:X} applied now that the actor is loaded here", it->first);
        it = m_pendingInventories.erase(it);
    }
}

bool CharacterService::IsSameCopy(const uint32_t aFormId, const PendingInventory& acPending) const noexcept
{
    if (TESForm::GetById(aFormId) != acPending.pActor)
        return false;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const auto cEntity : view)
    {
        if (view.get<FormIdComponent>(cEntity).Id != aFormId)
            continue;
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(cEntity))
            return pRemote->Id == acPending.ServerId && pRemote->OwnershipEpoch == acPending.OwnershipEpoch;
        if (const auto* pLocal = m_world.try_get<LocalComponent>(cEntity))
            return pLocal->Id == acPending.ServerId && pLocal->OwnershipEpoch == acPending.OwnershipEpoch;
        return false;
    }
    return false;
}

void CharacterService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch == 0)
        return;

    // A copy waiting for its 3D gets its spawn request's inventory once it has it, which would undo this change.
    auto waitingView = m_world.view<RemoteComponent, WaitingFor3D>();
    for (auto entity : waitingView)
    {
        const auto& cRemote = waitingView.get<RemoteComponent>(entity);
        if (cRemote.Id != acMessage.ServerId || cRemote.OwnershipEpoch != acMessage.OwnershipEpoch)
            continue;
        if (!acMessage.Drop || acMessage.Item.Count < 0)
        {
            waitingView.get<WaitingFor3D>(entity).SpawnRequest.InventoryContent.AddOrRemoveEntry(acMessage.Item);
            spdlog::info("CopyInventory: waiting copy {:X}, an inventory change from its owner applied to its spawn request too ({:+})", acMessage.ServerId, acMessage.Item.Count);
        }
        return;
    }

    for (auto it = m_pendingInventories.begin(); it != m_pendingInventories.end(); ++it)
    {
        if (it->second.ServerId != acMessage.ServerId || it->second.OwnershipEpoch != acMessage.OwnershipEpoch)
            continue;
        // As InventoryService applies it to the actor: a drop takes the items out, a pick-up does nothing yet.
        if (!acMessage.Drop || acMessage.Item.Count < 0)
        {
            it.value().Content.AddOrRemoveEntry(acMessage.Item);
            spdlog::info("CopyInventory: waiting inventory of {:X}, an inventory change from its owner applied to it too ({:+})", it->first, acMessage.Item.Count);
        }
        return;
    }
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    std::vector<uint32_t> toRemove{};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];

        data.m_timer += acUpdateEvent.Delta;

        // Remote actors get 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence.
        double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor || !pActor->GetExtension()->IsRemote())
        {
            toRemove.push_back(cId);
            continue;
        }

        // A draw that took shows "wantToDraw" or "drawing" here.
        const auto cStateBefore = pActor->actorState.GetWeaponState();
        // A copy already sheathed is left as it is.
        // A sheathe forced on it starts no animation, so the copy would wait for good for a sheathe that never plays.
        // A game that takes the character over would keep asking for that sheathe and be refused.
        const bool cAlreadySheathed = !data.m_drawWeapon && cStateBefore == ActorState::WeaponState::Sheathed;
        // A correction's draw that took is not played again: a copy in view would draw its sword twice.
        const bool cAlreadyDrawn = data.m_correction && data.m_drawWeapon && cStateBefore == ActorState::WeaponState::Drawn;
        if (!cAlreadySheathed && !cAlreadyDrawn)
            pActor->SetWeaponDrawnEx(data.m_drawWeapon);
        // A forced draw that started is not played again by the second pass on a copy in view.
        if (data.m_forced && data.m_isFirstPass && pActor->actorState.GetWeaponState() != ActorState::WeaponState::Sheathed)
            data.m_correction = true;
        if (pActor->GetExtension()->IsRemotePlayer())
            spdlog::info("Weapon of player copy {:X} {} ({} pass); the game went from {} to {}", cId,
                         cAlreadySheathed ? "already sheathed, left as it is" : cAlreadyDrawn ? "already drawn, left as it is" : data.m_drawWeapon ? "set drawn" : "set sheathed",
                         data.m_isFirstPass ? "first" : "second", ActorState::WeaponStateName(cStateBefore), ActorState::WeaponStateName(pActor->actorState.GetWeaponState()));

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}

// Sends each clash VRBodySync saw: the other player's weapon body met one of this player's HIGGS bodies.
// It is felt here, and the other player feels it on the hand holding that weapon.
void CharacterService::SendClashes() noexcept
{
#ifdef SKYRIMVR
    VRBodySync::Clash clash;
    while (VRBodySync::TakeClash(clash))
    {
        auto view = m_world.view<FormIdComponent, RemoteComponent>();
        const auto it = Utils::FindByFormId(view, clash.FormId);
        if (!it)
            continue;
        // A weapon on a hand holding no weapon or shield is a hit on the hand, not a parry: nothing is felt, heard or sent.
        // Such touches are logged at most every 5 s.
        if (!clash.Parries)
        {
            static uint32_t s_bare = 0;
            static auto s_nextBareSaid = std::chrono::steady_clock::now();
            ++s_bare;
            if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextBareSaid)
            {
                spdlog::info("Clash: {} touch(es) of the weapon of {:X} on our {} holding no weapon or shield since the last such line; no parry", s_bare, clash.FormId,
                             clash.OwnSide == 0 ? "left hand" : clash.OwnSide == 1 ? "right hand" : "hand");
                s_bare = 0;
                s_nextBareSaid = cNow + std::chrono::seconds(5);
            }
            continue;
        }
        // Every touch is weighed by the defender's rule (ActorValueService); only a meeting is felt, heard and sent.
        const uint64_t cTick = m_transport.GetClock().GetCurrentTick();
        m_dispatcher.trigger(ClashEvent{clash.FormId, cTick});
        if (!clash.Start)
        {
            // A resting touch is not a meeting; it is logged every 5 s while it lasts.
            static uint32_t s_touches = 0;
            static auto s_nextSaid = std::chrono::steady_clock::now();
            ++s_touches;
            if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextSaid)
            {
                spdlog::info("Clash: {} touch(es) of the weapon of {:X} since the last such line, still touching, not meetings", s_touches, clash.FormId);
                s_touches = 0;
                s_nextSaid = cNow + std::chrono::seconds(5);
            }
            continue;
        }
        ClashRequest request{};
        request.OtherId = view.get<RemoteComponent>(*it).Id;
        request.OtherSide = clash.Side;
        request.OwnSide = clash.OwnSide;
        request.Point = clash.Point;
        request.Speed = clash.Speed;
        request.Tick = cTick;
        if (m_transport.Send(request))
            GameExtension::Impact(clash.FormId, clash.Side, clash.Speed, cTick, &clash.Point.x);
        spdlog::debug(
            "Clash: our {} ({:.0f} units from its HIGGS body) met the {} weapon of {:X} (server {}) at ({:.0f}), {:.0f} units a second; felt{} here, sent",
            clash.OwnSide == 0   ? "left hand"
            : clash.OwnSide == 1 ? "right hand"
                                 : "hand or weapon",
            clash.HandDistance, clash.Side ? "right" : "left", clash.FormId, request.OtherId, clash.Point, clash.Speed, clash.Heard ? " and heard" : "");
    }
#endif
}

void CharacterService::OnNotifyClash(const NotifyClash& acMessage) const noexcept
{
    if (!ClientTrust::IsInsideWorld({acMessage.Point.x, acMessage.Point.y, acMessage.Point.z}))
        return;
    // Every player near it is told; the one whose weapon it was feels it.
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = Utils::FindByFormId(view, 0x14);
    const bool cOurs = it && view.get<LocalComponent>(*it).Id == acMessage.OtherId;
    const glm::vec3 point(acMessage.Point);
    const int64_t cAgo = static_cast<int64_t>(m_transport.GetClock().GetCurrentTick()) - static_cast<int64_t>(acMessage.Tick);
    spdlog::debug(
        "Clash from server {}: their {} met {} {} weapon at ({:.0f}), {:.0f} units a second, {} ms ago", acMessage.FromId,
        acMessage.OwnSide == 0   ? "left hand"
        : acMessage.OwnSide == 1 ? "right hand"
                                 : "hand or weapon",
        cOurs ? "our" : fmt::format("{}'s", acMessage.OtherId), acMessage.OtherSide ? "right" : "left", point, acMessage.Speed, cAgo);
#ifdef SKYRIMVR
    // Heard by everyone near it, felt by the one whose weapon it was.
    const bool cHeard = VRBodySync::PlayClashSound(point);
    VRBodySync::PlayClashSparks(point);
    if (cOurs)
        VRBodySync::PulseClashHaptics(acMessage.OtherSide);
    spdlog::info("Clash from server {}: {}{}", acMessage.FromId, cHeard ? "heard" : "not heard (no sound)", cOurs ? ", felt" : "");
#endif
}

void CharacterService::OnProjectileLaunched(const ProjectileLaunchedEvent& acEvent) noexcept
{
    // An arrow or bolt this player shot has left the string (RunLocalUpdates).
    if (const PlayerCharacter* pPlayer = PlayerCharacter::Get(); pPlayer && acEvent.ShooterID == pPlayer->formID && acEvent.AmmoID != 0)
        ++m_localArrowsShot;
}

void CharacterService::OnNotifyArrowNocked(const NotifyArrowNocked& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>();
    const auto it = Utils::FindById<RemoteComponent>(view, acMessage.ServerId);
    if (!it)
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor || !pActor->GetNiNode() || pActor->IsDead())
        return;

    // The arrow is the game's own: it takes the copy's equipped ammo, so none means nothing to put on the string.
    const bool cWanted = acMessage.Nocked || acMessage.InHand;
    if (cWanted && (!pActor->GetEquippedAmmo() || !pActor->actorState.IsWeaponDrawn()))
    {
        spdlog::info("ArrowNocked: {:X} (server {:X}) has an arrow {} over there, but this copy has {}; not attached", pActor->formID, acMessage.ServerId,
                     acMessage.Nocked ? "on the string" : "in the hand", pActor->GetEquippedAmmo() ? "its weapon sheathed" : "no arrows equipped");
        return;
    }

    // Actor::AttachArrow and DetachArrow take the actor's biped (TESObjectREFR::GetBiped2, slot 0x7F on every runtime).
    // They are called by slot because Actor.h declares the SE slot (0xCD) and VR has them two further on.
    // CommonLibVR: RelocateVirtual(0x0CD, 0x0CF) and (0x0CE, 0x0D0).
#ifdef SKYRIMVR
    constexpr size_t cAttachSlot = 0xCF;
    constexpr size_t cDetachSlot = 0xD0;
#else
    constexpr size_t cAttachSlot = 0xCD;
    constexpr size_t cDetachSlot = 0xCE;
#endif
    constexpr size_t cGetBiped2Slot = 0x7F;
    using TGetBiped = void* (*)(Actor*);
    using TArrow = void (*)(Actor*, void*);
    void** pVtable = *reinterpret_cast<void***>(pActor);
    void* pBipedHolder = reinterpret_cast<TGetBiped>(pVtable[cGetBiped2Slot])(pActor);
    if (!pBipedHolder || !*static_cast<void**>(pBipedHolder))
        return;
#ifdef SKYRIMVR
    size_t before = 0;
    VRBodySync::NodesNamedLike(pActor, "arrow", 0, before);
#endif
    // An arrow going from the hand to the string, or back, is the same arrow: only how it is held changes.
    const bool cHad = VRBodySync::HasCopyArrow(pActor->formID);
    VRBodySync::ArrowHold hold{};
    hold.InHand = acMessage.InHand && !acMessage.Nocked;
    hold.Side = acMessage.HandSide;
    hold.Offset = acMessage.HandOffset;
    hold.Rotation = acMessage.HandRotation;
    VRBodySync::SetArrowNocked(pActor->formID, acMessage.Nocked, hold);
    if (cWanted != cHad)
        reinterpret_cast<TArrow>(pVtable[cWanted ? cAttachSlot : cDetachSlot])(pActor, pBipedHolder);
    spdlog::info("ArrowNocked: {:X} (server {:X}): arrow {}", pActor->formID, acMessage.ServerId,
                 acMessage.Nocked ? "put on the string" : acMessage.InHand ? "held in the hand" : "taken off the string");
#ifdef SKYRIMVR
    // TEMPORARY: which arrow nodes the copy has, until the arrow is checked in a session.
    size_t after = 0;
    const std::string names = VRBodySync::NodesNamedLike(pActor, "arrow", 16, after);
    spdlog::info("ArrowNocked: {:X} has {} node(s) named like an arrow, {} before{}{}", pActor->formID, after, before, names.empty() ? "" : ": ", names);
#endif
}
