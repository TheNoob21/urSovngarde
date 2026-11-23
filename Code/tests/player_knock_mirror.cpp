#include <catch2/catch.hpp>

#include <Systems/PlayerKnockMirror.h>

#include <string_view>

// Another player's copy goes down and gets up as its owner does; a knock a hit here leaves on it while its owner stands is taken off.

namespace
{
using namespace PlayerKnockMirror;
using namespace std::chrono_literals;

constexpr uint32_t Knock(const uint32_t aState) noexcept
{
    return aState << 25;
}

constexpr uint32_t Life(const uint32_t aState) noexcept
{
    return aState << 21;
}
} // namespace

TEST_CASE("Player knock mirror: what an owner's action says", "[player_knock_mirror]")
{
    CHECK(WordOf(true, false, 0) == Word::Down);
    CHECK(WordOf(false, true, 0) == Word::GetUp);
    CHECK(WordOf(false, false, Knock(3)) == Word::Down);
    CHECK(WordOf(false, false, Knock(5)) == Word::Down);
    CHECK(WordOf(false, false, Knock(6)) == Word::GetUp);
    CHECK(WordOf(false, false, 0) == Word::Stands);
    // Other bits (sitting, weapon drawn) say nothing of a fall.
    CHECK(WordOf(false, false, 0x1u << 14) == Word::Stands);

    // Dying, dead, or downed for a revive: not a fall this mirrors.
    CHECK(WordOf(true, false, Life(1)) == Word::None);
    CHECK(WordOf(true, false, Life(2)) == Word::None);
    CHECK(WordOf(false, false, Life(8) | Knock(3)) == Word::None);
    CHECK(WordOf(false, true, Life(7)) == Word::None);
}

TEST_CASE("Player knock mirror: a hit here on a copy whose owner stands never knocks it down", "[player_knock_mirror]")
{
    Mirror mirror;
    const auto cNow = Clock::now();
    CHECK(mirror.Frame(0, true, cNow) == Act::None);
    CHECK(mirror.Frame(3, true, cNow) == Act::StandNow);
    CHECK(mirror.Frame(5, true, cNow) == Act::StandNow);
    CHECK_FALSE(mirror.RunsProcess(3));
    // Its owner's actions while standing change nothing.
    mirror.OnOwnerWord(Word::Stands, cNow);
    mirror.OnOwnerWord(Word::GetUp, cNow);
    CHECK(mirror.GetPhase() == Mirror::Phase::Standing);
    CHECK(mirror.Frame(3, true, cNow) == Act::StandNow);
}

TEST_CASE("Player knock mirror: down with its owner, held, and up with its owner", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();

    mirror.OnOwnerWord(Word::Down, now);
    CHECK(mirror.GetPhase() == Mirror::Phase::Down);
    // Knocked the game's way, once.
    CHECK(mirror.Frame(0, true, now) == Act::Knock);

    // Falling: its process runs; it lies where it is.
    now += 100ms;
    CHECK(mirror.Frame(3, true, now) == Act::Hold);
    CHECK(mirror.RunsProcess(3));

    // At rest with its get-up queued, while its owner lies: held, and its process waits so the get-up does not start.
    now += 1s;
    CHECK(mirror.Frame(kKnockQueued, true, now) == Act::Hold);
    CHECK_FALSE(mirror.RunsProcess(kKnockQueued));

    // A standing word that came with the fall itself is not a get-up.
    mirror.OnOwnerWord(Word::Stands, now - 1s + 100ms);
    CHECK(mirror.GetPhase() == Mirror::Phase::Down);

    // The owner gets up: its own get-up runs.
    now += 2s;
    mirror.OnOwnerWord(Word::GetUp, now);
    CHECK(mirror.GetPhase() == Mirror::Phase::GettingUp);
    CHECK(mirror.Frame(kKnockGettingUp, true, now) == Act::LetUp);
    CHECK(mirror.RunsProcess(kKnockGettingUp));
    CHECK(mirror.RunsProcess(kKnockQueued));

    // On its feet: it walks back to its owner, once.
    now += 1s;
    CHECK(mirror.Frame(0, true, now) == Act::Risen);
    CHECK(mirror.GetPhase() == Mirror::Phase::Standing);
    CHECK(mirror.Frame(0, true, now) == Act::None);
    CHECK_FALSE(mirror.RunsProcess(0));
    // And the old rule is back.
    CHECK(mirror.Frame(3, true, now) == Act::StandNow);
}

