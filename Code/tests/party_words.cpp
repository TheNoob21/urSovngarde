#include <catch2/catch.hpp>

#include <algorithm>

#include <LoadLatch.h>
#include <PartyWords.h>

TEST_CASE("a word of power one player is taught goes to the party once", "[words]")
{
    PartyWords words;
    const uint64_t cFus = PartyWords::Key(0, 0x13E22);
    const uint64_t cDragonbornWord = PartyWords::Key(2, 0x13E22);

    SECTION("the first member taught it tells the party, the others' games taught it by the same quest do not")
    {
        CHECK(words.IsNew(1, cFus));
        CHECK_FALSE(words.IsNew(1, cFus));
        CHECK_FALSE(words.IsNew(1, cFus));
    }

    SECTION("another party, or the same id in another plugin, is told it on its own")
    {
        CHECK(words.IsNew(1, cFus));
        CHECK(words.IsNew(2, cFus));
        CHECK(words.IsNew(1, cDragonbornWord));
    }

    SECTION("the key keeps the plugin and the form apart")
    {
        CHECK(PartyWords::Key(2, 0x1A2B) == 0x0000000200001A2Bull);
        CHECK(PartyWords::Key(0, 0x1A2B) != PartyWords::Key(1, 0x1A2B));
    }
}

TEST_CASE("a member who joins a party later is told every word the party was taught", "[words]")
{
    PartyWords words;
    const uint64_t cFus = PartyWords::Key(0, 0x13E22);
    const uint64_t cRo = PartyWords::Key(0, 0x13E23);
    CHECK(words.IsNew(1, cFus));
    CHECK(words.IsNew(1, cRo));
    CHECK_FALSE(words.IsNew(1, cFus));
    CHECK(words.IsNew(2, PartyWords::Key(2, 0x1A2B)));

    SECTION("each word once, from its own party only")
    {
        auto taught = words.Of(1);
        std::sort(taught.begin(), taught.end());
        CHECK(taught == std::vector<uint64_t>{cFus, cRo});
        CHECK(words.Of(2) == std::vector<uint64_t>{PartyWords::Key(2, 0x1A2B)});
    }

    SECTION("a party that was taught none, or never existed, has none to send")
    {
        CHECK(words.Of(3).empty());
    }
}

TEST_CASE("a word a party member was taught waits here while a save loads, for this connection and party only", "[words]")
{
    WordGrants grants;
    grants.BindPlayer(0xAA);
    constexpr uint32_t cFeim = 0x32917;

    SECTION("taught once, after the load, in the same connection and party")
    {
        grants.Add(cFeim);
        CHECK(grants.Due(false, 0xAA).empty());
        CHECK(grants.Due(true, 0xAA) == std::vector<uint32_t>{cFeim});
        grants.Taught(cFeim);
        CHECK(grants.Due(true, 0xAA).empty());
    }

    SECTION("not taught when the party is left before the load ends, nor in the solo game loaded after")
    {
        grants.Add(cFeim);
        CHECK(grants.Due(false, 0xAA).empty());
        grants.NewTerm(); // left the party
        CHECK(grants.Due(true, 0xAA).empty());
        CHECK(grants.Size() == 0);
    }

    SECTION("not taught after a disconnection, nor in another party joined after")
    {
        grants.Add(cFeim);
        grants.NewTerm(); // disconnected
        grants.NewTerm(); // connected
        grants.NewTerm(); // joined another party
        CHECK(grants.Due(true, 0xAA).empty());
    }

    SECTION("kept while the game's teaching function is not there, and given once")
    {
        grants.Add(cFeim);
        grants.Add(cFeim);
        CHECK(grants.Due(true, 0xAA) == std::vector<uint32_t>{cFeim});
        CHECK(grants.Due(true, 0xAA) == std::vector<uint32_t>{cFeim}); // not taught: still waiting
        CHECK(grants.Size() == 1);
    }
}

TEST_CASE("word grants validate the actual save player at execution, including receive during load", "[words]")
{
    WordGrants grants;
    grants.BindPlayer(0x12345678); // connection authenticated for A
    grants.Add(1);                 // receive before load entry
    CHECK(grants.Due(true, 0x12345678, true).empty());
    grants.Add(2); // receive while native load is pending
    CHECK(grants.Due(true, 0x87654321, true).empty());

    SECTION("A finishes loading before the connection service's timer")
    {
        CHECK(grants.Due(true, 0x12345678) == std::vector<uint32_t>{1, 2});
        grants.Taught(1);
        grants.Taught(2);
        CHECK(grants.Due(true, 0x12345678).empty());
    }
    SECTION("B finishes loading with the same display name, before reconnection")
    {
        CHECK(grants.Due(true, 0x87654321).empty());
        grants.Add(3); // the connection and party still belong to A
        CHECK(grants.Due(true, 0x87654321).empty());
        grants.BindPlayer(0x87654321); // only a new connection can bind B
        CHECK(grants.Size() == 0);
        CHECK(grants.Due(true, 0x87654321).empty());
    }
    SECTION("an unidentifiable save cannot receive a grant")
    {
        CHECK(grants.Due(true, 0).empty());
        grants.BindPlayer(0);
        grants.Add(3);
        CHECK(grants.Due(true, 0).empty());
        CHECK(grants.Due(true, 0x12345678).empty());
    }
}

