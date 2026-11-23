#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>
#include <Systems/InterpolationSystem.h>
#include <Systems/JumpSync.h>
#include <Systems/BodySolver.h>
#include <FlyState.h>
#include <SneakState.h>
#include <PlayerGraphEvents.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESNPC.h>
#include <Forms/ActorValueInfo.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Games/Misc/MenuTopicManager.h>

#include <Messages/RequestActionEvents.h>
#include <Structs/ReferenceUpdate.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_VampireLordBehavior.h>

#include <Components.h>
#include <World.h>
#include <Copies/Takeover.h>
#include <Services/MountService.h>
#include <Copies/CopyRegistry.h>
#include <NpcFurniture.h>
#include <DialogueFacing.h>
#include <ActionModIds.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <NetImmerse/NiTransform.h>

#include <Games/Skyrim/CopyBodyRules.h>
#include <Games/Skyrim/VR/VRBodySync.h>
#include <Services/CopyWorkGate.h>

#ifdef SKYRIMVR
#include <PlayerCharacter.h>
#include <Interface/UI.h>
#include <BSAnimationGraphManager.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/AnimationGraphDescriptor.h>
#endif

extern thread_local const char* g_animErrorCode;

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

    if (aAnimationComponent.ForeignGraph)
    {
        // The owner's actions belong to another creature's graph (see MarkForeignGraph); nothing here fits.
        actions.clear();
        return;
    }

    const auto it = std::begin(actions);
    if (it != std::end(actions) && it->Tick <= aTick)
    {
        // GetExtension() returns null for an actor this client did not allocate (Actor::ForcePosition); no owner's action plays on it.
        auto* pExtension = apActor->GetExtension();
        if (!pExtension)
        {
            actions.clear();
            return;
        }

        // Check if animation graph is ready before attempting to play animations
        if (!apActor->animationGraphHolder.IsReady())
        {
            // Animation graph not ready, keep the action in queue and try again later
            return;
        }
        // A dead copy plays nothing more, or it stands back up.
        // The owner's actions are replayed 300 ms late, so ones recorded while it was alive still arrive after it died here.
        if (apActor->actorState.IsDead())
        {
            aAnimationComponent.LastRanAction = *it;
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }

        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = *it;

        // Another player's own graph event, with no action behind it: sent to its copy's graph, if it is one sent at all.
        // One from the history a copy is spawned with is over; any other kind of copy, name or rate is ignored.
        if (PlayerGraphEvents::IsGraphEvent(first.Type, first.ActionId))
        {
            const auto cNow = std::chrono::steady_clock::now();
            if (aAnimationComponent.ReplayCount == 0 && pExtension->IsRemotePlayer() && aAnimationComponent.GraphEventGate.Allows(first.EventName, cNow) &&
                aAnimationComponent.GraphEvents.Take(cNow))
            {
                BSFixedString event(first.EventName.c_str());
                const bool cTaken = apActor->SendAnimationEvent(&event);
                spdlog::debug("Player graph events: '{}' on player copy {:X} {}", first.EventName.c_str(), apActor->formID, cTaken ? "taken" : "refused");
            }
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        const auto pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));

        // The owner's state word also carries the life state (bits 21-24).
        // Late actions recorded while the body was alive would stand a corpse back up, so once dying or dead that part is kept.
        // Death messages own it.
        constexpr uint32_t cLifeStateMask = 0x1E00000;
        // Every copy keeps its own knock state (bits 25-27): its fall and its get-up run on this game's physics.
        // With the owner's written over it, a copy thrown to the floor here would count as standing and never get up (live-copy-knockdown).
        constexpr uint32_t cKnockStateMask = 0xE000000;
        // Another player's copy never goes down (ActorValueService keeps it at 25 health).
        // It keeps its own life state too, and the owner's "Ragdoll" is not replayed on it.
        // A replayed fall would leave the copy on the floor, since the later "GetUpBegin" does not stand it up.
        const bool cPlayerCopy = pExtension->IsRemotePlayer();
        // Its owner's fall and get-up are read from them (InterpolationSystem, PlayerKnockMirror.h),
        // except from the history a copy is spawned with: that fall is over.
        if (cPlayerCopy && aAnimationComponent.ReplayCount == 0)
            InterpolationSystem::NotePlayerOwnerAction(apActor->formID, first.EventName.c_str(), first.State1);

        // Its owner's stop from a fall before the one its copy kneels in now: that fall's history came after the live actions.
        // Played, its state word and its stop would stand the copy up while its owner's NPC is down again.
        const bool cOwnersStop = !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStop") == 0;
        if (cPlayerCopy && cOwnersStop)
            InterpolationSystem::NotePlayerOwnerStop(apActor->formID, first.Tick);
        if (cOwnersStop && CopyBodyRules::StopBeforeFall(!cPlayerCopy, first.Tick, pExtension->OwnerFallTick.load(std::memory_order_relaxed)))
        {
            spdlog::debug(
                "KnockReplay: copy {:X} dropped its owner's 'bleedOutStop' of tick {}, from before its owner's fall at tick {}", apActor->formID, first.Tick,
                pExtension->OwnerFallTick.load(std::memory_order_relaxed));
            aAnimationComponent.LastRanAction = first;
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }
        if (!cPlayerCopy && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStart") == 0 &&
            first.Tick > pExtension->OwnerFallTick.load(std::memory_order_relaxed))
            pExtension->OwnerFallTick.store(first.Tick, std::memory_order_relaxed);

        const uint32_t cOwnStateBefore = apActor->actorState.flags1;
        uint32_t keptMask = cKnockStateMask;
        // FurnitureService and MountService set a player copy's native seat; actions still drive its graph.
        if (cPlayerCopy)
            keptMask |= NpcFurniture::kSitSleepMask;
        // A live owner's first action used to erase a copy's own essential-down/bleedout state while its graph still knelt.
        // Interpolation then saw an alive copy with no knock state and carried it after the owner, never reaching its recovery.
        // Keep the local down state until OnBleedingOut has seen the owner's separate up word for long enough.
        if (CopyBodyRules::KeepLifeStateHere(cPlayerCopy, cOwnStateBefore))
            keptMask |= cLifeStateMask;

        // A copy left away from where its owner's NPC is (beyond the loaded area here) does not sit down mid-road as its owner
        // does in its seat over there: that way in is left out, and the copy keeps its own seat state.
        if (!cPlayerCopy)
        {
            if (const auto cOwnerPlace = CopyRegistry::AwayFromOwner(apActor->formID))
            {
                const TESWorldSpace* pWorldSpace = apActor->GetWorldSpace();
                const TESObjectCELL* pCell = pWorldSpace ? nullptr : apActor->GetParentCellEx();
                const bool cSameSpace = NpcFurniture::SameSpace(cOwnerPlace->WorldSpaceId, cOwnerPlace->CellId, pWorldSpace ? pWorldSpace->formID : 0, pCell ? pCell->formID : 0);
                const auto& cOwnerPosition = cOwnerPlace->Position;
                const float cDistance =
                    glm::distance(glm::vec3(apActor->position.x, apActor->position.y, apActor->position.z), glm::vec3(cOwnerPosition[0], cOwnerPosition[1], cOwnerPosition[2]));
                if (NpcFurniture::SkipSeatReplay(NpcFurniture::IsSeatReplay(first.EventName.c_str(), first.State1), true, cSameSpace, cDistance))
                {
                    keptMask |= NpcFurniture::kSitSleepMask;
                    if (NpcFurniture::IsSeatReplay(first.EventName.c_str(), 0))
                    {
                        if (cSameSpace)
                            spdlog::debug(
                                "ReplayDiag: '{}' not replayed on copy {:X}; its owner's NPC sits {:.0f} units from it, beyond the loaded area here", first.EventName.c_str(),
                                apActor->formID, cDistance);
                        else
                            spdlog::debug(
                                "ReplayDiag: '{}' not replayed on copy {:X}; its owner's NPC sits in another cell or world space", first.EventName.c_str(), apActor->formID);
                        aAnimationComponent.LastRanAction = first;
                        if (aAnimationComponent.ReplayCount > 0)
                            aAnimationComponent.ReplayCount--;
                        actions.pop_front();
                        return;
                    }
                }
            }
        }
        // Its flight state is its own too, set through the game (FlyState.h): written over raw, the copy kept the ground's
        // character controller, and fell the moment its game took it over in the air.
        apActor->actorState.flags1 = FlyState::CopyFlags1(first.State1, apActor->actorState.flags1, keptMask);
        // A vampire lord's copy too: its owner's hover, if the game gives it a flight state, is set the same way.
        const bool cVampireLord = pExtension->GraphDescriptorHash == AnimationGraphDescriptor_VampireLordBehavior::m_key;
        if (FlyState::SetThroughGame(cVampireLord || apActor->IsDragon(), apActor->movementDrivenControl != nullptr, first.State1, apActor->actorState.flags1))
        {
            if (cVampireLord)
                spdlog::debug("FlyState: vampire lord copy {:X} set to its owner's flight state {}", apActor->formID, FlyState::Of(first.State1));
            apActor->SetFlyState(FlyState::Of(first.State1));
        }
        apActor->actorState.flags2 = first.State2;

        // On VR the owner's own "Ragdoll" knocks the copy down when it mirrors its owner's falls, as an NPC copy's does here.
        // It plays only when the mirror takes it as the knock a flat frame would make: an alive owner's live word, this Ragdoll's
        // own included, a copy on its feet, not pinned, not dying and not knocked already (InterpolationSystem::AdmitPlayerRagdoll).
        // On a flat screen the game's own knock does it instead (InterpolationSystem), as the action left the copy without a knock state.
        const bool cOwnersRagdoll = cPlayerCopy && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "Ragdoll") == 0;
