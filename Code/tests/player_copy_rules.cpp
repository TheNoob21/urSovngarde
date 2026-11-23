#include <catch2/catch.hpp>

#include <PlayerCopyRules.h>

TEST_CASE("A new copy of a player retires the older one", "[player_copy]")
{
    using PlayerCopyRules::Older;
    using PlayerCopyRules::OnPlayerCopyMade;

    // The first copy of this player, or the same actor made again: nothing goes.
    CHECK(OnPlayerCopyMade(0, 0xFF000EF1, false, false) == Older::kKeep);
    CHECK(OnPlayerCopyMade(0xFF000EF1, 0xFF000EF1, true, true) == Older::kKeep);
    // The older one is gone already, or is no longer a player copy of ours.
    CHECK(OnPlayerCopyMade(0xFF000D0D, 0xFF000EF1, false, false) == Older::kKeep);
    // Still the server's character here under another server id: removed as the server's removal would.
    CHECK(OnPlayerCopyMade(0xFF000D0D, 0xFF000EF1, true, true) == Older::kRemoveSeen);
    // Left standing with no server character: deleted.
    CHECK(OnPlayerCopyMade(0xFF000D0D, 0xFF000EF1, true, false) == Older::kDelete);
}

TEST_CASE("Only the remembered actor itself is retired, not another that came to have its form id", "[player_copy]")
{
    using PlayerCopyRules::Found;
    using PlayerCopyRules::Older;
    using PlayerCopyRules::OnRememberedCopy;
    using PlayerCopyRules::Record;

    // Player 2's copy FF000D0D, handle 0x1D0D, made for server character 0x41.
    constexpr Record cOld{2, 0xFF000D0D, 0x1D0D, 0x41};

    // Still that actor, left standing as an untracked player copy of ours: deleted when player 2's next copy comes.
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000EF1, Found{0x1D0D, true, false}) == Older::kDelete);
    // Still tracked by an entity: the tracked branch removes it, not this one.
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000EF1, Found{0x1D0D, true, true}) == Older::kRemoveSeen);

    // Form id reuse: FF000D0D went, and the game gave it to player 3's orphan copy (another handle). It is another player's
    // remote player copy of ours, untracked, temporary: it stays.
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000EF1, Found{0x2D0D, true, false}) == Older::kKeep);
    // Nothing on that id any more, or not one of our player copies.
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000EF1, Found{0, false, false}) == Older::kKeep);
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000EF1, Found{0x1D0D, false, false}) == Older::kKeep);
    // A record of another player, or one without a handle, retires nothing.
    CHECK(OnRememberedCopy(cOld, 3, 0xFF000EF1, Found{0x1D0D, true, false}) == Older::kKeep);
    CHECK(OnRememberedCopy(Record{2, 0xFF000D0D, 0, 0x41}, 2, 0xFF000EF1, Found{0, true, false}) == Older::kKeep);
    // The same actor made again.
    CHECK(OnRememberedCopy(cOld, 2, 0xFF000D0D, Found{0x1D0D, true, false}) == Older::kKeep);
}

TEST_CASE("A player copy's record goes with that actor, and a replacement is judged by its own", "[player_copy]")
{
    using PlayerCopyRules::Found;
    using PlayerCopyRules::Older;
    using PlayerCopyRules::OnRememberedCopy;
    using PlayerCopyRules::Record;
    using PlayerCopyRules::Remembered;

    Remembered records;

    // Player 2's first copy: nothing before it.
    CHECK_FALSE(records.Remember({2, 0xFF000D0D, 0x1D0D, 0x41}).has_value());

    // That copy is deleted here (the server removed it): its record goes with it.
    records.Forget(0xFF000D0D, 0x1D0D);
    CHECK_FALSE(records.Of(2).has_value());

    // The game gives FF000D0D to player 3's copy; then player 2 comes back as a new copy. No record of player 2: nothing goes,
    // and player 3's copy on the reused id is untouched.
    CHECK_FALSE(records.Remember({3, 0xFF000D0D, 0x2D0D, 0x52}).has_value());
    CHECK_FALSE(records.Remember({2, 0xFF000EF1, 0x1EF1, 0x43}).has_value());
    // Player 3's record is still its own.
    REQUIRE(records.Of(3).has_value());
    CHECK(records.Of(3)->Handle == 0x2D0D);

    // A deletion of another actor on a remembered id (another handle) leaves the record alone.
    records.Forget(0xFF000D0D, 0x9999);
    CHECK(records.Of(3).has_value());

    // Player 2 respawns as server character 0x44 while its copy FF000EF1 stands untracked: the older record is handed back, and that
    // same actor (by handle) is retired; its server id names the ride to take it off first.
    const auto cBefore = records.Remember({2, 0xFF000F00, 0x1F00, 0x44});
    REQUIRE(cBefore.has_value());
    CHECK(cBefore->ServerId == 0x43);
    CHECK(OnRememberedCopy(*cBefore, 2, 0xFF000F00, Found{0x1EF1, true, false}) == Older::kDelete);

    // Had FF000EF1 been freed meanwhile and given to another of player 2's copies (another handle), that one is not the remembered
    // actor.
    CHECK(OnRememberedCopy(*cBefore, 2, 0xFF000F00, Found{0x3EF1, true, false}) == Older::kKeep);

    // An actor gone already is forgotten by its form id alone; the disconnect forgets every record.
    records.Forget(0xFF000F00, 0);
    CHECK_FALSE(records.Of(2).has_value());
    records.Clear();
    CHECK_FALSE(records.Of(3).has_value());
}

TEST_CASE("A stale duplicate spawn of a player that arrives last is never taken for the newest", "[player_copy]")
{
    using PlayerCopyRules::OnPlayerSpawn;
    using PlayerCopyRules::Remembered;
    using PlayerCopyRules::Spawn;

    Remembered records;
    const auto cArrive = [&records](const uint32_t aPlayerId, const uint32_t aGeneration)
    {
        const Spawn cSpawn = OnPlayerSpawn(records.NewestGeneration(aPlayerId), aGeneration);
        if (cSpawn == Spawn::kNewest)
            records.SawGeneration(aPlayerId, aGeneration);
        return cSpawn;
    };

    // Player 2 reloads: the server makes character 6 while 5 is still about. A cell crossing then sends both, 6 first.
    CHECK(cArrive(2, 5) == Spawn::kNewest);
    CHECK(cArrive(2, 6) == Spawn::kNewest);
    CHECK(cArrive(2, 5) == Spawn::kStale);
    // The newest itself sent again is still the newest.
    CHECK(cArrive(2, 6) == Spawn::kNewest);
    CHECK(records.NewestGeneration(2) == 6);

    // Another player's characters are counted on their own, and the counter is shared: 3 is older than 6 but player 7's first.
    CHECK(cArrive(7, 3) == Spawn::kNewest);

    // The newest copy goes here (out of range): the stale one sent later still makes no copy.
    records.Remember({2, 0xFF000EF1, 0x1EF1, 0x46});
    records.Forget(0xFF000EF1, 0x1EF1);
    CHECK_FALSE(records.Of(2).has_value());
    CHECK(cArrive(2, 5) == Spawn::kStale);
    CHECK(cArrive(2, 8) == Spawn::kNewest);

    // An older server numbers nothing: every spawn is taken as before.
    CHECK(cArrive(2, 0) == Spawn::kNewest);
    CHECK(records.NewestGeneration(2) == 8);

    // The disconnect forgets what was seen: a new session starts the count again.
    records.Clear();
    CHECK(records.NewestGeneration(2) == 0);
    CHECK(cArrive(2, 1) == Spawn::kNewest);
}