TEST_CASE("Player knock mirror: a later action with its owner standing counts as the get-up", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();
    mirror.OnOwnerWord(Word::Down, now);
    CHECK(mirror.Frame(0, true, now) == Act::Knock);
    now += kMinDown;
    mirror.OnOwnerWord(Word::Stands, now);
    CHECK(mirror.GetPhase() == Mirror::Phase::GettingUp);
}

TEST_CASE("Player knock mirror: a get-up that waits queued is nudged, then stood up", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();
    mirror.OnOwnerWord(Word::Down, now);
    CHECK(mirror.Frame(0, true, now) == Act::Knock);
    CHECK(mirror.Frame(kKnockQueued, true, now) == Act::Hold);
    now += 1s;
    mirror.OnOwnerWord(Word::GetUp, now);

    uint32_t nudges = 0;
    for (int i = 0; i < 120; ++i)
    {
        const Act cAct = mirror.Frame(kKnockQueued, true, now);
        if (cAct == Act::NudgeGetUp)
            ++nudges;
        if (cAct == Act::StandNow)
            break;
        CHECK((cAct == Act::LetUp || cAct == Act::NudgeGetUp));
        now += 100ms;
    }
    CHECK(nudges == kMaxNudges);
    CHECK(mirror.GetPhase() == Mirror::Phase::Standing);
}

TEST_CASE("Player knock mirror: a lost get-up word does not leave the copy down for good", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();
    mirror.OnOwnerWord(Word::Down, now);
    CHECK(mirror.Frame(0, true, now) == Act::Knock);
    CHECK(mirror.Frame(kKnockQueued, true, now + kMaxDown) == Act::Hold);
    // Past the longest hold: its get-up runs as if its owner got up.
    CHECK(mirror.Frame(kKnockQueued, true, now + kMaxDown + 1ms) == Act::LetUp);
    CHECK(mirror.GetPhase() == Mirror::Phase::GettingUp);
}

TEST_CASE("Player knock mirror: a copy that cannot fall now is not knocked, and a refused knock is tried again once", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();
    mirror.OnOwnerWord(Word::Down, now);
    // Seated or riding: left as it is, following its owner.
    CHECK(mirror.Frame(0, false, now) == Act::None);
    CHECK(mirror.Frame(0, true, now) == Act::Knock);
    // The knock did not take: retried after a pause, and only kMaxKnocks times in all.
    CHECK(mirror.Frame(0, true, now + 500ms) == Act::None);
    CHECK(mirror.Frame(0, true, now + kKnockRetry) == Act::Knock);
    CHECK(mirror.Frame(0, true, now + 3 * kKnockRetry) == Act::None);
    // Its owner gets up while it never went down: it was asked to fall, so its rise closes whatever gap it has.
    mirror.OnOwnerWord(Word::GetUp, now + 4s);
    CHECK(mirror.Frame(0, true, now + 4s) == Act::Risen);
}

TEST_CASE("Player knock mirror: a fall again while getting up holds it down again", "[player_knock_mirror]")
{
    Mirror mirror;
    auto now = Clock::now();
    mirror.OnOwnerWord(Word::Down, now);
    CHECK(mirror.Frame(0, true, now) == Act::Knock);
    now += 2s;
    mirror.OnOwnerWord(Word::GetUp, now);
    CHECK(mirror.Frame(kKnockGettingUp, true, now) == Act::LetUp);
    // Knocked over again over there before it stood.
    mirror.OnOwnerWord(Word::Down, now + 200ms);
    CHECK(mirror.GetPhase() == Mirror::Phase::Down);
    CHECK(mirror.Frame(3, true, now + 300ms) == Act::Hold);
    CHECK(mirror.Frame(kKnockQueued, true, now + 2s) == Act::Hold);
    CHECK_FALSE(mirror.RunsProcess(kKnockQueued));
}

