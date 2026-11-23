#include <catch2/catch.hpp>

#include <Games/Skyrim/CopyBodyRules.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace CopyBodyRules;

namespace
{
constexpr uint32_t Flags(const uint32_t aLife, const uint32_t aKnock) noexcept
{
    return (aLife << 21) | (aKnock << 25);
}

constexpr uint32_t cStanding = Flags(0, 0);
constexpr uint32_t cBleedout = Flags(8, 5);
constexpr uint32_t cEssentialDown = Flags(7, 0);
constexpr uint32_t cDead = Flags(2, 0);
constexpr uint32_t cThrown = Flags(0, 5);

// Lydia's copy in its own bleedout, her owner's Lydia up again for a while: the 11-06 case, which takes her out.
BleedingOut StuckLydia()
{
    BleedingOut copy{};
    copy.BleedingOutHere = true;
    copy.OwnerWordKnown = true;
    copy.OwnerFlags1 = cStanding | 0x1F; // other bits of the word say nothing here
    copy.OwnerUpForMs = kTakeOutAfterMs;
    return copy;
}
} // namespace

TEST_CASE("the state word's life and knock states", "[copybody]")
{
    CHECK(LifeState(cBleedout) == 8);
    CHECK(KnockState(cBleedout) == 5);
    CHECK(OwnerStands(cStanding));
    CHECK(OwnerStands(Flags(6, 0))); // restrained
    CHECK_FALSE(OwnerStands(cEssentialDown));
    CHECK_FALSE(OwnerStands(cBleedout));
    CHECK_FALSE(OwnerStands(cDead));
    CHECK_FALSE(OwnerStands(Flags(1, 0)));
    CHECK_FALSE(OwnerStands(cThrown));
    CHECK_FALSE(OwnerStands(Flags(0, 6))); // still getting up
}

TEST_CASE("only an accepted fall on the same live NPC copy permits bone observation", "[copybody]")
{
    CHECK(MayObserveFall(true, true, false, false, true));
    CHECK_FALSE(MayObserveFall(false, true, false, false, true));
    CHECK_FALSE(MayObserveFall(true, false, false, false, true));
    CHECK_FALSE(MayObserveFall(true, true, true, false, true));
    CHECK_FALSE(MayObserveFall(true, true, false, true, true));
    CHECK_FALSE(MayObserveFall(true, true, false, false, false));
}

TEST_CASE("an accepted ragdoll needs consecutive low bones and a real get-up", "[copybody]")
{
    ObservedFall fall{};
    CHECK_FALSE(ObserveFall(fall, true, 24.f, false));
    CHECK_FALSE(ObserveFall(fall, true, 65.f, false));
    CHECK_FALSE(ObserveFall(fall, true, 24.f, false));
    CHECK(ObserveFall(fall, true, 24.f, false));
    CHECK(ObserveFall(fall, false, 65.f, false));
    CHECK(ObserveFall(fall, true, 40.f, false));
    CHECK(ObserveFall(fall, true, 41.f, false));
    CHECK_FALSE(ObserveFall(fall, true, 41.f, false));
}

TEST_CASE("essential graph falls include kneeling but ordinary ragdolls must lie flat", "[copybody]")
{
    ObservedFall ragdoll{}, essential{};
    for (int sample = 0; sample < 2; ++sample)
    {
        CHECK_FALSE(ObserveFall(ragdoll, true, 31.f, false));
        ObserveFall(essential, true, 31.f, true);
    }
    CHECK(essential.Down);
    CHECK_FALSE(ragdoll.Down);
    ObservedFall standing{};
    ObserveFall(standing, true, 65.f, true);
    CHECK_FALSE(ObserveFall(standing, true, 65.f, true));
}

TEST_CASE("only a refused real Ragdoll on the same live remote NPC may retry its graph event", "[copybody]")
{
    CHECK(MayRetryRagdoll(false, true, true, false, false, true));
    CHECK_FALSE(MayRetryRagdoll(true, true, true, false, false, true)); // accepted actions are never replayed twice
    CHECK_FALSE(MayRetryRagdoll(false, false, true, false, false, true));
    CHECK_FALSE(MayRetryRagdoll(false, true, false, false, false, true)); // player/local actors
    CHECK_FALSE(MayRetryRagdoll(false, true, true, true, false, true));
    CHECK_FALSE(MayRetryRagdoll(false, true, true, false, true, true));
    CHECK_FALSE(MayRetryRagdoll(false, true, true, false, false, false));
}

TEST_CASE("a living NPC copy that refused its owner's Ragdoll both ways is knocked down by the game", "[copybody]")
{
    CHECK(KnockAfterRefusedRagdoll(true, false, true, true, false, false, true, cEssentialDown));
    CHECK(KnockAfterRefusedRagdoll(true, false, true, true, false, false, true, cStanding));
    CHECK_FALSE(KnockAfterRefusedRagdoll(false, false, true, true, false, false, true, cEssentialDown)); // switched off
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, true, true, true, false, false, true, cEssentialDown));   // its graph took it
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, false, true, false, false, true, cEssentialDown)); // not a Ragdoll
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, true, false, false, false, true, cEssentialDown)); // a player's copy
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, true, true, true, false, true, cEssentialDown));   // dead here
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, true, true, false, true, true, cEssentialDown));   // dead at its owner
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, true, true, false, false, false, cEssentialDown)); // another life
    CHECK_FALSE(KnockAfterRefusedRagdoll(true, false, true, true, false, false, true, cBleedout));       // already knocked here
}

TEST_CASE("an essential copy at its health floor kneels where its owner's NPC starts its bleedout", "[copybody]")
{
    CHECK(KneelsInsteadOfFalling(true, true, true, true, true, false, false, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(false, true, true, true, true, false, false, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(true, false, true, true, true, false, false, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, false, true, true, false, false, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, false, true, false, false, cStanding)); // not essential: a real fall
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, true, false, false, false, cStanding)); // healthy: a push
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, true, true, true, false, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, true, true, false, true, cStanding));
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, true, true, false, false, cEssentialDown)); // thrown from its bleedout
    CHECK_FALSE(KneelsInsteadOfFalling(true, true, true, true, true, false, false, cThrown));
}