#ifdef SKYRIMVR
        const bool cMirrorsKnocks = cOwnersRagdoll;
        const bool cMirroredRagdoll = cMirrorsKnocks && InterpolationSystem::AdmitPlayerRagdoll(aWorld, apActor, aAnimationComponent.ReplayCount > 0, first.State1);
#else
        constexpr bool cMirrorsKnocks = false;
        constexpr bool cMirroredRagdoll = false;
#endif
        if (cMirrorsKnocks && !cMirroredRagdoll)
        {
            spdlog::debug("KnockDown: owner's 'Ragdoll' not played on player copy {:X}; its mirror did not take it as a knock", apActor->formID);
            aAnimationComponent.LastRanAction = first;
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }
        if (cOwnersRagdoll && !cMirroredRagdoll)
        {
            static std::chrono::steady_clock::time_point s_nextSaid{};
            if (std::chrono::steady_clock::now() >= s_nextSaid)
            {
                s_nextSaid = std::chrono::steady_clock::now() + 10s;
                spdlog::info("ReplayDiag: 'Ragdoll' not replayed on player copy {:X}; a copy never goes down", apActor->formID);
            }
            aAnimationComponent.LastRanAction = first;
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }

        // The owner's climb on or off its horse names the horse by its own game's id, and is played on the copy by MountService, which
        // seats the copy on its horse here: the climb is only noted, so that the copy climbs on with its owner (and never twice).
        if (!first.EventName.empty() && MountService::TakesClimbs() &&
            (_stricmp(first.EventName.c_str(), "HorseEnter") == 0 || _stricmp(first.EventName.c_str(), "HorseExit") == 0))
        {
            // Not one from the history a copy is spawned with: that rider has been riding for a while.
            if (_stricmp(first.EventName.c_str(), "HorseEnter") == 0 && aAnimationComponent.ReplayCount == 0)
                MountService::NoteOwnerClimb(apActor->formID);
            aAnimationComponent.LastRanAction = first;
            if (aAnimationComponent.ReplayCount > 0)
                aAnimationComponent.ReplayCount--;
            actions.pop_front();
            return;
        }

        // Its legs keep the speed it is shown moving at, which its interpolation set this frame.
        AnimationVariables variables = first.Variables;
        InterpolationSystem::HoldStride(apActor, variables);
        apActor->LoadAnimationVariables(variables);

        aAnimationComponent.LastRanAction = first;

