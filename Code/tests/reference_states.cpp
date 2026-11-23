#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <ReferenceStateLedger.h>
#include <ReferenceStates.h>
#include <TrapSync.h>

#include <Messages/ClientMessageFactory.h>
#include <Messages/NotifyReferenceStates.h>
#include <Messages/RequestReferenceState.h>
#include <Messages/ServerMessageFactory.h>

#include <optional>
#include <string>

using namespace ReferenceStates;
using namespace std::chrono_literals;
using TiltedPhoques::Buffer;

namespace
{
// A cell id and nothing else: enough for the ledger, which asks the caller what is in range.
struct Place
{
    uint32_t Cell;
};

auto InCell(uint32_t aCell)
{
    return [aCell](const Place& acPlace)
    {
        return acPlace.Cell == aCell;
    };
}

using Ledger = ReferenceStateLedger<Place>;
using Result = Ledger::Result;

constexpr uint64_t cVein = 0x10DCC9;
constexpr uint64_t cPlant = 0x4B9A1;
constexpr uint64_t cTrap = 0x7A11E;
constexpr uint8_t cDestruction = static_cast<uint8_t>(Kind::Destruction);
constexpr uint8_t cTrapKind = static_cast<uint8_t>(Kind::Trap);
constexpr uint8_t cUsed = static_cast<uint8_t>(Kind::Used);
const Point cHere{100.f, 200.f, 0.f};
const auto cStart = std::chrono::steady_clock::time_point{} + 1h;
} // namespace

TEST_CASE("a report is taken from a player in the reference's cell who used it or stands near it", "[reference_states]")
{
    Ledger ledger;
    const Point cNear{cHere.X + 300.f, cHere.Y, cHere.Z};
    const Point cFar{cHere.X + 5000.f, cHere.Y, cHere.Z};

    SECTION("a sender outside the reference's cell is refused, used or near")
    {
        ledger.NoteUse("Brother", cPlant, cStart);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, false, cNear, cHere, cStart) == Refusal::OffCell);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, cNear, cHere, cStart) == Refusal::None);
    }

    SECTION("a sender who never used it and stands out of reach is refused")
    {
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, cFar, cHere, cStart) == Refusal::NoInteraction);
        // Nor one the server has no place for.
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, std::nullopt, cHere, cStart) == Refusal::NoInteraction);
        // Another player's use is not this one's.
        ledger.NoteUse("Sister", cPlant, cStart);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, cFar, cHere, cStart) == Refusal::NoInteraction);
    }

    SECTION("a reference back as new needs no interaction, as its cell reset it, but the sender must be in that cell")
    {
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 0, true, cFar, cHere, cStart) == Refusal::None);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 0, true, cFar, cHere, cStart) == Refusal::None);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 0, false, cNear, cHere, cStart) == Refusal::OffCell);
        // A bear trap's state is never kept, so a sprung one is no reset.
        CHECK(ledger.Judge("Brother", cTrap, cTrapKind, 0, true, cFar, cHere, cStart) == Refusal::NoInteraction);
    }

    SECTION("a use counts for a minute: a vein is depleted some seconds after its use, from wherever the miner stands")
    {
        ledger.NoteUse("Brother", cVein, cStart);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 1, true, cFar, cHere, cStart + kUseWindow) == Refusal::None);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 1, true, cFar, cHere, cStart + kUseWindow + 1s) == Refusal::NoInteraction);
    }

    SECTION("near is a step for a bear trap, and a bow's range for a destructible or a rune")
    {
        const Point cBowShot{cHere.X + 3000.f, cHere.Y, cHere.Z};
        CHECK(ledger.Judge("Brother", cTrap, cTrapKind, 0, true, Point{cHere.X + 1024.f, cHere.Y, cHere.Z}, cHere, cStart) == Refusal::None);
        CHECK(ledger.Judge("Brother", cTrap, cTrapKind, 0, true, Point{cHere.X + 1025.f, cHere.Y, cHere.Z}, cHere, cStart) == Refusal::NoInteraction);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, Point{cHere.X + 4096.f, cHere.Y, cHere.Z}, cHere, cStart) == Refusal::None);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 1, true, Point{cHere.X + 4097.f, cHere.Y, cHere.Z}, cHere, cStart) == Refusal::NoInteraction);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 2, true, cBowShot, cHere, cStart) == Refusal::None);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 2, true, Point{cHere.X + 4097.f, cHere.Y, cHere.Z}, cHere, cStart) == Refusal::NoInteraction);
    }

    SECTION("a kept reference is where its first report put it, not where a later sender says")
    {
        REQUIRE(ledger.Record(cVein, Kind::Destruction, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        // Claims the vein stands beside it, but the kept place is far.
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 2, true, cFar, cFar, cStart) == Refusal::NoInteraction);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, 2, true, cNear, cFar, cStart) == Refusal::None);
    }

    SECTION("an unknown kind or a value outside its kind is refused")
    {
        ledger.NoteUse("Brother", cPlant, cStart);
        CHECK(ledger.Judge("Brother", cPlant, 3, 1, true, cNear, cHere, cStart) == Refusal::UnknownKind);
        CHECK(ledger.Judge("Brother", cPlant, 0xFF, 1, true, cNear, cHere, cStart) == Refusal::UnknownKind);
        CHECK(ledger.Judge("Brother", cPlant, cUsed, 2, true, cNear, cHere, cStart) == Refusal::BadValue);
        CHECK(ledger.Judge("Brother", cTrap, cTrapKind, 2, true, cNear, cHere, cStart) == Refusal::BadValue);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, kMaxStage + 1, true, cNear, cHere, cStart) == Refusal::BadValue);
        CHECK(ledger.Judge("Brother", cVein, cDestruction, kMaxStage, true, cNear, cHere, cStart) == Refusal::None);
    }

    SECTION("the recent uses are bounded, the oldest going first")
    {
        for (uint64_t i = 0; i < Ledger::kMaxRecentUses + 1; ++i)
            ledger.NoteUse("Brother", 0x1000 + i, cStart);
        CHECK_FALSE(ledger.UsedRecently("Brother", 0x1000, cStart));
        CHECK(ledger.UsedRecently("Brother", 0x1001, cStart));
    }

    SECTION("a player sends a handful a second at most")
    {
        TrapSync::RateLimit limit;
        for (uint32_t i = 0; i < TrapSync::RateLimit::kPerSecond; ++i)
            CHECK(limit.Allow(cStart));
        CHECK_FALSE(limit.Allow(cStart + 999ms));
        CHECK(limit.Allow(cStart + 1s));
    }
}