TEST_CASE("an NPC copy is at its owner's health floor once its owner's 0 came, though it healed here", "[copybody]")
{
    const float cNone = std::numeric_limits<float>::quiet_NaN();
    CHECK(AtOwnersHealthFloor(1.f, cNone, 1.f));
    CHECK(AtOwnersHealthFloor(8.f, 0.f, 1.f)); // healed a little here; its owner's NPC is at 0
    CHECK(AtOwnersHealthFloor(8.f, -30.f, 1.f));
    CHECK_FALSE(AtOwnersHealthFloor(8.f, cNone, 1.f)); // no owner word: its own health decides
    CHECK_FALSE(AtOwnersHealthFloor(8.f, 100.f, 1.f)); // healthy at its owner: a push
}

TEST_CASE("on flat, its owner's bones do not keep a knocked copy from where its owner's NPC fell", "[copybody]")
{
    CHECK_FALSE(OwnerBonesHoldBody(false, false, true));
    CHECK_FALSE(OwnerBonesHoldBody(true, false, true)); // flat never poses an NPC from them
    CHECK(OwnerBonesHoldBody(true, false, false));      // a corpse they arrive for still waits
    CHECK(OwnerBonesHoldBody(true, true, true));        // VR poses it from them
    CHECK(OwnerBonesHoldBody(true, true, false));
}

TEST_CASE("a copy kneeling in its essential fall that takes its owner's Ragdoll is knocked down by the game too", "[copybody]")
{
    CHECK(KnockAfterKneelingRagdoll(true, true, true, true, false, false, true, cEssentialDown));
    CHECK(KnockAfterKneelingRagdoll(true, true, true, true, false, false, true, Flags(8, 0)));
    CHECK_FALSE(KnockAfterKneelingRagdoll(false, true, true, true, false, false, true, cEssentialDown)); // switched off
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, false, true, true, false, false, true, cEssentialDown)); // refused: the path above
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, false, true, false, false, true, cEssentialDown)); // not a Ragdoll
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, false, false, false, true, cEssentialDown)); // a player's copy
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, true, true, false, true, cEssentialDown));   // dead here
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, true, false, true, true, cEssentialDown));   // dead at its owner
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, true, false, false, false, cEssentialDown)); // another life
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, true, false, false, true, cStanding));       // a plain push: its own knock comes
    CHECK_FALSE(KnockAfterKneelingRagdoll(true, true, true, true, false, false, true, cBleedout));       // already knocked here
}

TEST_CASE("an essential fall is held down while its replayed push throws it over", "[copybody]")
{
    CHECK(HoldEssentialFall(true, true, 0));
    CHECK(HoldEssentialFall(true, true, kKnockProofMs - 1));
    CHECK_FALSE(HoldEssentialFall(true, true, kKnockProofMs)); // never seen flat: its pelvis decides again
    CHECK_FALSE(HoldEssentialFall(true, false, 0));            // no push replayed: a kneel it rises from
    CHECK_FALSE(HoldEssentialFall(false, true, 0));            // already tracking the flat ragdoll
}

TEST_CASE("a refused bleedOutStart goes to a living copy's graph", "[copybody]")
{
    CHECK(SendRefusedBleedOutStart(true, false, false, false));
    // An NPC copy only where flat ragdolls are moved.
    CHECK_FALSE(SendRefusedBleedOutStart(false, false, false, false));
    // A downed player's copy on every screen, VR included.
    CHECK(SendRefusedBleedOutStart(true, true, false, false));
    CHECK(SendRefusedBleedOutStart(false, true, false, false));
    // Never a copy dead or dying here, or dead at its owner's.
    CHECK_FALSE(SendRefusedBleedOutStart(true, false, true, false));
    CHECK_FALSE(SendRefusedBleedOutStart(true, false, false, true));
    CHECK_FALSE(SendRefusedBleedOutStart(false, true, true, false));
    CHECK_FALSE(SendRefusedBleedOutStart(false, true, false, true));
}

TEST_CASE("a player's copy kneeling in its graph stands once its owner is up or has moved away", "[copybody]")
{
    // Its owner's word from after the kneel: a later action with them on their feet, or their own "bleedOutStop", whose state word
    // is still the bleedout it ends.
    CHECK(PlayerUpAfterKneel(true, false, cStanding));
    CHECK(PlayerUpAfterKneel(true, true, cBleedout));
    // A word from before the kneel, or a later one that has them down, is not.
    CHECK_FALSE(PlayerUpAfterKneel(false, false, cStanding));
    CHECK_FALSE(PlayerUpAfterKneel(true, false, cBleedout));
    CHECK_FALSE(PlayerUpAfterKneel(true, false, cEssentialDown));
    CHECK_FALSE(PlayerUpAfterKneel(true, false, cThrown));

    // Up long enough: up. Not for long enough yet: still kneeling.
    CHECK(PlayerKneelEnds(true, kTakeOutAfterMs, 0.f));
    CHECK_FALSE(PlayerKneelEnds(true, kTakeOutAfterMs - 1, 0.f));
    CHECK_FALSE(PlayerKneelEnds(false, kTakeOutAfterMs * 10, 0.f));
    // A revived player who stands still and sends nothing after the stop that the copy refused still gets up, on the stop.
    CHECK(PlayerKneelEnds(PlayerUpAfterKneel(true, true, cBleedout), kTakeOutAfterMs, 0.f));

    // A downed player does not move, so one whose reported place moved is up (revived, or respawned elsewhere), word or no word.
    CHECK(PlayerKneelEnds(false, 0, kKneelMovedUnits));
    CHECK(PlayerKneelEnds(false, 0, 5000.f));
    CHECK_FALSE(PlayerKneelEnds(false, 0, kKneelMovedUnits - 1.f));
    CHECK_FALSE(PlayerKneelEnds(false, 0, std::numeric_limits<float>::quiet_NaN()));

    // Measured on the owner's reports: the owner went down sprinting at x 1000 and stays there while its copy is still catching
    // up from x 880. The copy moves 120 units more, its owner none: still kneeling.
    const float cOwnerAtKneel = 1000.f;
    const float cOwnerNow = 1000.f;
    CHECK_FALSE(PlayerKneelEnds(false, 0, std::abs(cOwnerNow - cOwnerAtKneel)));
    // Pushed by a hit a little: still kneeling. Walked off once revived: up.
    CHECK_FALSE(PlayerKneelEnds(false, 0, std::abs(1060.f - cOwnerAtKneel)));
    CHECK(PlayerKneelEnds(false, 0, std::abs(1200.f - cOwnerAtKneel)));
}