namespace
{
//! The copy here, as AnimationSystem and InterpolationSystem read it.
struct CopyHere
{
    bool Dead{};
    bool DyingOrDown{};
    bool OnFoot{true};
    bool Pinned{};
    uint32_t Knock{};

    [[nodiscard]] bool MayFall() const noexcept { return PlayerKnockMirror::MayFall(Dead, DyingOrDown, OnFoot, Pinned); }
};

//! One owner action reaching a VR player copy, in AnimationSystem's order: its word goes to the mirror unless it is history
//! (NotePlayerOwnerAction), then a "Ragdoll" plays only if the mirror admits it (AdmitPlayerRagdoll).
bool VrReplays(Mirror& aMirror, const char* acpEvent, const uint32_t aOwnerFlags1, const bool aHistory, const CopyHere& acCopy, const Clock::time_point aNow)
{
    const bool cRagdoll = std::string_view(acpEvent) == "Ragdoll";
    const bool cGetUp = std::string_view(acpEvent) == "GetUpBegin";
    if (!aHistory)
        aMirror.OnOwnerWord(WordOf(cRagdoll, cGetUp, aOwnerFlags1), aNow);
    return cRagdoll && aMirror.AdmitRagdoll(WordOf(cRagdoll, cGetUp, aOwnerFlags1), aHistory, acCopy.Knock, acCopy.MayFall(), aNow);
}
} // namespace

TEST_CASE("Player knock mirror: vr plays the owner's Ragdoll only when a flat frame would knock", "[player_knock_mirror]")
{
    const auto cNow = Clock::now();

    SECTION("an alive owner's live fall on a copy on its feet is played, once")
    {
        Mirror mirror;
        CopyHere copy;
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, copy, cNow));
        // Taken: it lies while its owner does. A second Ragdoll is not played over it.
        copy.Knock = 3;
        CHECK(mirror.Frame(copy.Knock, copy.MayFall(), cNow + 50ms, false) == Act::Hold);
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, copy, cNow + 2s));
    }
    SECTION("a dying, dead or bleedout-downed owner's Ragdoll is not")
    {
        for (const uint32_t cLife : {1u, 2u, 3u, 7u, 8u})
        {
            Mirror mirror;
            CHECK_FALSE(VrReplays(mirror, "Ragdoll", Life(cLife), false, CopyHere{}, cNow));
            CHECK(mirror.GetPhase() == Mirror::Phase::Standing);
        }
    }
    SECTION("a copy seated or riding, dying or bleeding out here, or dead here is not knocked")
    {
        CopyHere pinned;
        pinned.Pinned = true;
        CopyHere seated;
        seated.OnFoot = false;
        CopyHere bleeding;
        bleeding.DyingOrDown = true;
        CopyHere dead;
        dead.Dead = true;
        for (const CopyHere& cCopy : {pinned, seated, bleeding, dead})
        {
            Mirror mirror;
            CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, cCopy, cNow));
            // The owner is still down for the mirror: once it may fall, the next Ragdoll is played.
            CHECK(mirror.GetPhase() == Mirror::Phase::Down);
            CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + 10ms));
        }
    }
    SECTION("the history a copy is spawned with is never played")
    {
        Mirror mirror;
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, true, CopyHere{}, cNow));
        CHECK(mirror.GetPhase() == Mirror::Phase::Standing);
        // Not even while a live fall has the mirror down.
        mirror.OnOwnerWord(Word::Down, cNow);
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, true, CopyHere{}, cNow));
    }
    SECTION("tries are shared with the knock limit")
    {
        Mirror mirror;
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow));
        // Not taken by the graph: tried again only after the pause, and kMaxKnocks times in all.
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + 500ms));
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + kKnockRetry));
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + 5 * kKnockRetry));
    }
}