#ifndef SKYRIMVR
        // An essential NPC brought to 0 by its owner's game falls into its bleedout there and kneels; its copy here, kept above 0 health,
        // took that fall as a full ragdoll and lay through the bleedout while its owner's knelt (live-copy-knockdown-place).
        // So the copy kneels too, and lies only when its owner's is thrown over from there.
        if (!cPlayerCopy && !first.EventName.empty() && (_stricmp(first.EventName.c_str(), "Ragdoll") == 0 || _stricmp(first.EventName.c_str(), "DeathAnim") == 0))
        {
            bool deadAtOwner = true;
            float ownerHealth = std::numeric_limits<float>::quiet_NaN();
            const auto remotes = aWorld.view<RemoteComponent>();
            for (const auto entity : remotes)
                if (remotes.get<RemoteComponent>(entity).CachedRefId == apActor->formID)
                {
                    deadAtOwner = remotes.get<RemoteComponent>(entity).DeadAtOwner;
                    ownerHealth = remotes.get<RemoteComponent>(entity).OwnerHealth;
                    break;
                }
            // Kept at 1 health here (ActorValueService), the copy has healed a little by the time its owner's fall is replayed.
            constexpr float cCopyHealthFloor = 1.f;
            const float cHealth = apActor->GetActorValue(ActorValueInfo::kHealth);
            if (CopyBodyRules::KneelsInsteadOfFalling(
                    VRBodySync::MovesFlatRagdolls(), true, pExtension && pExtension->IsRemote(), apActor->IsEssential(),
                    CopyBodyRules::AtOwnersHealthFloor(cHealth, ownerHealth, cCopyHealthFloor), apActor->actorState.IsDeadOrDying(), deadAtOwner, first.State1))
            {
                BSFixedString event("bleedOutStart");
                const bool cTaken = apActor->SendAnimationEvent(&event);
                spdlog::debug(
                    "KnockReplay: essential copy {:X} kneels for its owner's '{}' ({}; health {:.0f} here, {:.0f} at its owner)", apActor->formID, first.EventName.c_str(),
                    cTaken ? "taken" : "not taken", cHealth, ownerHealth);
                if (cTaken)
                {
                    InterpolationSystem::NoteReplayedFall(aWorld, apActor, true, pExtension->NativeLifeGeneration.load(std::memory_order_relaxed), first.Tick);
                    if (first.Tick > pExtension->OwnerFallTick.load(std::memory_order_relaxed))
                        pExtension->OwnerFallTick.store(first.Tick, std::memory_order_relaxed);
                }
                if (aAnimationComponent.ReplayCount > 0)
                    aAnimationComponent.ReplayCount--;
                actions.pop_front();
                return;
            }
        }
