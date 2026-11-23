#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>

#include <string>

#include <catch2/catch.hpp>

#include <TwinSpawn.h>

using namespace TwinSpawn;

namespace
{
Spawn MakeSpawn()
{
    Spawn spawn{};
    spawn.PlayerId = 2;
    spawn.BaseId = GameId(0, 0x10A2B);
    spawn.CellId = GameId(0, 0x9000);
    spawn.WorldSpaceId = GameId(0, 0x3C);
    spawn.Position = {100.f, 200.f, 0.f};
    return spawn;
}

Candidate MakeCandidate()
{
    Candidate candidate{};
    candidate.OwnerId = 1;
    candidate.SameParty = true;
    candidate.BaseId = GameId(0, 0x10A2B);
    candidate.CellId = GameId(0, 0x9000);
    candidate.WorldSpaceId = GameId(0, 0x3C);
    candidate.SpawnPosition = {110.f, 200.f, 0.f};
    candidate.AgeSeconds = 5;
    return candidate;
}
} // namespace

TEST_CASE("Another party game's spawn of the same kind at the same marker is the twin", "[twin_spawn]")
{
    const Spawn spawn = MakeSpawn();
    REQUIRE(Check(spawn, MakeCandidate()) == Miss::kNone);
    REQUIRE(FindTwin(spawn, {MakeCandidate()}) == std::optional<std::size_t>(0));
}

TEST_CASE("A twin is refused for each reason", "[twin_spawn]")
{
    const Spawn spawn = MakeSpawn();

    Candidate same = MakeCandidate();
    same.OwnerId = spawn.PlayerId;
    REQUIRE(Check(spawn, same) == Miss::kSamePlayer);

    Candidate party = MakeCandidate();
    party.SameParty = false;
    REQUIRE(Check(spawn, party) == Miss::kOtherParty);

    Candidate base = MakeCandidate();
    base.BaseId = GameId(0, 0x10A2C);
    REQUIRE(Check(spawn, base) == Miss::kBase);

    Candidate place = MakeCandidate();
    place.CellId = GameId(0, 0x9001);
    REQUIRE(Check(spawn, place) == Miss::kPlace);

    Candidate world = MakeCandidate();
    world.WorldSpaceId = GameId(0, 0x3D);
    REQUIRE(Check(spawn, world) == Miss::kPlace);

    Candidate old = MakeCandidate();
    old.AgeSeconds = kWindowSeconds + 1;
    REQUIRE(Check(spawn, old) == Miss::kTooOld);

    Candidate paired = MakeCandidate();
    paired.PairedWithPlayer = true;
    REQUIRE(Check(spawn, paired) == Miss::kAlreadyPaired);

    Candidate far = MakeCandidate();
    far.SpawnPosition = {100.f + kReachUnits + 1.f, 200.f, 0.f};
    REQUIRE(Check(spawn, far) == Miss::kTooFar);

    REQUIRE_FALSE(FindTwin(spawn, {same, party, base, place, world, old, paired, far}).has_value());
}

TEST_CASE("A spawn with no base is never paired", "[twin_spawn]")
{
    Spawn spawn = MakeSpawn();
    spawn.BaseId = GameId{};
    Candidate candidate = MakeCandidate();
    candidate.BaseId = GameId{};
    REQUIRE(Check(spawn, candidate) == Miss::kBase);
}

TEST_CASE("Levelled picks must agree only when both are known", "[twin_spawn]")
{
    Spawn spawn = MakeSpawn();
    Candidate candidate = MakeCandidate();

    spawn.LeveledNpcPickId = GameId(0, 0x500);
    REQUIRE(Check(spawn, candidate) == Miss::kNone);

    candidate.LeveledNpcPickId = GameId(0, 0x500);
    REQUIRE(Check(spawn, candidate) == Miss::kNone);

    candidate.LeveledNpcPickId = GameId(0, 0x501);
    REQUIRE(Check(spawn, candidate) == Miss::kPick);
}

TEST_CASE("The edges of the window and the reach still pair", "[twin_spawn]")
{
    const Spawn spawn = MakeSpawn();
    Candidate candidate = MakeCandidate();
    candidate.AgeSeconds = kWindowSeconds;
    candidate.SpawnPosition = {100.f + kReachUnits, 200.f, 0.f};
    REQUIRE(Check(spawn, candidate) == Miss::kNone);
}

TEST_CASE("The nearest twin wins, then the oldest", "[twin_spawn]")
{
    const Spawn spawn = MakeSpawn();

    Candidate farther = MakeCandidate();
    farther.SpawnPosition = {300.f, 200.f, 0.f};
    Candidate nearer = MakeCandidate();
    nearer.SpawnPosition = {120.f, 200.f, 0.f};
    REQUIRE(FindTwin(spawn, {farther, nearer}) == std::optional<std::size_t>(1));

    Candidate younger = MakeCandidate();
    younger.AgeSeconds = 3;
    Candidate older = MakeCandidate();
    older.AgeSeconds = 9;
    REQUIRE(FindTwin(spawn, {younger, older}) == std::optional<std::size_t>(1));
}

TEST_CASE("Three spawns in each game pair one to one", "[twin_spawn]")
{
    // Game 1 made three at three markers; game 2 makes its three, one at a time, at the same markers.
    std::vector<Candidate> candidates;
    for (float x : {0.f, 200.f, 400.f})
    {
        Candidate candidate = MakeCandidate();
        candidate.SpawnPosition = {x, 0.f, 0.f};
        candidates.push_back(candidate);
    }

    for (float x : {400.f, 0.f, 200.f})
    {
        Spawn spawn = MakeSpawn();
        spawn.Position = {x, 0.f, 0.f};
        const auto twin = FindTwin(spawn, candidates);
        REQUIRE(twin.has_value());
        REQUIRE(candidates[*twin].SpawnPosition.x == x);
        candidates[*twin].PairedWithPlayer = true;
    }

    // A fourth in game 2 has nothing left to stand for.
    Spawn extra = MakeSpawn();
    extra.Position = {0.f, 0.f, 0.f};
    REQUIRE_FALSE(FindTwin(extra, candidates).has_value());
    REQUIRE(Check(extra, candidates[0]) == Miss::kAlreadyPaired);
    REQUIRE(IsNearMiss(Miss::kAlreadyPaired));
    REQUIRE_FALSE(IsNearMiss(Miss::kBase));
}

TEST_CASE("Every miss has words", "[twin_spawn]")
{
    for (Miss miss : {Miss::kNone, Miss::kSamePlayer, Miss::kOtherParty, Miss::kBase, Miss::kPick, Miss::kPlace, Miss::kTooOld, Miss::kAlreadyPaired, Miss::kTooFar})
        REQUIRE(std::string(Describe(miss)) != "unknown");
}