TEST_CASE("an essential fall's observed knees cannot stand in for its later flat ragdoll", "[copybody]")
{
    ObservedFall essential{}, laterKnock{};
    ObserveFall(essential, true, 31.f, true);
    REQUIRE(ObserveFall(essential, true, 31.f, true));
    CHECK_FALSE(ObserveFall(laterKnock, true, 31.f, false));
    CHECK_FALSE(ObserveFall(laterKnock, true, 24.f, false));
    CHECK(ObserveFall(essential, true, 31.f, true)); // keep the established fall while the new one is unproved
    REQUIRE(ObserveFall(laterKnock, true, 24.f, false));
    CHECK(ObserveFall(laterKnock, true, 41.f, false));
    CHECK_FALSE(ObserveFall(laterKnock, true, 41.f, false)); // a real get-up can enter the original rising path
}

TEST_CASE("normal-looking owner words alone cannot recover an observed graph fall", "[copybody]")
{
    OwnerRecoveryEvidence proof{};
    CHECK_FALSE(proof.Allows(10, 20));
    proof.GetUpTick = 10;
    CHECK_FALSE(proof.Allows(10, 20));
    proof.GetUpTick = 15;
    CHECK_FALSE(proof.Allows(10, 14));
    CHECK(proof.Allows(10, 15));
    CHECK(proof.Allows(10, 20));
    CHECK_FALSE(proof.Allows(0, 20));
    CHECK_FALSE(proof.Allows(15, 20));

    BleedingOut copy = StuckLydia();
    copy.BleedingOutHere = ObservedWithoutNativeKnock(true, cStanding);
    copy.OwnerWordKnown = proof.Allows(10, 20);
    copy.OwnerUpForMs = kTakeOutAfterMs - 1;
    CHECK(OnBleedingOut(copy) == Step::Wait);
    copy.OwnerUpForMs = kTakeOutAfterMs;
    CHECK(OnBleedingOut(copy) == Step::TakeOut);
    copy.OwnerFlags1 = cEssentialDown;
    CHECK(OnBleedingOut(copy) == Step::Leave); // the later word must also actually say up
    copy.OwnerFlags1 = cStanding;
    copy.DeadAtOwner = true;
    CHECK(OnBleedingOut(copy) == Step::Leave);
    copy.DeadAtOwner = false;
    copy.PlayerCopy = true;
    CHECK(OnBleedingOut(copy) == Step::Leave);
    CHECK_FALSE(ObservedWithoutNativeKnock(true, cThrown)); // native knock keeps its original recovery
    CHECK_FALSE(ObservedWithoutNativeKnock(false, cStanding));
}

TEST_CASE("a same-life alive word needs a subsequent healthy value after the observed fall", "[copybody]")
{
    OwnerRecoveryEvidence proof{};
    proof.Health(true);
    CHECK_FALSE(proof.Allows(10, 20));
    proof.Alive();
    CHECK_FALSE(proof.Allows(10, 20));
    proof.Health(false);
    CHECK_FALSE(proof.Allows(10, 20));
    proof.Health(true);
    CHECK(proof.Allows(10, 20));
    CHECK_FALSE(proof.Allows(10, 10)); // stale pre-fall action is not an owner-up word
    proof.Alive();
    CHECK_FALSE(proof.Allows(10, 20)); // no reusing health from before this alive word
    proof.Health(true);
    proof = {}; // new fall/life/authority withdraws all proof
    CHECK_FALSE(proof.Allows(10, 20));
}

TEST_CASE("an NPC copy bleeding out here is taken out once its owner's NPC has been up 2 s", "[copybody]")
{
    CHECK(OnBleedingOut(StuckLydia()) == Step::TakeOut);

    BleedingOut early = StuckLydia();
    early.OwnerUpForMs = kTakeOutAfterMs - 1;
    CHECK(OnBleedingOut(early) == Step::Wait);
}

TEST_CASE("a replayed alive word cannot erase a copy's local bleedout before recovery", "[copybody]")
{
    for (const uint32_t cLocal : {cEssentialDown, cBleedout})
    {
        REQUIRE(KeepLifeStateHere(false, cLocal));
        // The owner's later action says up, while the local native down state remains available to the recovery gate.
        BleedingOut copy = StuckLydia();
        copy.BleedingOutHere = LifeState(cLocal) == 7 || LifeState(cLocal) == 8;
        CHECK(OnBleedingOut(copy) == Step::TakeOut);
        copy.DeadAtOwner = true;
        CHECK(OnBleedingOut(copy) == Step::Leave);
    }
    CHECK(KeepLifeStateHere(false, Flags(1, 0)));
    CHECK(KeepLifeStateHere(false, cDead));
    CHECK(KeepLifeStateHere(true, cStanding));
    CHECK_FALSE(KeepLifeStateHere(false, cStanding));
    CHECK_FALSE(KeepLifeStateHere(false, Flags(6, 0)));
}