#endif

        // Its owner's NPC got up from its bleedout, while the copy's own is kept here: the game ends it its own way, writing it alive
        // and then performing the stop (Actor::SetLifeState). Forced as an action while its life still reads down, the stop was refused,
        // and the copy knelt on while its owner's NPC fought (Lydia, 2026-11-06).
        if (!cPlayerCopy && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStop") == 0)
        {
            bool deadAtOwner = true;
            const auto remotes = aWorld.view<RemoteComponent>();
            for (const auto entity : remotes)
                if (remotes.get<RemoteComponent>(entity).CachedRefId == apActor->formID)
                {
                    deadAtOwner = remotes.get<RemoteComponent>(entity).DeadAtOwner;
                    break;
                }
            if (CopyBodyRules::RecoversOnOwnersStop(true, apActor->actorState.flags1, first.State1, deadAtOwner))
            {
                const uint32_t cLifeBefore = apActor->actorState.LifeState();
                // The game's stop plays on a copy only while its owner's word has its NPC up, and this word does.
                const uint8_t cWordBefore = pExtension->OwnerWordOfCopy.exchange(static_cast<uint8_t>(CopyBodyRules::OwnerWord::Up), std::memory_order_relaxed);
                pExtension->EngineStopPlayed.store(0, std::memory_order_relaxed);
                apActor->SetLifeState(LifeStates::kAlive);
                pExtension->OwnerWordOfCopy.store(cWordBefore, std::memory_order_relaxed);
                const bool cPlayed = pExtension->EngineStopPlayed.load(std::memory_order_relaxed) == 1;
                spdlog::debug(
                    "KnockReplay: copy {:X} got its owner's 'bleedOutStop' while in its own bleedout (life state {}); ended it the game's way, "
                    "its stop {}, life state now {}",
                    apActor->formID, cLifeBefore, cPlayed ? "played" : "not played", apActor->actorState.LifeState());
                if (cPlayed)
                {
                    pExtension->KneelsInGraph.store(false, std::memory_order_relaxed);
                    if (aAnimationComponent.ReplayCount > 0)
                        aAnimationComponent.ReplayCount--;
                    actions.pop_front();
                    return;
                }
            }
        }

        // Play the animation
        TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
        actionData.eventName = BSFixedString(first.EventName.c_str());
        actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
        actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

        const uint64_t cNativeLifeAtAction = pExtension->NativeLifeGeneration.load(std::memory_order_relaxed);
        const auto result = ActorMediator::Get()->ForceAction(&actionData);
        // A jump, fall or landing its graph refused as an action: the graph is sent the event itself, as its owner's took it.
        bool jumpTaken = result != 0;
        if (JumpSync::SendsRefusedEvent(cPlayerCopy, result != 0, first.EventName.c_str(), apActor->actorState.IsDeadOrDying() || apActor->actorState.IsKnockedDown()))
        {
            BSFixedString event(first.EventName.c_str());
            const bool cTaken = apActor->SendAnimationEvent(&event);
            jumpTaken = cTaken;
            spdlog::debug("Jump: '{}' refused as an action on player copy {:X}; its graph {} the event", first.EventName.c_str(), apActor->formID, cTaken ? "took" : "refused");
        }
        // Where its jump stands, so that it lands once its owner is on the ground (InterpolationSystem, JumpSync::Landing).
        if (cPlayerCopy)
            InterpolationSystem::NoteCopyJumpEvent(apActor->formID, first.EventName.c_str(), jumpTaken);
        bool cGraphRagdoll = false;
        bool cDeadAtOwner = true; // unknown canonical authority is not permission to replay a fall
        const auto remotes = aWorld.view<RemoteComponent>();
        for (const auto entity : remotes)
            if (remotes.get<RemoteComponent>(entity).CachedRefId == apActor->formID)
            {
                cDeadAtOwner = remotes.get<RemoteComponent>(entity).DeadAtOwner;
                break;
            }
#ifndef SKYRIMVR
        const bool cRagdoll = !first.EventName.empty() && _stricmp(first.EventName.c_str(), "Ragdoll") == 0;
        const auto* extension = apActor->GetExtension();
        if (CopyBodyRules::MayRetryRagdoll(
                result != 0, cRagdoll, extension && extension->IsRemote() && !cPlayerCopy, apActor->actorState.IsDeadOrDying(), cDeadAtOwner,
                extension && extension->NativeLifeGeneration.load(std::memory_order_relaxed) == cNativeLifeAtAction))
        {
            // The owner's real push action can be refused by PerformComplexAction on a locally kneeling copy.
            // Ask its graph for the SAME event, as the existing refused bleedOutStop path does below.
            // Actual consecutive flat bones, not this acknowledgement, must still establish its ragdoll.
            BSFixedString event("Ragdoll");
            cGraphRagdoll = apActor->SendAnimationEvent(&event);
        }
        // Refused both ways, as by a copy kneeling in its bleedout when its owner's is thrown over: the game knocks it down itself,
        // with no push, so that it lies as its owner's does and its own knock word gets it up (live-copy-knockdown-place).
        if (CopyBodyRules::KnockAfterRefusedRagdoll(
                VRBodySync::MovesFlatRagdolls(), result != 0 || cGraphRagdoll, cRagdoll, extension && extension->IsRemote() && !cPlayerCopy, apActor->actorState.IsDeadOrDying(),
                cDeadAtOwner, extension && extension->NativeLifeGeneration.load(std::memory_order_relaxed) == cNativeLifeAtAction, apActor->actorState.flags1) &&
            apActor->currentProcess)
        {
            const NiPoint3 cAt = apActor->position;
            apActor->currentProcess->KnockExplosion(apActor, &cAt, 0.f);
            spdlog::debug(
                "KnockReplay: copy {:X} refused its owner's 'Ragdoll'; knocked down here instead (knock state {})", apActor->formID,
                CopyBodyRules::KnockState(apActor->actorState.flags1));
        }
        // Taken by the graph of a copy kneeling in its essential fall, it lies flat but still in its bleedout, with no knock word of its own.
        // The game's own knock gives it one, as when it refused the fall above.
        else if (
            CopyBodyRules::KnockAfterKneelingRagdoll(
                VRBodySync::MovesFlatRagdolls(), result != 0 || cGraphRagdoll, cRagdoll, extension && extension->IsRemote() && !cPlayerCopy, apActor->actorState.IsDeadOrDying(),
                cDeadAtOwner, extension && extension->NativeLifeGeneration.load(std::memory_order_relaxed) == cNativeLifeAtAction, apActor->actorState.flags1) &&
            apActor->currentProcess)
        {
            const NiPoint3 cAt = apActor->position;
            apActor->currentProcess->KnockExplosion(apActor, &cAt, 0.f);
            spdlog::debug(
                "KnockReplay: copy {:X} took its owner's 'Ragdoll' while kneeling; knocked down here too (knock state {})", apActor->formID,
                CopyBodyRules::KnockState(apActor->actorState.flags1));
        }
#endif
        // The game refuses "bleedOutStart" as an action on a copy above 0 health, and a player's copy always is; its graph takes the
        // event, as its owner's did, so it kneels where its owner kneels instead of lying in the fall that came before or standing.
        if (!result && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStart") == 0 &&
            CopyBodyRules::SendRefusedBleedOutStart(VRBodySync::MovesFlatRagdolls(), cPlayerCopy, apActor->actorState.IsDeadOrDying(), cDeadAtOwner))
        {
            BSFixedString event("bleedOutStart");
            const bool cTaken = apActor->SendAnimationEvent(&event);
            spdlog::debug("ReplayDiag: 'bleedOutStart' refused as an action on {:X}; sent to its graph instead ({})", apActor->formID, cTaken ? "taken" : "not taken");
            // Only its graph kneels, its life still alive: nothing else would take it for down, and with its owner's "bleedOutStop" lost
            // it knelt for good while its owner's NPC stood (live-copy-essential-up). Remembered, it gets up once its owner's does.
            if (CopyBodyRules::KneelsInGraphOnly(true, cTaken, apActor->actorState.flags1))
            {
                pExtension->KneelsInGraph.store(true, std::memory_order_relaxed);
                // A player's copy gets up on a word of its owner's from after this kneel (CopyBodyRules::PlayerKneelEnds).
                if (cPlayerCopy && first.Tick > pExtension->OwnerFallTick.load(std::memory_order_relaxed))
                    pExtension->OwnerFallTick.store(first.Tick, std::memory_order_relaxed);
            }
        }
        if (!first.EventName.empty() && (_stricmp(first.EventName.c_str(), "Ragdoll") == 0 || _stricmp(first.EventName.c_str(), "DeathAnim") == 0 ||
                                         _stricmp(first.EventName.c_str(), "GetUpBegin") == 0 || _stricmp(first.EventName.c_str(), "bleedOutStop") == 0))
            spdlog::debug(
                "KnockReplay: copy {:X} event {} tick {}, owner flags {:#x}, local before {:#x} after {:#x}, native action {}, same-event graph retry {}, native life generation "
                "{}",
                apActor->formID, first.EventName.c_str(), first.Tick, first.State1, cOwnStateBefore, apActor->actorState.flags1, result != 0, cGraphRagdoll, cNativeLifeAtAction);
        if ((result || cGraphRagdoll) && !cPlayerCopy && !first.EventName.empty())
        {
            if (_stricmp(first.EventName.c_str(), "Ragdoll") == 0)
                InterpolationSystem::NoteReplayedFall(aWorld, apActor, false, cNativeLifeAtAction, first.Tick);
            else if (_stricmp(first.EventName.c_str(), "DeathAnim") == 0)
                InterpolationSystem::NoteReplayedFall(aWorld, apActor, true, cNativeLifeAtAction, first.Tick);
        }
        if (!cPlayerCopy && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "GetUpBegin") == 0)
            InterpolationSystem::NoteOwnerGetUp(aWorld, apActor, first.Tick);
        if (result && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStop") == 0)
            pExtension->KneelsInGraph.store(false, std::memory_order_relaxed);

        // The owner's flags1 came from just before its sneak toggle; the owner now sneaks, or no longer does. Detection reads this bit,
        // in this game and in the NPCs' owner's game, which judges this copy. Whether or not this copy's graph took the action.
        apActor->actorState.flags1 = SneakState::AfterAction(actionId, first.State1, apActor->actorState.flags1);

        // The game can refuse "bleedOutStop" as an action, which leaves the copy down while its owner is up again.
        // Sending the graph event itself takes the copy out of the pose, as on the owner's side.
        // A player's copy can refuse the graph event too and lie on after its owner is revived, and so can an NPC copy whose owner is up.
        // "IdleForceDefaultState" puts its graph back to standing from whatever state it is in.
        if (!result && !first.EventName.empty() && _stricmp(first.EventName.c_str(), "bleedOutStop") == 0)
        {
            BSFixedString event("bleedOutStop");
            const bool cSent = apActor->SendAnimationEvent(&event);
            const char* cpOutcome = cSent ? "taken" : "not taken";
            if (cSent)
                pExtension->KneelsInGraph.store(false, std::memory_order_relaxed);
            const uint32_t cOwnLifeBefore = ActorState::LifeStateOf(cOwnStateBefore);
            if (!cSent && CopyBodyRules::ForceStandAfterRefusedStop(cPlayerCopy, cOwnLifeBefore == 1 || cOwnLifeBefore == 2, first.State1))
            {
                BSFixedString reset("IdleForceDefaultState");
                cpOutcome = apActor->SendAnimationEvent(&reset) ? "not taken; IdleForceDefaultState taken" : "not taken; IdleForceDefaultState not taken either";
            }
            // The copy's own state before the owner's was written over it: what the game judged the action on.
            spdlog::info("ReplayDiag: 'bleedOutStop' refused as an action on {:X}; sent to its graph instead ({}); before it the copy had life state {}, knock state {}, "
                         "health {:.0f}; the owner's life state {}",
                         apActor->formID, cpOutcome, ActorState::LifeStateOf(cOwnStateBefore), (cOwnStateBefore >> 25) & 0x7,
                         apActor->GetActorValue(ActorValueInfo::kHealth), ActorState::LifeStateOf(first.State1));
        }

        // The game refuses its owner's way out of a seat as an action on a copy that was put in its seat here, so its get-up did not play
        // and the copy was stood 2 s later (FurnitureService). Its graph is sent the event itself, as with a refused "bleedOutStop"
        // above: the get-up plays if the graph is in the seat, and is ignored if it is not.
        if (!result && (first.Type & 0x4) == 0 && NpcFurniture::IsSeatExitReplay(first.EventName.c_str()))
        {
            BSFixedString event(first.EventName.c_str());
            const bool cTaken = apActor->SendAnimationEvent(&event);
            spdlog::info(
                "ReplayDiag: seat exit '{}' refused as an action on {:X}; sent to its graph instead ({}); its sit/sleep state was {} before its owner's flags "
                "were written, is {} now, and its owner's was {}",
                first.EventName.c_str(), apActor->formID, cTaken ? "taken" : "not taken", (cOwnStateBefore >> NpcFurniture::kSitSleepShift) & 0xF,
                (apActor->actorState.flags1 >> NpcFurniture::kSitSleepShift) & 0xF, (first.State1 >> NpcFurniture::kSitSleepShift) & 0xF);
        }

        // TEMPORARY: kept to show what state an NPC copy's graph was left in.
        if (!cPlayerCopy)
            Takeover::NoteReplay(apActor->formID, first.EventName.c_str(), result != 0);

        // TEMPORARY diagnostic for remote NPCs sliding while the game refuses most of their moveStart and turnStop replays.
        // For one refused replay per 10 s, it logs everything the refusal could depend on. Remove once understood.
        // A transition without animation (Type bit 4) played nothing on its owner either, so its refusal says nothing.
        if (!result && (first.Type & 0x4) == 0)
        {
            static std::chrono::steady_clock::time_point s_nextSample{};
            static uint32_t s_refused = 0;
            ++s_refused;
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextSample)
            {
                s_nextSample = now + 10s;
                const auto* pBase = Cast<TESNPC>(apActor->baseForm);
                spdlog::info("ReplayDiag: refused '{}' on {:X} ({}): action {:X} resolved {}, idle {:X} resolved {}, type {}, target {:X} resolved {}, graph ready {}, has 3D {}, "
                             "has process {}, flags1 {:#x}, flags2 {:#x}, dead or dying {}, replay left {}, queued {}; {} refused since the last line",
                             first.EventName.c_str(), apActor->formID, pBase ? pBase->fullName.value.AsAscii() : "?", actionId, pAction != nullptr, first.IdleId,
                             actionData.idleForm != nullptr, first.Type, targetId, pTarget != nullptr, apActor->animationGraphHolder.IsReady(),
                             apActor->GetNiNode() != nullptr, apActor->currentProcess != nullptr, apActor->actorState.flags1, apActor->actorState.flags2,
                             apActor->actorState.IsDeadOrDying(), aAnimationComponent.ReplayCount, actions.size(), s_refused);
                s_refused = 0;
            }
        }

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