TEST_CASE("Player knock mirror: vr and flat take the same knock decision", "[player_knock_mirror]")
{
    const auto cNow = Clock::now();
    const uint32_t cOwnerFlags[] = {0, Knock(3), Knock(6), Life(1), Life(8) | Knock(3)};
    for (const uint32_t cFlags : cOwnerFlags)
        for (int copyCase = 0; copyCase < 6; ++copyCase)
        {
            CopyHere copy;
            copy.Pinned = copyCase == 1;
            copy.OnFoot = copyCase != 2;
            copy.DyingOrDown = copyCase == 3;
            copy.Knock = copyCase == 4 ? 3u : 0u;
            copy.Dead = copyCase == 5;

            // Flat: the owner's word, then the frame's knock.
            Mirror flat;
            flat.OnOwnerWord(WordOf(true, false, cFlags), cNow);
            const bool cFlatKnocks = flat.Frame(copy.Knock, copy.MayFall(), cNow) == Act::Knock;

            // VR: the same word arrives as the owner's Ragdoll.
            Mirror vr;
            const bool cVrPlays = VrReplays(vr, "Ragdoll", cFlags, false, copy, cNow);

            INFO("owner flags " << cFlags << ", copy case " << copyCase);
            CHECK(cFlatKnocks == cVrPlays);
        }
}

TEST_CASE("Player knock mirror: a vr frame never asks for a knock it cannot make", "[player_knock_mirror]")
{
    // A fall told only by the owner's knock bits: VR has no safe knock of its own for a player copy, so it waits for a Ragdoll.
    Mirror mirror;
    const auto cNow = Clock::now();
    CHECK_FALSE(VrReplays(mirror, "attackStart", Knock(3), false, CopyHere{}, cNow));
    CHECK(mirror.GetPhase() == Mirror::Phase::Down);
    CHECK(mirror.Frame(0, true, cNow, false) == Act::None);
    CHECK(mirror.Frame(0, true, cNow + 3 * kKnockRetry, false) == Act::None);
    // No try was used up: the owner's Ragdoll is still played.
    CHECK(VrReplays(mirror, "Ragdoll", Knock(3), false, CopyHere{}, cNow + 3 * kKnockRetry));
}

TEST_CASE("Player knock mirror: an earlier fall does not let a dying owner's Ragdoll play", "[player_knock_mirror]")
{
    const auto cNow = Clock::now();

    SECTION("down by its knock bits, then a dying or downed owner's Ragdoll, with no try used")
    {
        for (const uint32_t cLife : {1u, 2u, 3u, 7u, 8u})
        {
            Mirror mirror;
            // An alive owner's fall told by its knock bits: the mirror is down and no try is used on VR.
            CHECK_FALSE(VrReplays(mirror, "attackStart", Knock(3), false, CopyHere{}, cNow));
            CHECK(mirror.GetPhase() == Mirror::Phase::Down);
            // The owner then bleeds out (or dies) and its game sends a Ragdoll: it says nothing, and the old Down stays.
            INFO("owner life " << cLife);
            CHECK_FALSE(VrReplays(mirror, "Ragdoll", Life(cLife) | Knock(3), false, CopyHere{}, cNow + 100ms));
            CHECK(mirror.GetPhase() == Mirror::Phase::Down);
            // The try is still there for an alive owner's own Ragdoll.
            CHECK(VrReplays(mirror, "Ragdoll", Knock(3), false, CopyHere{}, cNow + 200ms));
        }
    }
    SECTION("an earlier Ragdoll the graph did not take, then a dying owner's Ragdoll once the pause is over")
    {
        Mirror mirror;
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow));
        // The copy did not go down (its knock state stays 0); the next try is due after kKnockRetry.
        CHECK(mirror.Frame(0, true, cNow + 50ms, false) == Act::None);
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", Life(3), false, CopyHere{}, cNow + kKnockRetry + 10ms));
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", Life(1), false, CopyHere{}, cNow + 2 * kKnockRetry));
        // No try was used: an alive owner's Ragdoll still gets the second one.
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + 2 * kKnockRetry + 10ms));
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow + 4 * kKnockRetry));
    }
}