TEST_CASE("the essential get-up never fires when the owner says dead", "[copybody]")
{
    BleedingOut dead = StuckLydia();
    dead.DeadAtOwner = true;
    CHECK(OnBleedingOut(dead) == Step::Leave);

    // The owner's word itself says dead or dying: no take-out either.
    BleedingOut word = StuckLydia();
    word.OwnerFlags1 = cDead;
    CHECK(OnBleedingOut(word) == Step::Leave);
    word.OwnerFlags1 = Flags(1, 0);
    CHECK(OnBleedingOut(word) == Step::Leave);
}

TEST_CASE("a copy is left down while its owner's word has it down, or before any word came", "[copybody]")
{
    BleedingOut down = StuckLydia();
    down.OwnerFlags1 = cEssentialDown;
    CHECK(OnBleedingOut(down) == Step::Leave);
    down.OwnerFlags1 = cBleedout;
    CHECK(OnBleedingOut(down) == Step::Leave);
    down.OwnerFlags1 = cThrown;
    CHECK(OnBleedingOut(down) == Step::Leave);

    // No action of its owner's yet: the default word reads alive, but says nothing.
    BleedingOut unknown = StuckLydia();
    unknown.OwnerWordKnown = false;
    CHECK(OnBleedingOut(unknown) == Step::Leave);
}

TEST_CASE("not for a player's copy, a copy not bleeding out, or one just taken out", "[copybody]")
{
    BleedingOut player = StuckLydia();
    player.PlayerCopy = true;
    CHECK(OnBleedingOut(player) == Step::Leave);

    BleedingOut standing = StuckLydia();
    standing.BleedingOutHere = false;
    CHECK(OnBleedingOut(standing) == Step::Leave);

    BleedingOut again = StuckLydia();
    again.TakenOutLately = true;
    CHECK(OnBleedingOut(again) == Step::Leave);
}

TEST_CASE("a refused bleedOutStop stands a player's copy, and an NPC copy whose owner is up", "[copybody]")
{
    CHECK(ForceStandAfterRefusedStop(true, false, cBleedout));
    CHECK(ForceStandAfterRefusedStop(false, false, cStanding));
    CHECK_FALSE(ForceStandAfterRefusedStop(false, false, cBleedout));
    CHECK_FALSE(ForceStandAfterRefusedStop(false, false, cEssentialDown));
    CHECK_FALSE(ForceStandAfterRefusedStop(false, true, cStanding));
}

TEST_CASE("a knocked copy is put where its owner's NPC fell once it has come to rest", "[copybody]")
{
    Knocked copy{kKnockQueued, true, 200.f};
    CHECK(PlaceWhereOwnerFell(copy));

    Knocked falling = copy;
    falling.KnockStateHere = 3;
    CHECK(PlaceWhereOwnerFell(falling)); // and then only once at rest (KnockedAtRest)
    CHECK(KnockedAtRest(3, 2.f));
    CHECK_FALSE(KnockedAtRest(3, 50.f));
    CHECK(KnockedAtRest(kKnockQueued, 50.f));
    CHECK_FALSE(KnockedAtRest(0, 0.f));
    CHECK_FALSE(KnockedAtRest(6, 0.f)); // getting up

    Knocked standing = copy;
    standing.KnockStateHere = 0;
    CHECK_FALSE(PlaceWhereOwnerFell(standing));

    Knocked near = copy;
    near.ApartUnits = kApartUnits;
    CHECK_FALSE(PlaceWhereOwnerFell(near));

    Knocked ownerUp = copy;
    ownerUp.OwnerStillDown = false;
    CHECK_FALSE(PlaceWhereOwnerFell(ownerUp));
}

TEST_CASE("the owner counts as down until a word since the copy fell has it up", "[copybody]")
{
    CHECK(OwnerStillDown(false, cStanding));
    CHECK(OwnerStillDown(true, cThrown));
    CHECK(OwnerStillDown(true, cBleedout));
    CHECK_FALSE(OwnerStillDown(true, cStanding));
}

TEST_CASE("a ragdoll is never moved while dying, bleeding out, posed by its owner, held or looted here", "[copybody]")
{
    BodyHere corpse{};
    corpse.Dead = true;
    CHECK(MayMoveRagdoll(corpse));

    for (bool BodyHere::* pBlock : {&BodyHere::Dying, &BodyHere::BleedingOut, &BodyHere::OwnerBonesArrive, &BodyHere::HeldHere, &BodyHere::LootOpenNear})
    {
        BodyHere blocked = corpse;
        blocked.*pBlock = true;
        CHECK_FALSE(MayMoveRagdoll(blocked));
    }

    // A living body: only knocked to be put where its owner fell, or carried over there just now.
    BodyHere living{};
    CHECK_FALSE(MayMoveRagdoll(living));
    living.KnockedWhereOwnerFell = true;
    CHECK(MayMoveRagdoll(living));
    living.KnockedWhereOwnerFell = false;
    living.CarriedLately = true;
    CHECK(MayMoveRagdoll(living));
    living.BleedingOut = true;
    CHECK_FALSE(MayMoveRagdoll(living));
}

TEST_CASE("a dead copy settles into its owner's last pose while its one-shot may start", "[copybody]")
{
    CHECK(SettleIntoOwnerPose(true, true, OneShot{}));
    CHECK_FALSE(SettleIntoOwnerPose(true, true, OneShot{0, false, true}));              // done
    CHECK_FALSE(SettleIntoOwnerPose(true, true, OneShot{0, true, false}));              // queued, waiting to run
    CHECK_FALSE(SettleIntoOwnerPose(true, true, OneShot{kMaxMoveTries, false, false})); // given up
    CHECK_FALSE(SettleIntoOwnerPose(true, false, OneShot{}));
    CHECK_FALSE(SettleIntoOwnerPose(false, true, OneShot{}));
}