// Update plays at most one action per frame, so every path that pushes into this queue trims it here.
// A long queue would replay minutes-old actions, leaving the actor acting out the past (an automaton idle through a fight).
// An action this far behind is not worth playing, so the oldest go and the actor catches up with the present.
void AnimationSystem::TrimBacklog(RemoteAnimationComponent& aAnimationComponent, const uint32_t aFormId) noexcept
{
    constexpr size_t cMaxQueued = 96; // over a second of backlog at the frame rate these replay at
    if (aAnimationComponent.TimePoints.size() <= cMaxQueued)
        return;

    const size_t dropped = aAnimationComponent.TimePoints.size() - cMaxQueued;
    for (size_t i = 0; i < dropped; ++i)
        aAnimationComponent.TimePoints.pop_front();

    if (aAnimationComponent.ReplayCount > dropped)
        aAnimationComponent.ReplayCount -= dropped;
    else
        aAnimationComponent.ReplayCount = 0;

    static std::chrono::steady_clock::time_point s_nextSaid{};
    static uint32_t s_since = 0;
    static size_t s_worst = 0;
    static uint32_t s_worstActor = 0;
    ++s_since;
    if (dropped > s_worst)
    {
        s_worst = dropped;
        s_worstActor = aFormId;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= s_nextSaid)
    {
        s_nextSaid = now + 10s;
        spdlog::warn("Animation replay backlog over {} actions; trimmed {} times, worst {} dropped at once on {:X}. An owner is looping an action.", cMaxQueued, s_since, s_worst, s_worstActor);
        s_since = 0;
        s_worst = 0;
        s_worstActor = 0;
    }
}

