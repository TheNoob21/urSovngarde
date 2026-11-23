#include <catch2/catch.hpp>

#include <GhostDeathPolicy.h>

#include <cstdint>

// The owner's fox is killed on its copy here, over this game's own hare; later the copy goes.

TEST_CASE("a reference whose stand-in died comes back as a body", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::OnStandInGone(true, false) == GhostDeathPolicy::Action::LayDown);
}

TEST_CASE("a reference whose stand-in is alive comes back as it was", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::OnStandInGone(false, false) == GhostDeathPolicy::Action::Show);
}

TEST_CASE("another player's follower is never killed here, dead copy or not", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::OnStandInGone(true, true) == GhostDeathPolicy::Action::Show);
    REQUIRE(GhostDeathPolicy::OnStandInGone(false, true) == GhostDeathPolicy::Action::Show);
}

// The copy goes: whether it was a body is read from it while the game has it, else from the game that ran its character.

TEST_CASE("a copy the game still has says itself whether it died", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::CopyWasDead(false, true, false));
    REQUIRE_FALSE(GhostDeathPolicy::CopyWasDead(false, false, true));
}

TEST_CASE("a copy freed or marked deleted is a body by the word of the game that ran it", "[ghostdeath]")
{
    // The owner's word for a copy shown here, this game's own for a copy it took on.
    REQUIRE(GhostDeathPolicy::CopyWasDead(true, false, true));
    REQUIRE_FALSE(GhostDeathPolicy::CopyWasDead(true, false, false));
    // What a copy marked deleted still reads as does not count; the word decides.
    REQUIRE_FALSE(GhostDeathPolicy::CopyWasDead(true, true, false));
}

TEST_CASE("a copy this game ran that died here and was freed lays its reference down", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::OnStandInGone(GhostDeathPolicy::CopyWasDead(true, false, true), false) == GhostDeathPolicy::Action::LayDown);
    // Not another player's follower, even one this game ran.
    REQUIRE(GhostDeathPolicy::OnStandInGone(GhostDeathPolicy::CopyWasDead(true, false, true), true) == GhostDeathPolicy::Action::Show);
}

// The owner's fox was run here and died here, and is handed on before the server took the death.

TEST_CASE("one killed here before its hand-off is a body until its new owner's word comes", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::KilledBeforeHandOff(true, false, false));
    // Alive here, or dead in the grant already: nothing to carry over.
    REQUIRE_FALSE(GhostDeathPolicy::KilledBeforeHandOff(false, false, false));
    REQUIRE_FALSE(GhostDeathPolicy::KilledBeforeHandOff(true, true, false));
}

TEST_CASE("a protected or essential one killed here is never a body for its hand-off: it lives on at its new owner", "[ghostdeath]")
{
    REQUIRE_FALSE(GhostDeathPolicy::KilledBeforeHandOff(true, false, true));
    REQUIRE_FALSE(GhostDeathPolicy::KilledBeforeHandOff(false, false, true));
}

// The hare is queued to be laid down; each frame decides what happens to it.

namespace
{
GhostDeathPolicy::Waiting LoadedHare() noexcept
{
    GhostDeathPolicy::Waiting waiting{};
    waiting.InMemory = true;
    waiting.Has3D = true;
    return waiting;
}
} // namespace

TEST_CASE("a loaded, enabled hare with its 3D is killed", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(LoadedHare()) == GhostDeathPolicy::Step::Kill);
}

TEST_CASE("a hare without its 3D waits for it", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Has3D = false;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Wait);
}

TEST_CASE("a hare not in memory waits until its cell may have reset", "[ghostdeath]")
{
    GhostDeathPolicy::Waiting waiting{};
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Wait);
    waiting.Expired = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::GiveUp);
}

TEST_CASE("a hare waiting until its cell may have reset is given up on, not killed", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Expired = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::GiveUp);
}

TEST_CASE("a hare already dead is announced, however long it waited", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Dead = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
    waiting.Expired = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
}