TEST_CASE("a rest pose sent before the copy's ragdoll is held until it is there, settled into once, then forgotten", "[copybody]")
{
    // Its 3D or ragdoll is not there yet.
    CHECK(OnHeldRestPose(false, false, false) == HeldRestPose::Keep);
    CHECK(OnHeldRestPose(false, true, false) == HeldRestPose::Settle);
    // Its owner's bones place it themselves, and their last pose is the one settled into.
    CHECK(OnHeldRestPose(true, false, false) == HeldRestPose::Forget);
    CHECK(OnHeldRestPose(true, true, false) == HeldRestPose::Forget);
    // Sent again with a spawn while the copy already lies in it.
    CHECK(OnHeldRestPose(false, true, true) == HeldRestPose::Forget);
    CHECK(OnHeldRestPose(false, false, true) == HeldRestPose::Forget);

    // Handed to the settle with a fresh one-shot: one settle that lays it out, then none.
    OneShot settle{};
    REQUIRE(SettleIntoOwnerPose(true, true, settle));
    settle = OnQueued(settle);
    CHECK_FALSE(SettleIntoOwnerPose(true, true, settle));
    settle = OnFinished(settle, MoveKind::Settle, MoveOutcome::Posed);
    CHECK_FALSE(SettleIntoOwnerPose(true, true, settle));
}

TEST_CASE("a move's outcome is what it wrote", "[copybody]")
{
    CHECK(OutcomeOf(0, 15, true) == MoveOutcome::NotWritten);
    CHECK(OutcomeOf(0, 0, false) == MoveOutcome::NotWritten);
    CHECK(OutcomeOf(7, 15, true) == MoveOutcome::Partial);
    CHECK(OutcomeOf(15, 15, false) == MoveOutcome::Translated);
    CHECK(OutcomeOf(15, 15, true) == MoveOutcome::Posed);

    CHECK_FALSE(BodyMoved(MoveOutcome::Canceled));
    CHECK_FALSE(BodyMoved(MoveOutcome::NotWritten));
    CHECK(BodyMoved(MoveOutcome::Partial));
    CHECK(BodyMoved(MoveOutcome::Translated));
    CHECK(BodyMoved(MoveOutcome::Posed));
}

TEST_CASE("a settle succeeds only when posed; a place or knock place when every body moved", "[copybody]")
{
    for (const MoveKind kind : {MoveKind::Place, MoveKind::Settle, MoveKind::KnockPlace})
    {
        CHECK_FALSE(Succeeded(kind, MoveOutcome::Canceled));
        CHECK_FALSE(Succeeded(kind, MoveOutcome::NotWritten));
        CHECK_FALSE(Succeeded(kind, MoveOutcome::Partial));
        CHECK(Succeeded(kind, MoveOutcome::Posed));
    }
    // The translation fallback of a settle whose pose could not be read is not the owner's pose.
    CHECK_FALSE(Succeeded(MoveKind::Settle, MoveOutcome::Translated));
    CHECK(Succeeded(MoveKind::Place, MoveOutcome::Translated));
    CHECK(Succeeded(MoveKind::KnockPlace, MoveOutcome::Translated));
}

TEST_CASE("a one-shot is queued, then done only on success", "[copybody]")
{
    OneShot shot{};
    REQUIRE(MayStart(shot));

    shot = OnQueued(shot);
    CHECK(shot.Pending);
    CHECK_FALSE(shot.Done);
    CHECK_FALSE(MayStart(shot)); // not queued twice

    shot = OnFinished(shot, MoveKind::Settle, MoveOutcome::Posed);
    CHECK_FALSE(shot.Pending);
    CHECK(shot.Done);
    CHECK(shot.Failures == 0);
    CHECK_FALSE(MayStart(shot));
    CHECK_FALSE(GaveUp(shot));

    // A late end after it is done changes nothing.
    shot = OnFinished(shot, MoveKind::Settle, MoveOutcome::NotWritten);
    CHECK(shot.Done);
    CHECK(shot.Failures == 0);
}

TEST_CASE("canceled, unwritten, partial and translated-only ends are failures that leave the one-shot to try again", "[copybody]")
{
    for (const MoveOutcome outcome : {MoveOutcome::Canceled, MoveOutcome::NotWritten, MoveOutcome::Partial, MoveOutcome::Translated})
    {
        OneShot shot = OnFinished(OnQueued(OneShot{}), MoveKind::Settle, outcome);
        CHECK_FALSE(shot.Pending);
        CHECK_FALSE(shot.Done);
        CHECK(shot.Failures == 1);
        CHECK(MayStart(shot));
    }
    // A knocked copy moved by one offset has done what it is for.
    CHECK(OnFinished(OnQueued(OneShot{}), MoveKind::KnockPlace, MoveOutcome::Translated).Done);
    CHECK_FALSE(OnFinished(OnQueued(OneShot{}), MoveKind::KnockPlace, MoveOutcome::Canceled).Done);
}

TEST_CASE("a one-shot is given up after kMaxMoveTries failures, and a success before that still counts", "[copybody]")
{
    static_assert(kMaxMoveTries == 3);
    OneShot shot{};
    for (uint8_t i = 0; i < kMaxMoveTries; ++i)
    {
        REQUIRE(MayStart(shot));
        CHECK_FALSE(GaveUp(shot));
        shot = OnFinished(OnQueued(shot), MoveKind::KnockPlace, MoveOutcome::Canceled);
    }
    CHECK(shot.Failures == kMaxMoveTries);
    CHECK(GaveUp(shot));
    CHECK_FALSE(MayStart(shot));
    // Not counted past the bound.
    shot = OnFinished(shot, MoveKind::KnockPlace, MoveOutcome::NotWritten);
    CHECK(shot.Failures == kMaxMoveTries);

    OneShot late{};
    late = OnFinished(OnQueued(late), MoveKind::Settle, MoveOutcome::NotWritten);
    late = OnFinished(OnQueued(late), MoveKind::Settle, MoveOutcome::Translated);
    late = OnFinished(OnQueued(late), MoveKind::Settle, MoveOutcome::Posed);
    CHECK(late.Done);
    CHECK_FALSE(GaveUp(late));
}