void AnimationSystem::AddModIds(World& aWorld, ActionEvent& aAction) noexcept
{
    ActionModIds::Fill(aWorld.GetModSystem(), aAction);
}

void AnimationSystem::ResolveModIds(World& aWorld, ActionEvent& aAction) noexcept
{
    ActionModIds::Resolve(aWorld.GetModSystem(), aAction);
}

void AnimationSystem::AddActionsForReplay(World& aWorld, RemoteAnimationComponent& aAnimationComponent, const ActionReplayChain& acReplay) noexcept
{
    for (ActionEvent action : acReplay.Actions)
    {
        ResolveModIds(aWorld, action);
        aAnimationComponent.TimePoints.push_back(std::move(action));
    }
    aAnimationComponent.ReplayCount = acReplay.Actions.size();
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;

    TrimBacklog(aAnimationComponent, 0);
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);

    TrimBacklog(aAnimationComponent, 0);
}

#ifdef SKYRIMVR
// Where the graph's "Direction" float sits among the variables the player sends, or -1 when its behavior has none.
// Looked up by name once per behavior descriptor, because a modded behavior can put it elsewhere.
static int PlayerDirectionSlot(Actor* apPlayer) noexcept
{
    static size_t s_hash = 0;
    static int s_slot = -1;
    const size_t cHash = apPlayer->GetExtension()->GraphDescriptorHash;
    if (cHash == s_hash)
        return s_slot;
    s_hash = cHash;
    s_slot = -1;

    const AnimationGraphDescriptor* pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(cHash);
    BSAnimationGraphManager* pManager = nullptr;
    if (!pDescriptor || !apPlayer->animationGraphHolder.GetBSAnimationGraph(&pManager))
        return s_slot;
    {
        BSScopedLock<BSRecursiveLock> _{pManager->lock};
        // The player's variables are always read from graph 0 (SaveAnimationVariables).
        for (const auto& [index, name] : pManager->DumpAnimationVariables(false, 0))
        {
            if (name != "Direction")
                continue;
            for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
                if (pDescriptor->FloatLookupTable[i] == index)
                    s_slot = static_cast<int>(i);
        }
    }
    pManager->Release();
    return s_slot;
}
#endif

