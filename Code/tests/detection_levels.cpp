#include <catch2/catch.hpp>

#include <DetectionLevels.h>
#include <VRAddressOverrides.h>

#include <algorithm>

using namespace DetectionLevels;

TEST_CASE("The game's detection level falls in a band, and a band gives back a level within it", "[detection_levels]")
{
    CHECK(BandOf(1) == Band::kSeen);
    CHECK(BandOf(100) == Band::kSeen);
    CHECK(BandOf(0) == Band::kNoticed);
    CHECK(BandOf(kNoticedFrom + 1) == Band::kNoticed);
    CHECK(BandOf(kNoticedFrom) == Band::kUnseen);
    CHECK(BandOf(-1000) == Band::kUnseen);

    for (const Band cBand : {Band::kUnseen, Band::kNoticed, Band::kSeen})
        CHECK(BandOf(LevelOf(cBand)) == cBand);

    // Only "seen" is detected for the game (above 0), so only it makes a copy a witness.
    CHECK(LevelOf(Band::kSeen) > 0);
    CHECK_FALSE(LevelOf(Band::kNoticed) > 0);
    CHECK_FALSE(LevelOf(Band::kUnseen) > 0);
}

TEST_CASE("The owner sends a word when it changes, and all of them at a heartbeat", "[detection_levels]")
{
    Outbox outbox;
    uint64_t now = 1000;

    // The first round is a heartbeat: everything goes.
    outbox.Put(7, 1, Band::kUnseen);
    outbox.Put(8, 1, Band::kSeen);
    CHECK(outbox.Take(now).size() == 2);

    // Nothing changed: nothing goes.
    now += kReadEveryMs;
    outbox.Put(7, 1, Band::kUnseen);
    outbox.Put(8, 1, Band::kSeen);
    CHECK(outbox.Take(now).empty());

    // One changed: only it goes.
    now += kReadEveryMs;
    outbox.Put(7, 1, Band::kSeen);
    outbox.Put(8, 1, Band::kSeen);
    auto words = outbox.Take(now);
    REQUIRE(words.size() == 1);
    CHECK(words[0].NpcId == 7);
    CHECK(words[0].PlayerId == 1);
    CHECK(words[0].Value == Band::kSeen);

    // At the heartbeat, every word goes again, so a game that missed one, or came since, has them.
    now = 1000 + kHeartbeatMs;
    outbox.Put(7, 1, Band::kSeen);
    outbox.Put(8, 1, Band::kSeen);
    CHECK(outbox.Take(now).size() == 2);
}

TEST_CASE("A pair the owner no longer reads is forgotten, and new when it comes back", "[detection_levels]")
{
    Outbox outbox;
    outbox.Put(7, 1, Band::kSeen);
    CHECK(outbox.Take(1000).size() == 1);

    // Out of range for a round: no word, the other game's expires.
    CHECK(outbox.Take(1250).empty());

    // Back in range with the same band: sent again, as a new word.
    outbox.Put(7, 1, Band::kSeen);
    CHECK(outbox.Take(1500).size() == 1);
}

TEST_CASE("Two players near one NPC each get their own word", "[detection_levels]")
{
    Outbox outbox;
    outbox.Put(7, 1, Band::kSeen);
    outbox.Put(7, 2, Band::kUnseen);
    auto words = outbox.Take(1000);
    REQUIRE(words.size() == 2);
    std::sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.PlayerId < b.PlayerId; });
    CHECK(words[0].Value == Band::kSeen);
    CHECK(words[1].Value == Band::kUnseen);
}

TEST_CASE("A copy answers with its owner's word while it is fresh, and \"noticed\" otherwise", "[detection_levels]")
{
    // Never heard: noticed, just short of detected: not a witness, and not the "unseen" the copy's own (empty) knowledge would give.
    CHECK(Answer(nullptr, 1000) == Band::kNoticed);

    const Heard cSeen{Band::kSeen, 1000};
    CHECK(Answer(&cSeen, 1000) == Band::kSeen);
    // Change-only words: a word a few seconds old still stands while its owner keeps the NPC.
    CHECK(Answer(&cSeen, 1000 + kHeartbeatMs + kReadEveryMs) == Band::kSeen);
    CHECK(Answer(&cSeen, 1000 + kExpireMs) == Band::kSeen);
    CHECK(Answer(&cSeen, 1000 + kExpireMs + 1) == Band::kNoticed);

    const Heard cUnseen{Band::kUnseen, 1000};
    CHECK(Answer(&cUnseen, 2000) == Band::kUnseen);
    CHECK(Answer(&cUnseen, 1000 + kExpireMs + 1) == Band::kNoticed);

    // The heartbeat comes well before a word expires.
    CHECK(kHeartbeatMs * 2 < kExpireMs);
}

TEST_CASE("The server passes on only an owner's word about a party member near its NPC", "[detection_levels]")
{
    CHECK(ServerTakes(true, false, true, true, true));
    CHECK_FALSE(ServerTakes(false, false, true, true, true)); // not a character
    CHECK_FALSE(ServerTakes(true, true, true, true, true));   // a player's character is no NPC
    CHECK_FALSE(ServerTakes(true, false, false, true, true)); // another game runs it
    CHECK_FALSE(ServerTakes(true, false, true, false, true)); // not in the sender's party
    CHECK_FALSE(ServerTakes(true, false, true, true, false)); // too far from the NPC
}

TEST_CASE("A pair kept but not read stays as it was, and is no new word", "[detection_levels]")
{
    Outbox outbox;
    outbox.Put(7, 1, Band::kSeen);
    CHECK(outbox.Take(1000).size() == 1);

    // Not read this round (the per-round cap), still near: nothing goes, nothing is forgotten.
    outbox.Keep(7, 1);
    CHECK(outbox.Take(1250).empty());

    // Read again with the same band: still nothing new.
    outbox.Put(7, 1, Band::kSeen);
    CHECK(outbox.Take(1500).empty());

    // Kept through a heartbeat: its last word goes again.
    outbox.Keep(7, 1);
    auto words = outbox.Take(1000 + kHeartbeatMs);
    REQUIRE(words.size() == 1);
    CHECK(words[0].Value == Band::kSeen);

    // A pair never sent cannot be kept: there is no word to keep.
    outbox.Keep(8, 1);
    CHECK(outbox.Take(1000 + kHeartbeatMs + kReadEveryMs).empty());
}

TEST_CASE("VR finds the detection query and the crime alarm where the VR game has them", "[detection_levels]")
{
    // SE ids, as VR looks them up, and the VR offsets of the functions that match flat AE 37764, 37757 and 37422 by their
    // exception-table entries (size and prologue) and by the order of the functions around them.
    const auto cOffsetOf = [](const uint32_t aId) -> uint32_t
    {
        for (const auto& cEntry : kVRAddressOverrides)
            if (cEntry.id == aId)
                return cEntry.offset;
        return 0;
    };
    CHECK(cOffsetOf(36748) == 0x605190); // Actor::RequestDetectionLevel
    CHECK(cOffsetOf(36741) == 0x604f30); // Actor::GetDetectionState, just before it, as in flat
    CHECK(cOffsetOf(36427) == 0x5e5dc0); // Actor::StealAlarm, which asks it about each witness
}