TEST_CASE("a hare the game is disposing of is forgotten, whatever else is set", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Deleted = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Forget);
    waiting.Dead = true;
    waiting.Expired = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Forget);
}

TEST_CASE("a hare hidden again or bound to a character since is left to that", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.TakenOver = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Forget);
    waiting.Expired = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Forget);
    waiting.Dead = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
}

TEST_CASE("a hare whose enable has not landed waits", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.HiddenHere = true;
    waiting.Disabled = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Wait);
}

TEST_CASE("a hare disabled within 5 s of being queued waits; after that its own save keeps it so", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Disabled = true;
    waiting.EnableMayLand = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Wait);
    waiting.EnableMayLand = false;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
}

TEST_CASE("a unique one is announced as it is, with its 3D or not", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.Unique = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
    waiting.Has3D = false;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
}

TEST_CASE("one whose life follows its owner is announced as it is", "[ghostdeath]")
{
    auto waiting = LoadedHare();
    waiting.LifeFollowsOwner = true;
    REQUIRE(GhostDeathPolicy::OnWaitingFrame(waiting) == GhostDeathPolicy::Step::Announce);
}

TEST_CASE("only a loaded, enabled hare with its 3D and nothing else in the way is killed", "[ghostdeath]")
{
    // Every combination of what is known: a kill only ever comes from the one input with nothing in the way,
    // so a reference not in memory, or one that waited too long, is never killed.
    constexpr uint32_t cFields = 11;
    uint32_t kills = 0;
    for (uint32_t bits = 0; bits < (1u << cFields); ++bits)
    {
        GhostDeathPolicy::Waiting waiting{};
        waiting.InMemory = bits & (1u << 0);
        waiting.Deleted = bits & (1u << 1);
        waiting.Dead = bits & (1u << 2);
        waiting.TakenOver = bits & (1u << 3);
        waiting.Expired = bits & (1u << 4);
        waiting.HiddenHere = bits & (1u << 5);
        waiting.Disabled = bits & (1u << 6);
        waiting.EnableMayLand = bits & (1u << 7);
        waiting.Unique = bits & (1u << 8);
        waiting.LifeFollowsOwner = bits & (1u << 9);
        waiting.Has3D = bits & (1u << 10);

        if (GhostDeathPolicy::OnWaitingFrame(waiting) != GhostDeathPolicy::Step::Kill)
            continue;
        ++kills;
        REQUIRE(waiting.InMemory);
        REQUIRE(waiting.Has3D);
        REQUIRE_FALSE(waiting.Deleted);
        REQUIRE_FALSE(waiting.Dead);
        REQUIRE_FALSE(waiting.TakenOver);
        REQUIRE_FALSE(waiting.Expired);
        REQUIRE_FALSE(waiting.HiddenHere);
        REQUIRE_FALSE(waiting.Disabled);
        REQUIRE_FALSE(waiting.Unique);
        REQUIRE_FALSE(waiting.LifeFollowsOwner);
    }
    // EnableMayLand alone does not stop a kill: it matters only for a disabled one.
    REQUIRE(kills == 2);
}

TEST_CASE("the wait ends a day before a cell's respawn time, and game time set back counts as none passed", "[ghostdeath]")
{
    REQUIRE(GhostDeathPolicy::kMaxWaitDays < GhostDeathPolicy::kCellRespawnDays);
    REQUIRE_FALSE(GhostDeathPolicy::HasExpired(100.f, 100.f));
    REQUIRE_FALSE(GhostDeathPolicy::HasExpired(100.f, 100.f + GhostDeathPolicy::kMaxWaitDays - 0.5f));
    REQUIRE(GhostDeathPolicy::HasExpired(100.f, 100.f + GhostDeathPolicy::kMaxWaitDays));
    REQUIRE_FALSE(GhostDeathPolicy::HasExpired(100.f, 50.f));
}