#ifdef SKYRIMVR
namespace
{
// TEMPORARY: a dead body this game owns whose reported position jumps, and what it was read from.
// The others pull their copy after every report, so a jump here is a jump on their screens.
void NoteBodyJump(const Actor* apActor, const glm::vec3& acSent, const bool aFromRoot) noexcept
{
    static std::unordered_map<uint32_t, glm::vec3> s_lastSent;
    if (s_lastSent.size() > 512)
        s_lastSent.clear();
    const auto [it, cIsNew] = s_lastSent.try_emplace(apActor->formID, acSent);
    if (cIsNew)
        return;
    const float cJump = glm::distance(it->second, acSent);
    if (cJump > 300.f)
    {
        const auto* pPlayer = PlayerCharacter::Get();
        spdlog::debug(
            "CorpseDiag: own body {:X} sent {:.0f} units from its last report, from ({:.0f}) to ({:.0f}), read from its {}; "
            "reference at ({:.0f}) in cell {:X}, attached {}, player at ({:.0f}, {:.0f}, {:.0f})",
            apActor->formID, cJump, it->second, acSent, aFromRoot ? "3D root" : "reference", apActor->position, apActor->parentCell ? apActor->parentCell->formID : 0,
            apActor->parentCell && apActor->parentCell->IsAttached(), pPlayer ? pPlayer->position.x : 0.f, pPlayer ? pPlayer->position.y : 0.f,
            pPlayer ? pPlayer->position.z : 0.f);
    }
    it->second = acSent;
}
} // namespace
#endif

void AnimationSystem::SerializeMovement(World& aWorld, Actor* apActor, ReferenceUpdate& aUpdate) noexcept
{
    auto& movement = aUpdate.UpdatedMovement;

    if (const auto pCell = apActor->parentCell)
        aWorld.GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = apActor->GetWorldSpace())
        aWorld.GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = apActor->position;
    // A dead body lies where its ragdoll is, and dragging or throwing it leaves its reference where it died.
    // A living NPC knocked into its ragdoll is carried the same way, as a player picks up a rabbit.
    // The others put their copy where this says (VRBodySync::PlaceCorpse), so the 3D root's world position is sent instead.
    const bool cKnockedNpc = apActor != PlayerCharacter::Get() && apActor->actorState.IsInKnockRagdoll();
    if (apActor != PlayerCharacter::Get() && (apActor->actorState.IsDead() || cKnockedNpc))
    {
        // Not while a load is up: the client's update can run beside the main thread then, which may be freeing the 3D. Its reference
        // position goes instead until the load is over.
        const NiNode* pRoot = LoadingScreenUp() ? nullptr : apActor->GetNiNode();
        if (pRoot)
        {
            const auto& world = WorldTransformOf(pRoot);
            movement.Position = glm::vec3(world.translate.x, world.translate.y, world.translate.z);
        }
#ifdef SKYRIMVR
        if (apActor->actorState.IsDead())
            NoteBodyJump(apActor, static_cast<glm::vec3>(movement.Position), pRoot != nullptr);
#endif
    }

    movement.Rotation.x = apActor->rotation.x;
    movement.Rotation.y = apActor->rotation.z;

    apActor->SaveAnimationVariables(movement.Variables);

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        movement.Direction = apActor->currentProcess->middleProcess->direction;
    }

    // A dragon in the air here is not taken over by another player's game (CharacterService::CanClaimOwnership on the server).
    // Taken over in flight, a dragon falls and dies.
    movement.Flying = apActor->IsDragon() && apActor->actorState.IsFlying();