TEST_CASE("a load the game accepted holds the words until its own load event or refusal", "[words]")
{
    LoadLatch latch;
    WordGrants grants;
    grants.BindPlayer(0xAA);
    grants.Add(7);
    latch.Begin();
    CHECK(latch.Loading());
    CHECK(grants.Due(true, 0xAA, latch.Loading()).empty());

    SECTION("a request the game refused ends it at once, and the word is taught")
    {
        latch.Refused(latch.Generation());
        CHECK_FALSE(latch.Loading());
        CHECK(grants.Due(true, 0xAA, latch.Loading()) == std::vector<uint32_t>{7});
    }
    SECTION("a refusal of an older request leaves the newer load latched")
    {
        const uint64_t cOld = latch.Generation();
        latch.Begin();
        latch.Refused(cOld);
        CHECK(latch.Loading());
        CHECK(grants.Due(true, 0xAA, latch.Loading()).empty());
    }
    SECTION("the load event ends it")
    {
        latch.Complete();
        CHECK_FALSE(latch.Loading());
        CHECK(grants.Due(true, 0xAA, latch.Loading()) == std::vector<uint32_t>{7});
    }
    SECTION("a loading menu open at 1000 ms and closed at 25000 ms with the load event pending holds it")
    {
        // The two looks the review names: nothing the menu does, or the time between looks, is evidence the load is over.
        // The latch has no way to take a menu sight or a clock, so the latch and the waiting word stay as they were.
        CHECK(latch.Loading());
        CHECK(grants.Due(true, 0xAA, latch.Loading()).empty());
        CHECK(grants.Size() == 1);
    }
    SECTION("a new load after the end latches again")
    {
        latch.Refused(latch.Generation());
        latch.Begin();
        CHECK(grants.Due(true, 0xAA, latch.Loading()).empty());
    }
}

TEST_CASE("a party is told no more words past the cap, and forgets them when it is gone", "[words]")
{
    PartyWords words;
    for (uint32_t i = 1; i <= PartyWords::kMaxPerParty; ++i)
        REQUIRE(words.IsNew(1, PartyWords::Key(0, i)));
    CHECK_FALSE(words.IsNew(1, PartyWords::Key(0, 0xFFFFFF)));
    CHECK(words.Of(1).size() == PartyWords::kMaxPerParty);
    CHECK(words.IsNew(2, PartyWords::Key(0, 0xFFFFFF)));

    words.Forget(1);
    CHECK(words.Of(1).empty());
    CHECK(words.IsNew(1, PartyWords::Key(0, 1)));
}

TEST_CASE("a word with no form id is not told to a party, and does not use up its room", "[words]")
{
    PartyWords words;
    CHECK_FALSE(PartyWords::IsValid(PartyWords::Key(0, 0)));
    CHECK_FALSE(PartyWords::IsValid(PartyWords::Key(2, 0)));
    CHECK(PartyWords::IsValid(PartyWords::Key(0, 0x13E22)));
    CHECK(PartyWords::IsValid(PartyWords::Key(2, 1)));

    CHECK_FALSE(words.IsNew(1, PartyWords::Key(0, 0)));
    CHECK_FALSE(words.IsNew(1, PartyWords::Key(2, 0)));
    CHECK(words.Of(1).empty());
}

TEST_CASE("a game keeps no more waiting words than a party can be told, each once", "[words]")
{
    WordGrants grants;
    grants.BindPlayer(0xAA);
    for (uint32_t i = 1; i <= PartyWords::kMaxPerParty + 10; ++i)
        grants.Add(i);
    CHECK(grants.Size() == PartyWords::kMaxPerParty);
    CHECK(grants.Due(true, 0xAA).size() == PartyWords::kMaxPerParty);

    grants.Add(1); // already waiting
    CHECK(grants.Size() == PartyWords::kMaxPerParty);

    grants.Taught(1);
    grants.Add(PartyWords::kMaxPerParty + 20);
    CHECK(grants.Size() == PartyWords::kMaxPerParty);
}