TEST_CASE("a copy's authority is its session, owner, entity and generation", "[copybody]")
{
    constexpr CopyAuthority cAuthority{2, 7, (uint64_t{0x600049} << 32) | 0xFF00127E, 41};
    CHECK(cAuthority.Valid());
    CHECK((cAuthority == CopyAuthority{2, 7, (uint64_t{0x600049} << 32) | 0xFF00127E, 41}));
    for (uint64_t CopyAuthority::* pField : {&CopyAuthority::Session, &CopyAuthority::Owner, &CopyAuthority::Entity, &CopyAuthority::Generation})
    {
        CopyAuthority other = cAuthority;
        other.*pField += 1;
        CHECK(other != cAuthority);
    }
    CHECK_FALSE(CopyAuthority{}.Valid());
    CHECK_FALSE((CopyAuthority{2, 7, 9, 0}).Valid());
}

namespace
{
// A queued move of a dead body whose every gate still holds a frame later.
QueuedMoveNow StillGood(const MoveKind aKind = MoveKind::Place)
{
    QueuedMoveNow move{};
    move.Kind = aKind;
    move.AuthorityKnown = true;
    move.SameAuthority = true;
    move.AuthorityFresh = true;
    move.StillRemote = true;
    move.SameCell = true;
    move.Dead = aKind != MoveKind::KnockPlace;
    move.KnockRagdoll = aKind == MoveKind::KnockPlace;
    return move;
}
} // namespace

TEST_CASE("a queued move runs only when every gate still holds for the same copy", "[copybody]")
{
    for (const MoveKind kind : {MoveKind::Place, MoveKind::Settle, MoveKind::KnockPlace})
        CHECK(StillAuthorized(StillGood(kind)));

    // Retired, made again, handed to another owner or a new session, no longer interpolated, run here now, or moved cell or loading.
    for (bool QueuedMoveNow::* pGate :
         {&QueuedMoveNow::AuthorityKnown, &QueuedMoveNow::SameAuthority, &QueuedMoveNow::AuthorityFresh, &QueuedMoveNow::StillRemote, &QueuedMoveNow::SameCell})
    {
        QueuedMoveNow move = StillGood();
        move.*pGate = false;
        CHECK_FALSE(StillAuthorized(move));
    }
    // Into view, picked up, looted, its owner's bones back, dying or bleeding out since.
    for (bool QueuedMoveNow::* pBlock :
         {&QueuedMoveNow::Loading, &QueuedMoveNow::Dying, &QueuedMoveNow::BleedingOut, &QueuedMoveNow::OwnerBonesSince, &QueuedMoveNow::InView, &QueuedMoveNow::HeldHere,
          &QueuedMoveNow::LootOpenNear})
    {
        for (const MoveKind kind : {MoveKind::Place, MoveKind::Settle, MoveKind::KnockPlace})
        {
            QueuedMoveNow move = StillGood(kind);
            move.*pBlock = true;
            CHECK_FALSE(StillAuthorized(move));
        }
    }
}

TEST_CASE("a queued body move needs the body dead, a knock place needs it still lying alive", "[copybody]")
{
    QueuedMoveNow revived = StillGood(MoveKind::Settle);
    revived.Dead = false; // its owner brought it back
    CHECK_FALSE(StillAuthorized(revived));

    QueuedMoveNow up = StillGood(MoveKind::KnockPlace);
    up.KnockRagdoll = false; // got up since
    CHECK_FALSE(StillAuthorized(up));

    QueuedMoveNow died = StillGood(MoveKind::KnockPlace);
    died.Dead = true;
    CHECK_FALSE(StillAuthorized(died));
}

TEST_CASE("the owner's last word is unknown, down or up", "[copybody]")
{
    CHECK(OwnerWordOf(false, cStanding) == OwnerWord::Unknown);
    CHECK(OwnerWordOf(true, cStanding) == OwnerWord::Up);
    CHECK(OwnerWordOf(true, cBleedout) == OwnerWord::Down);
    CHECK(OwnerWordOf(true, cEssentialDown) == OwnerWord::Down);
    CHECK(OwnerWordOf(true, cDead) == OwnerWord::Down);
    CHECK(OwnerWordOf(true, cStanding, true) == OwnerWord::Down); // dead at its owner, whatever the word
    CHECK(OwnerWordOf(false, cStanding, true) == OwnerWord::Down);
}

TEST_CASE("the game's own end of a copy's bleedout plays only once it wrote the copy alive and its owner's NPC is up", "[copybody]")
{
    // The game has written it alive (Actor::SetLifeState), its owner's NPC is up: the stop plays.
    CHECK(LetsEngineStopThrough(true, true, cStanding, OwnerWord::Up));
    CHECK(LetsEngineStopThrough(true, true, Flags(6, 0), OwnerWord::Up));
    // Its owner's NPC still down, or no word yet: held back, and the copy kneels on as its owner's does.
    CHECK_FALSE(LetsEngineStopThrough(true, true, cStanding, OwnerWord::Down));
    CHECK_FALSE(LetsEngineStopThrough(true, true, cStanding, OwnerWord::Unknown));
    // Not yet written alive, or dead: the stop is not the game's recovery.
    CHECK_FALSE(LetsEngineStopThrough(true, true, cBleedout, OwnerWord::Up));
    CHECK_FALSE(LetsEngineStopThrough(true, true, cEssentialDown, OwnerWord::Up));
    CHECK_FALSE(LetsEngineStopThrough(true, true, cDead, OwnerWord::Up));
    CHECK_FALSE(LetsEngineStopThrough(true, true, Flags(1, 0), OwnerWord::Up));
    // Another action, or a player's copy: dropped.
    CHECK_FALSE(LetsEngineStopThrough(true, false, cStanding, OwnerWord::Up));
    CHECK_FALSE(LetsEngineStopThrough(false, true, cStanding, OwnerWord::Up));
}