#ifdef SKYRIMVR
    if (apActor == PlayerCharacter::Get())
    {
        auto* pPlayer = static_cast<PlayerCharacter*>(apActor);

        VRPose pose{};
        float heading = apActor->rotation.z;
        // In the skills and map menus (unpaused online) the others see the body's standing animation, not the
        // hands moving in the menu: no pose is sent. The map also parks the rig at its camera, and it is not home yet
        // when the menu closes. The heading is held; UI.cpp blocks turning while one of these menus is open.
        static float s_heldHeading = 0.f;
        static bool s_hasHeldHeading = false;
        // Without a body plugin, while a weapon or spell is out, the others see the body's own drawn stance (BodySolver.h).
        const bool cSendsPose = BodySolver::SendsPose(VRBodySync::HasBodySolver(), static_cast<uint32_t>(pPlayer->actorState.GetWeaponState()));
        if (IsOptionalMenuOpenUnpaused() || (UnpausedMenusOnline() && VRBodySync::IsRigParked(pPlayer)))
        {
            if (s_hasHeldHeading)
                movement.Rotation.y = s_heldHeading;
        }
        // In any other full menu the game shows a second body, in another pose, so the copy would snap at each opening and closing.
        // The others see this player as they were when it opened, and the heading is held with it.
        else if (IsPausingMenuOpen() && cSendsPose && VRBodySync::LastLocalPose(pPlayer, pose))
        {
            if (s_hasHeldHeading)
                movement.Rotation.y = s_heldHeading;
        }
        // The pose left out: a snapshot with no bones goes instead, as a flat player's, so the copy lets go of the last pose at
        // once and shows its own drawn stance; it carries the held spell's aim while there is one.
        else if (!cSendsPose)
        {
            VRBodySync::CaptureSpellAimOnly(pose);
            s_heldHeading = heading;
            s_hasHeldHeading = true;
        }
        else if (!VRBodySync::CaptureLocalPose(pPlayer, pose, heading))
        {
            // The other player then sees this body in the plain animation pose.
            // Logged so it can be told apart from a problem on the receiving side.
            static std::chrono::steady_clock::time_point s_nextWarn{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextWarn)
            {
                s_nextWarn = now + 10s;
                spdlog::warn("VRBodySync: could not read the local VR pose (skeleton bones not found), the other player sees the animation pose instead");
            }
        }
        else
        {
            // In the air and just after landing the legs are left out, so the copy's legs play the jump, fall and land (JumpSync.h).
            static JumpSync::Legs s_legs;
            static BSFixedString s_inJump("bInJumpState");
            bool inJump = false;
            pPlayer->animationGraphHolder.GetVariableBool(&s_inJump, &inJump);
            if (!s_legs.Sent(inJump, std::chrono::steady_clock::now()))
                pose.HasLegs = false;

            // The body's facing is sent as the heading, so a head turn alone does not turn the copy's hips and legs.
            // The game's walk direction is relative to its own heading, which follows the headset, so it is re-based on the body.
            float turn = std::fmod(heading - apActor->rotation.z, glm::two_pi<float>());
            if (turn > glm::pi<float>())
                turn -= glm::two_pi<float>();
            else if (turn < -glm::pi<float>())
                turn += glm::two_pi<float>();
            movement.Rotation.y = heading;
            // The process direction is in radians from -pi to pi: 0 forward, pi/2 right.
            if (apActor->currentProcess && apActor->currentProcess->middleProcess)
            {
                float direction = std::fmod(movement.Direction - turn, glm::two_pi<float>());
                if (direction > glm::pi<float>())
                    direction -= glm::two_pi<float>();
                else if (direction < -glm::pi<float>())
                    direction += glm::two_pi<float>();
                movement.Direction = direction;
            }
            // The graph's Direction is a fraction of a turn clockwise from the heading: 0 forward, 0.25 right.
            if (const int cSlot = PlayerDirectionSlot(apActor); cSlot >= 0 && static_cast<size_t>(cSlot) < movement.Variables.Floats.size())
            {
                float& direction = movement.Variables.Floats[cSlot];
                direction -= turn / glm::two_pi<float>();
                direction -= std::floor(direction);
            }
            s_heldHeading = heading;
            s_hasHeldHeading = true;
        }
        aUpdate.UpdatedVRPose = pose;
    }
    else if (apActor->actorState.IsDead() || cKnockedNpc)
#else
    if (apActor == PlayerCharacter::Get())
    {
        // In a conversation the others see this player face the one they talk to, whichever way the game leaves the body here.
        if (const MenuTopicManager* pTopics = MenuTopicManager::Get(); pTopics && pTopics->menuOpen && pTopics->speaker)
        {
            if (const TESObjectREFR* pSpeaker = TESObjectREFR::GetByHandle(pTopics->speaker.handle.iBits); pSpeaker && pSpeaker != apActor)
            {
                if (const auto cHeading = DialogueFacing::HeadingTo(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(pSpeaker->position)))
                    movement.Rotation.y = *cHeading;
            }
        }

        // No tracked body on flat: the held spell's aim rides a boneless, rootless snapshot while it is held.
        VRPose pose{};
        if (VRBodySync::CaptureSpellAimOnly(pose))
            aUpdate.UpdatedVRPose = pose;
    }
    else if (apActor->actorState.IsDead() || cKnockedNpc)
#endif
    {
        // A body this machine owns, dead or knocked down, within reach of this player: its bones let the others see it carried, dragged, thrown or shoved.
        // Skyrim SE sends them too, so a VR player sees a body dragged with the grab key bend as it does here.
        // It is only sent while the body is moving (CaptureBodyPose returns false otherwise), and far bodies are never read.
        // Measured from where the body lies (movement.Position above), not where it died: a body dragged far away is still in reach.
        constexpr float cReachSquared = 600.f * 600.f;
        const auto* pPlayer = PlayerCharacter::Get();
        if (pPlayer)
        {
            const glm::vec3 toPlayer = static_cast<const glm::vec3&>(movement.Position) - static_cast<glm::vec3>(pPlayer->position);
            if (glm::dot(toPlayer, toPlayer) <= cReachSquared)
            {
                VRPose pose{};
                if (VRBodySync::CaptureBodyPose(apActor, pose))
                    aUpdate.UpdatedVRPose = pose;
            }
        }
    }
}

void AnimationSystem::SerializeActions(RequestActionEvents& aMessage, LocalComponent& localComponent, LocalAnimationComponent& animationComponent) noexcept
{
    if (animationComponent.Actions.empty())
        return;

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    aMessage.Actions[localComponent.Id] = std::move(animationComponent.Actions);
    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}