TEST_CASE("the server keeps the states a cell reset brings back, and only while that reset has not come", "[reference_states]")
{
    Ledger ledger;

    SECTION("a bear trap is passed on but never kept: its game arms it again as it loads")
    {
        CHECK(ledger.Record(cTrap, Kind::Trap, 0, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Size() == 0);
        CHECK(ledger.CatchUp("Brother", InCell(1), 10.0).empty());
    }

    SECTION("a destructible only breaks further; a game behind it is sent the kept stage")
    {
        CHECK(ledger.Record(cVein, Kind::Destruction, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Record(cVein, Kind::Destruction, 1, {1}, cHere, {"Sister"}, 10.0) == Result::Same);
        CHECK(ledger.Record(cVein, Kind::Destruction, 3, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Record(cVein, Kind::Destruction, 2, {1}, cHere, {"Sister"}, 10.0) == Result::Behind);
        CHECK(ledger.ValueOf(cVein, Kind::Destruction) == 3);
    }

    SECTION("a game that reports it back as new ends the kept state, and nothing is passed on")
    {
        REQUIRE(ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Record(cPlant, Kind::Used, 0, {1}, cHere, {"Owner"}, 300.0) == Result::Ended);
        CHECK(ledger.Size() == 0);
        CHECK(ledger.Record(cPlant, Kind::Used, 0, {1}, cHere, {"Owner"}, 300.0) == Result::Same);
        CHECK(ledger.CatchUp("Brother", InCell(1), 300.0).empty());
    }

    SECTION("a report back as new is not taken while another player has the place loaded: the shared world never left it")
    {
        REQUIRE(ledger.Record(cVein, Kind::Destruction, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Record(cVein, Kind::Destruction, 0, {1}, cHere, {"Sister", "Owner"}, 300.0, true) == Result::Same);
        CHECK(ledger.ValueOf(cVein, Kind::Destruction) == 1);
        CHECK(ledger.Record(cVein, Kind::Destruction, 0, {1}, cHere, {"Sister"}, 300.0, false) == Result::Ended);
    }

    SECTION("a player who comes later gets each kept state once, with the hours since anyone had it loaded")
    {
        REQUIRE(ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        REQUIRE(ledger.Record(cVein, Kind::Destruction, 1, {2}, cHere, {"Owner"}, 12.0) == Result::Changed);

        const auto cStates = ledger.CatchUp("Brother", InCell(1), 50.5);
        REQUIRE(cStates.size() == 1);
        CHECK(cStates[0].Ref == cPlant);
        CHECK(cStates[0].What == Kind::Used);
        CHECK(cStates[0].Value == 1);
        CHECK(cStates[0].IdleHours == 40);
        CHECK(ledger.CatchUp("Brother", InCell(1), 51.0).empty());
        // The sender's game has it: its own reset is left alone.
        CHECK(ledger.CatchUp("Owner", InCell(1), 51.0).empty());
    }

    SECTION("a player who leaves is forgotten: coming back, maybe from an older save, it gets the kept states again")
    {
        REQUIRE(ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"#1", "#2"}, 10.0) == Result::Changed);
        ledger.NoteUse("#1", cPlant, cStart);
        CHECK(ledger.CatchUp("#1", InCell(1), 20.0).empty());

        ledger.Forget("#1");
        CHECK_FALSE(ledger.UsedRecently("#1", cPlant, cStart));
        const auto cStates = ledger.CatchUp("#1", InCell(1), 20.0);
        REQUIRE(cStates.size() == 1);
        CHECK(cStates[0].Ref == cPlant);
        CHECK(ledger.CatchUp("#1", InCell(1), 21.0).empty());
        // The one still here keeps what its game has.
        CHECK(ledger.CatchUp("#2", InCell(1), 21.0).empty());
        CHECK(ledger.Size() == 1);
    }

    SECTION("two players of one name are two games, as the server knows them by connection")
    {
        REQUIRE(ledger.Record(cVein, Kind::Destruction, 2, {1}, cHere, {"#3"}, 10.0) == Result::Changed);
        const auto cStates = ledger.CatchUp("#4", InCell(1), 10.0);
        REQUIRE(cStates.size() == 1);
        CHECK(cStates[0].Value == 2);
    }

    SECTION("a place another player still has loaded counts as seen now")
    {
        REQUIRE(ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        ledger.Seen(InCell(1), 400.0);
        ledger.Seen(InCell(2), 500.0);
        const auto cStates = ledger.CatchUp("Brother", InCell(1), 410.0);
        REQUIRE(cStates.size() == 1);
        CHECK(cStates[0].IdleHours == 10);
    }

    SECTION("a full store of 2048 evicts the oldest recorded; one that changes counts as new, one reported again does not")
    {
        REQUIRE(ledger.Record(0x1000, Kind::Destruction, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        for (uint64_t i = 1; i < Ledger::kMaxEntries; ++i)
            REQUIRE(ledger.Record(0x1000 + i, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        REQUIRE(ledger.Size() == Ledger::kMaxEntries);

        // The vein breaks further, so the first plant is the oldest now.
        REQUIRE(ledger.Record(0x1000, Kind::Destruction, 2, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Record(0x9000, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Size() == Ledger::kMaxEntries);
        CHECK(ledger.ValueOf(0x1000, Kind::Destruction) == 2);
        CHECK_FALSE(ledger.ValueOf(0x1001, Kind::Used).has_value());
        CHECK(ledger.ValueOf(0x9000, Kind::Used) == 1);

        // Picked again in another game: it stays where it was, the oldest.
        REQUIRE(ledger.Record(0x1002, Kind::Used, 1, {1}, cHere, {"Sister"}, 10.0) == Result::Same);
        CHECK(ledger.Record(0x9001, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
        CHECK(ledger.Size() == Ledger::kMaxEntries);
        CHECK_FALSE(ledger.ValueOf(0x1002, Kind::Used).has_value());
        CHECK(ledger.ValueOf(0x1003, Kind::Used) == 1);
    }

    SECTION("at most a message's worth at once, the rest at the next cell change")
    {
        for (uint64_t i = 0; i < Ledger::kMaxCatchUp + 10; ++i)
            ledger.Record(0x1000 + i, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0);
        CHECK(ledger.CatchUp("Brother", InCell(1), 10.0).size() == Ledger::kMaxCatchUp);
        CHECK(ledger.CatchUp("Brother", InCell(1), 10.0).size() == 10);
    }

    SECTION("the last player out ends the session")
    {
        ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0);
        ledger.NoteUse("Owner", cPlant, cStart);
        ledger.Clear();
        CHECK(ledger.Size() == 0);
        CHECK_FALSE(ledger.UsedRecently("Owner", cPlant, cStart));
    }
}

TEST_CASE("a kept state is applied only while the receiving game would not have reset its cell", "[reference_states]")
{
    SECTION("at its respawn time the game resets the cell")
    {
        CHECK(IsFresh(0, 240));
        CHECK(IsFresh(239, 240));
        CHECK_FALSE(IsFresh(240, 240));
        CHECK_FALSE(IsFresh(241, 240));
    }

    SECTION("idle hours are whole hours on the server's calendar, never negative, capped")
    {
        CHECK(IdleHours(250.0, 10.5) == 239);
        CHECK(IdleHours(250.5, 10.5) == 240);
        CHECK(IdleHours(10.0, 50.0) == 0);
        CHECK(IdleHours(1e12, 0.0) == kMaxIdleHours);
        CHECK_FALSE(IsFresh(kMaxIdleHours, 1 << 30));
    }

    SECTION("a setting of nothing, of 0 or less takes Skyrim's default; one past a year is a year")
    {
        CHECK(RespawnHours(0) == kDefaultRespawnHours);
        CHECK(RespawnHours(-5) == kDefaultRespawnHours);
        CHECK(RespawnHours(72) == 72);
        CHECK(RespawnHours(1 << 30) == kMaxRespawnHours);
        CHECK(IsFresh(239, 0));
        CHECK_FALSE(IsFresh(240, 0));
        CHECK(IsFresh(kMaxRespawnHours - 1, 1 << 30));
        CHECK_FALSE(IsFresh(kMaxRespawnHours, 1 << 30));
    }
}

TEST_CASE("reference states go over the network whole, and a kind the receiver does not know is passed over", "[reference_states]")
{
    SECTION("RequestReferenceState")
    {
        Buffer buff(1000);
        RequestReferenceState sendMessage;
        sendMessage.CellId = GameId(0, 0x1F2E);
        sendMessage.WorldSpaceId = GameId(0, 0x3C);
        sendMessage.CenterCoords = GridCellCoords(-3, 7);
        sendMessage.Position = glm::vec3(1024.5f, -2048.f, 64.f);
        sendMessage.State.Id = GameId(0, 0x10DCC9);
        sendMessage.State.Kind = cDestruction;
        sendMessage.State.Value = 1;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kRequestReferenceState);
        CHECK(*CastUnique<RequestReferenceState>(std::move(pMessage)) == sendMessage);
    }

    SECTION("NotifyReferenceStates, with an unknown kind decoded and left to the receiver")
    {
        Buffer buff(2000);
        NotifyReferenceStates sendMessage;
        sendMessage.States.push_back({GameId(0, 0x4B9A1), cUsed, 1, 239});
        sendMessage.States.push_back({GameId(2, 0x1234), 9, 200, 0});
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        const auto pRecv = CastUnique<NotifyReferenceStates>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        CHECK(IsKnown(pRecv->States[0].Kind));
        CHECK_FALSE(IsKnown(pRecv->States[1].Kind));
        CHECK_FALSE(IsValid(pRecv->States[1].Kind, pRecv->States[1].Value));
    }

    SECTION("more states than a message holds are dropped with it")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kNotifyReferenceStates, 8);
        Serialization::WriteVarInt(writer, NotifyReferenceStates::kMax + 1);
        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
    }
}

TEST_CASE("the bases are the game's own", "[reference_states]")
{
    CHECK(IsBearTrap(0x0007144D));
    CHECK_FALSE(IsBearTrap(0x0207144D));
    CHECK(IsRuneProjectile(0x0007326E));
    CHECK_FALSE(IsRuneProjectile(0x0005DB90)); // FireRune, the spell, not its projectile
    CHECK(IsNirnroot(0x000E1FB2));
    CHECK(IsNirnroot(0x000B6FB9));
    CHECK_FALSE(IsNirnroot(0x000E1FB4)); // NirnrootGlowACTIVATOR
    CHECK(IsMiningFurniture(ActivationCatchUp::Plugin::Skyrim, 0x0613A6));
    CHECK(IsMiningFurniture(ActivationCatchUp::Plugin::Dragonborn, 0x03A491));
    CHECK_FALSE(IsMiningFurniture(ActivationCatchUp::Plugin::Skyrim, 0x03A491));
    CHECK_FALSE(IsMiningFurniture(ActivationCatchUp::Plugin::Other, 0x0613A6));
    CHECK(IsPlainDestructible(ActivationCatchUp::Plugin::Skyrim, 0x0EC3DE));       // FXspiderWebKitDestructible
    CHECK(IsPlainDestructible(ActivationCatchUp::Plugin::Dragonborn, 0x03A9A9));   // its Dragonborn twin
    CHECK_FALSE(IsPlainDestructible(ActivationCatchUp::Plugin::Skyrim, 0x0F02E1)); // BFXInertOilPool, whose fire burns
    CHECK_FALSE(IsPlainDestructible(ActivationCatchUp::Plugin::Skyrim, 0x04FCDD)); // TG01BersiDwemerUrn, scripted
    CHECK_FALSE(IsPlainDestructible(ActivationCatchUp::Plugin::Skyrim, 0x10DCC9)); // an ore vein, shared as one
    CHECK_FALSE(IsPlainDestructible(ActivationCatchUp::Plugin::Other, 0x0EC3DE));
    CHECK_FALSE(IsKept(Kind::Trap));
    CHECK(IsReset(Kind::Destruction, 0));
    CHECK_FALSE(IsReset(Kind::Trap, 0));
}

TEST_CASE("a game that loads a save is sent the kept states again, and only its own marks change", "[reference_states]")
{
    Ledger ledger;
    REQUIRE(ledger.Record(cPlant, Kind::Used, 1, {1}, cHere, {"Owner"}, 10.0) == Result::Changed);
    REQUIRE(ledger.CatchUp("Brother", InCell(1), 10.0).size() == 1);
    // Told once: not again on the next cell entry.
    CHECK(ledger.CatchUp("Brother", InCell(1), 10.0).empty());

    // Brother's game loads an older save, where the plant is back: it is sent again.
    ledger.NoteUse("Brother", cVein, cStart);
    ledger.ForgetToldTo("Brother");
    CHECK(ledger.CatchUp("Brother", InCell(1), 11.0).size() == 1);
    // The reporter's own mark stays, and Brother's recent use, which judges its reports, too.
    CHECK(ledger.CatchUp("Owner", InCell(1), 11.0).empty());
    CHECK(ledger.UsedRecently("Brother", cVein, cStart));
}

TEST_CASE("a player's game is sent the kept states again after a load at most once in a while", "[reference_states]")
{
    using Answer = LoadCatchUpLimit::Answer;
    LoadCatchUpLimit limit;
    CHECK(limit.Ask(7, cStart) == Answer::kTaken);
    // A flood of asks: the first one dropped is logged, the rest quietly.
    CHECK(limit.Ask(7, cStart + 1s) == Answer::kDropped);
    CHECK(limit.Ask(7, cStart + 2s) == Answer::kDroppedQuietly);
    // Another player is counted on its own.
    CHECK(limit.Ask(8, cStart + 2s) == Answer::kTaken);
    // Once the time has passed, the next load is taken again, and a later drop is logged again.
    CHECK(limit.Ask(7, cStart + LoadCatchUpLimit::kEvery) == Answer::kTaken);
    CHECK(limit.Ask(7, cStart + LoadCatchUpLimit::kEvery + 1s) == Answer::kDropped);
    // A player who left and came back starts afresh.
    limit.Forget(7);
    CHECK(limit.Ask(7, cStart + LoadCatchUpLimit::kEvery + 2s) == Answer::kTaken);
}