TEST_CASE("a copy kneeling only in its graph is remembered while it reads alive", "[copybody]")
{
    CHECK(KneelsInGraphOnly(true, true, cStanding));
    CHECK_FALSE(KneelsInGraphOnly(true, true, cBleedout)); // the game's own bleedout: OnBleedingOut sees that already
    CHECK_FALSE(KneelsInGraphOnly(true, true, cDead));
    CHECK_FALSE(KneelsInGraphOnly(true, false, cStanding)); // its graph did not take the kneel
    CHECK_FALSE(KneelsInGraphOnly(false, true, cStanding));
}

TEST_CASE("its owner's stop ends a copy's own bleedout the game's way", "[copybody]")
{
    CHECK(RecoversOnOwnersStop(true, cBleedout, cStanding, false));
    CHECK(RecoversOnOwnersStop(true, cEssentialDown, cStanding, false));
    CHECK(RecoversOnOwnersStop(true, cBleedout, Flags(0, 5), false)); // its owner's word still has the knock of its fall
    // Its own life already alive: the action is replayed as before.
    CHECK_FALSE(RecoversOnOwnersStop(true, cStanding, cStanding, false));
    // Its owner's NPC not up by that word, or dead there: never stood up.
    CHECK_FALSE(RecoversOnOwnersStop(true, cBleedout, cBleedout, false));
    CHECK_FALSE(RecoversOnOwnersStop(true, cBleedout, cStanding, true));
    CHECK_FALSE(RecoversOnOwnersStop(false, cBleedout, cStanding, false));
}

TEST_CASE("an owner's stop from before the copy's current fall is old", "[copybody]")
{
    // The first fall's stop, queued with a replayed history after the second fall's start.
    CHECK(StopBeforeFall(true, 100, 200));
    // The stop of the fall it kneels in, or one with no fall recorded yet: played as before.
    CHECK_FALSE(StopBeforeFall(true, 250, 200));
    CHECK_FALSE(StopBeforeFall(true, 200, 200));
    CHECK_FALSE(StopBeforeFall(true, 100, 0));
    CHECK_FALSE(StopBeforeFall(true, 0, 200));
    // A player's copy: never dropped.
    CHECK_FALSE(StopBeforeFall(false, 100, 200));
}

TEST_CASE("a copy taken out of its fall is stood up the game's way", "[copybody]")
{
    CHECK(RecoverHow(cBleedout, false) == Recovery::SetLifeState);
    CHECK(RecoverHow(cEssentialDown, true) == Recovery::SetLifeState);
    CHECK(RecoverHow(cStanding, true) == Recovery::PerformStop);
    CHECK(RecoverHow(cStanding, false) == Recovery::Events);
    CHECK(RecoverHow(cDead, true) == Recovery::Events);
}

TEST_CASE("a graph kneel ends on two standing readings once it had time to kneel", "[copybody]")
{
    uint32_t high = 0;
    // On its way down it still reads standing: not looked at yet.
    CHECK_FALSE(KneelEnded(high, 100, true, 60.f));
    CHECK_FALSE(KneelEnded(high, 200, true, 60.f));
    CHECK(high == 0);
    // Kneeling.
    CHECK_FALSE(KneelEnded(high, kKneelSettleMs, true, 27.f));
    CHECK_FALSE(KneelEnded(high, kKneelSettleMs + 100, true, 60.f));
    CHECK_FALSE(KneelEnded(high, kKneelSettleMs + 200, true, 30.f));  // one high reading alone is not standing
    CHECK_FALSE(KneelEnded(high, kKneelSettleMs + 300, false, 60.f)); // unreadable: no change
    CHECK_FALSE(KneelEnded(high, kKneelSettleMs + 400, true, 60.f));
    CHECK(KneelEnded(high, kKneelSettleMs + 500, true, 61.f));
    uint32_t old = 0;
    CHECK(KneelEnded(old, kKneelForgetMs, true, 27.f));
}

TEST_CASE("a living body's bones stop 2 s after its root keeps still", "[copybody]")
{
    CHECK(KeepsSendingBones(false, kBodyRestMs - 1, 0));
    CHECK_FALSE(KeepsSendingBones(false, kBodyRestMs, 0));
}

TEST_CASE("a dead body's bones are sent until its limbs keep still too, for a while at most", "[copybody]")
{
    // Its root lay still, its legs still settled: sent on (live-copy-death-pose, a body thrown by Fire Storm).
    CHECK(KeepsSendingBones(true, 3000, 500));
    CHECK(KeepsSendingBones(true, 3000, kBodyRestMs - 1));
    CHECK_FALSE(KeepsSendingBones(true, 3000, kBodyRestMs));
    // A ragdoll that twitches for good stops 6 s after its root came to rest.
    CHECK(KeepsSendingBones(true, kCorpseRestCapMs - 1, 0));
    CHECK_FALSE(KeepsSendingBones(true, kCorpseRestCapMs, 0));
    // Never seen to move (a corpse found lying long after): nothing more.
    CHECK_FALSE(KeepsSendingBones(true, UINT32_MAX, UINT32_MAX));
}