TEST_CASE("Player knock mirror: vr sends its graph a Ragdoll for a fall told only by knock bits", "[player_knock_mirror]")
{
    const auto cNow = Clock::now();

    SECTION("once per fall, after the wait for its owner's Ragdoll")
    {
        Mirror mirror;
        CHECK_FALSE(VrReplays(mirror, "attackStart", Knock(3), false, CopyHere{}, cNow));
        CHECK_FALSE(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait - 1ms));
        CHECK(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
        // Not taken by the graph either way: no second graph knock and no Ragdoll replay on top of it.
        CHECK_FALSE(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait + 2 * kKnockRetry));
        CHECK(VrReplays(mirror, "Ragdoll", Knock(3), false, CopyHere{}, cNow + kGraphKnockWait + 2 * kKnockRetry));
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", Knock(3), false, CopyHere{}, cNow + kGraphKnockWait + 5 * kKnockRetry));
    }
    SECTION("not when its owner's Ragdoll was played")
    {
        Mirror mirror;
        CHECK(VrReplays(mirror, "Ragdoll", 0, false, CopyHere{}, cNow));
        CHECK_FALSE(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait + kKnockRetry));
    }
    SECTION("a Ragdoll refused for a seated copy leaves the graph knock for when it may fall")
    {
        Mirror mirror;
        CopyHere pinned;
        pinned.Pinned = true;
        CHECK_FALSE(VrReplays(mirror, "Ragdoll", 0, false, pinned, cNow));
        CHECK_FALSE(mirror.TakeGraphKnock(0, pinned.MayFall(), cNow + kGraphKnockWait));
        CHECK(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait + 10ms));
    }
    SECTION("not for a copy that cannot fall, is knocked already, or whose owner stands")
    {
        for (int copyCase = 0; copyCase < 5; ++copyCase)
        {
            CopyHere copy;
            copy.Pinned = copyCase == 0;
            copy.OnFoot = copyCase != 1;
            copy.DyingOrDown = copyCase == 2;
            copy.Knock = copyCase == 3 ? 3u : 0u;
            copy.Dead = copyCase == 4;
            Mirror mirror;
            mirror.OnOwnerWord(WordOf(false, false, Knock(3)), cNow);
            INFO("copy case " << copyCase);
            CHECK_FALSE(mirror.TakeGraphKnock(copy.Knock, copy.MayFall(), cNow + kGraphKnockWait));
        }
        Mirror standing;
        standing.OnOwnerWord(WordOf(false, false, 0), cNow);
        CHECK_FALSE(standing.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
    }
    SECTION("not once its owner's latest word is a dying or downed one, nor from history")
    {
        for (const uint32_t cLife : {1u, 2u, 3u, 7u, 8u})
        {
            Mirror mirror;
            mirror.OnOwnerWord(WordOf(false, false, Knock(3)), cNow);
            mirror.OnOwnerWord(WordOf(false, false, Life(cLife) | Knock(3)), cNow + 100ms);
            INFO("owner life " << cLife);
            CHECK(mirror.GetPhase() == Mirror::Phase::Down);
            CHECK_FALSE(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
            // Alive again and still down: the graph knock is due.
            mirror.OnOwnerWord(WordOf(false, false, Knock(5)), cNow + kGraphKnockWait);
            CHECK(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
        }
        Mirror history;
        CHECK_FALSE(VrReplays(history, "attackStart", Knock(3), true, CopyHere{}, cNow));
        CHECK_FALSE(history.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
    }
    SECTION("its owner got up before the wait was over")
    {
        Mirror mirror;
        mirror.OnOwnerWord(WordOf(false, false, Knock(3)), cNow);
        mirror.OnOwnerWord(WordOf(false, true, 0), cNow + 100ms);
        CHECK_FALSE(mirror.TakeGraphKnock(0, true, cNow + kGraphKnockWait));
    }
}
