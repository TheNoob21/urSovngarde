#pragma once

#include <Structs/ActionEvent.h>
#include <atomic>

struct ActorExtension
{
    enum
    {
        kRemote = 1 << 0,
        kPlayer = 1 << 1,
        kFollowerCopy = 1 << 2,
        kHiddenHere = 1 << 3,
        kOthersFollower = 1 << 4,
        kOtherGame = 1 << 5,
    };

    enum class ReconciliationStage
    {
        None,
        WaitingForDisable,
        WaitingFor3D
    };

    bool IsRemote() const noexcept;
    bool IsLocal() const noexcept;
    bool IsPlayer() const noexcept;
    bool IsRemotePlayer() const noexcept;
    bool IsLocalPlayer() const noexcept;
    //! A temporary actor showing another player's follower.
    bool IsFollowerCopy() const noexcept;
    //! Another player's follower, on a copy or, without the save guard (flat), on this game's own reference.
    bool IsOthersFollower() const noexcept;
    //! A reference of this game's own disabled while a copy stands in for it; a save still has it enabled.
    bool IsHiddenHere() const noexcept;
    //! Another player's copy whose player plays the other game: flat Skyrim against Skyrim VR (PvpRules).
    bool IsOtherGame() const noexcept;
    void SetRemote(bool aSet) noexcept;
    void SetPlayer(bool aSet) noexcept;
    void SetFollowerCopy(bool aSet) noexcept;
    void SetOthersFollower(bool aSet) noexcept;
    void SetHiddenHere(bool aSet) noexcept;
    void SetOtherGame(bool aSet) noexcept;

    ActionEvent LatestAnimation{};
    size_t GraphDescriptorHash = 0;
    // The behavior project GraphDescriptorHash was taken from; the hash is taken again when it changes.
    uint64_t GraphDescriptorProjectKey = 0;

    //! Its owner's game, this one or another, revived it since its last death, as this game last heard.
    //! An announcement from here carries it (CharacterService::BuildActorData).
    bool RevivedSinceDeath = false;
    // The revive count (NativeLifeGeneration) the copy was seen to fall in, or 0. Set on the main thread; actor jobs only read it,
    // so that the game's own get-up can run.
    std::atomic<uint64_t> ObservedCopyFall{0};
    // How many times the game has revived this actor, counting a script reviving one that is up already.
    std::atomic<uint64_t> NativeLifeGeneration{1};
    // A copy's owner's last word of its NPC (CopyBodyRules::OwnerWord), written by InterpolationSystem on the main thread.
    // The game may end the copy's bleedout from an actor job, which reads it (HookPerformAction).
    std::atomic<uint8_t> OwnerWordOfCopy{0};
    // The copy's graph kneels while its life says alive (CopyBodyRules::KneelsInGraphOnly), until it gets up.
    std::atomic<bool> KneelsInGraph{false};
    // Whether the game's own end of the copy's bleedout played its stop: 0 not tried since it was cleared, 1 played, 2 refused.
    std::atomic<uint8_t> EngineStopPlayed{0};
    // The tick of its owner's latest "bleedOutStart" replayed on the copy, or 0: an owner's stop from before it belongs to an earlier fall.
    std::atomic<uint64_t> OwnerFallTick{0};
    // A player copy mirroring its owner's fall (PlayerKnockMirror): its process runs so that the game's fall and get-up play.
    // Written by InterpolationSystem on the main thread, read by HookActorProcess.
    std::atomic<bool> OwnerKnockRuns{false};

    //! Clears the copy's bleedout recovery above: it changed hands, was removed, revived, or the session ended.
    void ForgetCopyBleedout() noexcept;
    //! An NPC copy made from a spawn whose 3D step has not run yet (CharacterService::RunRemoteUpdates); a death in between is logged (SpawnDeathDiag).
    bool AwaitingFirst3D = false;
    //! A copy of another player's summon, marked here as commanded by this game's player so it turns on no one.
    //! That mark is ours, not a spell's, so it does not mean a spell is raising the body (Actor::IsBeingRaised).
    bool CommandedAsSummonCopy = false;

    // TODO: atomic? bool instead? maybe simplify to `IsReenabling()` ?
    // Protects discovery while rebuilding a leveled NPC.
    ReconciliationStage Reconciliation{ReconciliationStage::None};

private:
    uint32_t onlineFlags{0};
};