TEST_CASE("a twitch is not a move of a resting body's bones", "[copybody]")
{
    CHECK_FALSE(BonesMoved(0.f));
    CHECK_FALSE(BonesMoved(kBoneRestDegrees - 0.01f));
    CHECK(BonesMoved(kBoneRestDegrees));
    CHECK(BonesMoved(45.f));
    CHECK(BonesMoved(std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("a body in its owner's pose is laid on the ground here, not dropped into it", "[copybody]")
{
    SettleGround ground{};
    ground.Apart = 62.f;
    ground.FastestSpeed = 0.f;
    // Where its owner has the body is its 3D root, which follows its ragdoll's root: laid there, the pose lies where the owner's lay.
    // Its lowest body above the copy's own lowest one now: never lowered, only the clearance.
    ground.LowestHere = -120.f;
    ground.LowestPosed = -117.f;
    CHECK(SettleLift(ground) == Approx(kSettleClearanceUnits));
    ground.LowestPosed = ground.LowestHere + kSettleGroundShiftUnits;
    CHECK(SettleLift(ground) == Approx(kSettleClearanceUnits));
    // Lower than the ground here: raised onto it.
    ground.LowestPosed = -135.f;
    CHECK(SettleLift(ground) == Approx(15.f + kSettleClearanceUnits));
    // Never more than the old drop.
    ground.LowestPosed = ground.LowestHere - kSettleGroundShiftUnits;
    CHECK(SettleLift(ground) == Approx(kSettleDropUnits));
    for (float posed = -170.f; posed <= -70.f; posed += 5.f)
    {
        ground.LowestPosed = posed;
        CHECK(SettleLift(ground) >= kSettleClearanceUnits);
        CHECK(SettleLift(ground) <= kSettleDropUnits);
    }
}

TEST_CASE("the old drop when the ground here is not known", "[copybody]")
{
    SettleGround ground{};
    ground.Apart = 10.f;
    ground.LowestHere = -120.f;
    ground.LowestPosed = -118.f;
    CHECK(SettleLift(ground) == Approx(kSettleClearanceUnits));
    // Carried too far for the ground where it lies to be the ground there.
    SettleGround far = ground;
    far.Apart = kSettleGroundReachUnits + 1.f;
    CHECK(SettleLift(far) == kSettleDropUnits);
    // Still falling or rolling.
    SettleGround moving = ground;
    moving.FastestSpeed = kRestingUnitsPerSecond + 1.f;
    CHECK(SettleLift(moving) == kSettleDropUnits);
    // Fallen through the ground, or the pose read wrong.
    SettleGround deep = ground;
    deep.LowestHere = -400.f;
    CHECK(SettleLift(deep) == kSettleDropUnits);
    SettleGround broken = ground;
    broken.LowestPosed = std::numeric_limits<float>::quiet_NaN();
    CHECK(SettleLift(broken) == kSettleDropUnits);
    broken.LowestPosed = ground.LowestPosed;
    broken.Apart = std::numeric_limits<float>::quiet_NaN();
    CHECK(SettleLift(broken) == kSettleDropUnits);
}

TEST_CASE("a settling body takes its targets from its owner's pose, and from its own bodies only when no pose was read", "[copybody]")
{
    // The owner's pose, lifted by the settle; no pose sent or one that could not be read: the copy's own bodies, moved.
    CHECK(TargetSourceOf(true) == TargetSource::OwnerPoseSettled);
    CHECK(TargetSourceOf(false) == TargetSource::CurrentBodies);
}

TEST_CASE("a blend step may run in view, and every other gate still holds it back", "[copybody]")
{
    for (const MoveKind kind : {MoveKind::Place, MoveKind::Settle, MoveKind::KnockPlace})
    {
        QueuedMoveNow seen = StillGood(kind);
        seen.InView = true;
        CHECK_FALSE(StillAuthorized(seen));
        seen.Blended = true;
        CHECK(StillAuthorized(seen));

        for (bool QueuedMoveNow::* pBlock :
             {&QueuedMoveNow::Loading, &QueuedMoveNow::Dying, &QueuedMoveNow::BleedingOut, &QueuedMoveNow::OwnerBonesSince, &QueuedMoveNow::HeldHere, &QueuedMoveNow::LootOpenNear})
        {
            QueuedMoveNow move = seen;
            move.*pBlock = true;
            CHECK_FALSE(StillAuthorized(move));
        }
        QueuedMoveNow other = seen;
        other.SameAuthority = false;
        CHECK_FALSE(StillAuthorized(other));
    }
}

TEST_CASE("a blend takes more steps for a longer way, within its bounds", "[copybody]")
{
    // A settle, or a way too short to count, still blends over the fewest steps.
    CHECK(BlendSteps(0.f) == kMinBlendSteps);
    CHECK(BlendSteps(-5.f) == kMinBlendSteps);
    CHECK(BlendSteps(std::numeric_limits<float>::quiet_NaN()) == kMinBlendSteps);
    CHECK(BlendSteps(kApartUnits) == kMinBlendSteps + 1);
    CHECK(BlendSteps(kBlendUnitsPerStep * 30.f) == 30);
    CHECK(BlendSteps(kBlendUnitsPerStep * 30.5f) == 31);
    // A far one slides faster rather than for longer.
    CHECK(BlendSteps(100000.f) == kMaxBlendSteps);
    CHECK(BlendSteps(std::numeric_limits<float>::infinity()) == kMaxBlendSteps);
}

TEST_CASE("a blend moves a body at an even pace and lands it exactly on its last step", "[copybody]")
{
    for (const float cApart : {0.f, 10.f, kApartUnits + 1.f, 300.f, 5000.f})
    {
        const uint32_t cSteps = BlendSteps(cApart);
        float at = 0.f;
        float lastStep = -1.f;
        for (uint32_t step = 0; step < cSteps; ++step)
        {
            const float cFraction = BlendFraction(step, cSteps);
            REQUIRE(cFraction > 0.f);
            REQUIRE(cFraction <= 1.f);
            const float cNext = at + (cApart - at) * cFraction;
            if (lastStep >= 0.f)
                CHECK(cNext - at == Approx(lastStep).margin(1e-2f));
            lastStep = cNext - at;
            at = cNext;
            // Only the last step is a whole move, which ends the blend.
            CHECK((cFraction == 1.f) == (step + 1 == cSteps));
        }
        CHECK(at == Approx(cApart).margin(1e-3f));
        CHECK(lastStep <= std::max(kBlendUnitsPerStep, cApart / static_cast<float>(kMaxBlendSteps)) + 1e-3f);
    }
    // A step past the end is a whole move.
    CHECK(BlendFraction(kMinBlendSteps + 3, kMinBlendSteps) == 1.f);
    CHECK(BlendFraction(0, 0) == 1.f);
}
