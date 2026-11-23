#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <ActivationCatchUp.h>
#include <ActivationLedger.h>

#include <Messages/ClientMessageFactory.h>
#include <Messages/NotifyActivationCatchUp.h>
#include <Messages/RequestActivationState.h>
#include <Messages/ServerMessageFactory.h>

#include <string>

using namespace ActivationCatchUp;
using TiltedPhoques::Buffer;

namespace
{
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

// A mechanism of two objects whose handle turns 0.5 rad per position.
Pose PoseAt(uint32_t aPosition)
{
    Pose pose(2 * kPoseValuesPerObject, 0);
    pose[0] = QuantizeRotation(1.f);
    pose[kPoseValuesPerObject] = QuantizeRotation(0.5f * static_cast<float>(aPosition));
    pose[kPoseValuesPerObject + 9] = QuantizeTranslation(12.5f);
    return pose;
}

// A mechanism as this game has it: each use moves it one position on, round its positions.
struct Mechanism
{
    uint32_t Position;
    uint32_t Positions;
    Readiness State{Readiness::Go};
    uint32_t Uses{};

    StatePacer::Look Look() const { return {State, State == Readiness::Go ? PoseAt(Position) : Pose{}}; }
    void Use()
    {
        Position = (Position + 1) % Positions;
        ++Uses;
    }
};

// Runs the pacer for aSeconds, a step a second, using the mechanism whenever it asks.
StatePacer::Action Run(StatePacer& aPacer, Mechanism& aMechanism, std::chrono::steady_clock::time_point aStart, int aSeconds)
{
    StatePacer::Action last = StatePacer::Action::None;
    for (int i = 0; i <= aSeconds; ++i)
    {
        const auto cStep = aPacer.Next(aStart + std::chrono::seconds(i), [&aMechanism](uint32_t) { return aMechanism.Look(); });
        if (cStep.What == StatePacer::Action::Use)
            aMechanism.Use();
        else if (cStep.What != StatePacer::Action::None)
            last = cStep.What;
    }
    return last;
}
} // namespace

TEST_CASE("two poses are one position within their tolerances, and never across meshes", "[activation_end_state]")
{
    CHECK(PoseMatches(PoseAt(1), PoseAt(1)));
    CHECK_FALSE(PoseMatches(PoseAt(1), PoseAt(2)));

    Pose noisy = PoseAt(1);
    noisy[kPoseValuesPerObject] += kRotationTolerance;
    noisy[kPoseValuesPerObject + 9] -= kTranslationTolerance;
    CHECK(PoseMatches(PoseAt(1), noisy));
    noisy[1] += kRotationTolerance + 1;
    CHECK_FALSE(PoseMatches(PoseAt(1), noisy));

    Pose otherMesh = PoseAt(1);
    otherMesh.resize(kPoseValuesPerObject);
    CHECK_FALSE(PoseMatches(PoseAt(1), otherMesh));
    CHECK_FALSE(PoseMatches(Pose{}, Pose{}));
    CHECK_FALSE(IsValidPoseSize(kMaxPoseValues + kPoseValuesPerObject));
    CHECK_FALSE(IsValidPoseSize(kPoseValuesPerObject + 1));

    CHECK(QuantizeRotation(-1.f) == -1000);
    CHECK(QuantizeTranslation(1e9f) == 32767);
    CHECK(QuantizeTranslation(-1e9f) == -32767);
}

TEST_CASE("a game uses a mechanism only until it is in the end state, and never guesses", "[activation_end_state]")
{
    SECTION("already there: no use")
    {
        CHECK(NextSettle(PoseAt(1), PoseAt(1), PoseAt(1), PoseAt(1), 0) == Settle::Done);
    }

    SECTION("a lever one use away, a pillar two uses away")
    {
        CHECK(NextSettle(PoseAt(1), PoseAt(0), PoseAt(0), PoseAt(0), 0) == Settle::Use);
        CHECK(NextSettle(PoseAt(1), PoseAt(0), PoseAt(0), PoseAt(1), 1) == Settle::Done);
        CHECK(NextSettle(PoseAt(2), PoseAt(0), PoseAt(0), PoseAt(1), 1) == Settle::Use);
    }

    SECTION("another mesh here: left alone without a use")
    {
        Pose otherMesh = PoseAt(0);
        otherMesh.resize(kPoseValuesPerObject);
        CHECK(NextSettle(PoseAt(1), otherMesh, otherMesh, otherMesh, 0) == Settle::GiveUp);
    }

    SECTION("it did not move, or it came round to where it started: left as it is")
    {
        CHECK(NextSettle(PoseAt(5), PoseAt(0), PoseAt(0), PoseAt(0), 1) == Settle::GiveUp);
        CHECK(NextSettle(PoseAt(5), PoseAt(0), PoseAt(1), PoseAt(0), 2) == Settle::GiveUp);
    }

    SECTION("at most kMaxStateUses uses")
    {
        CHECK(NextSettle(PoseAt(9), PoseAt(0), PoseAt(2), PoseAt(3), kMaxStateUses) == Settle::GiveUp);
    }
}

TEST_CASE("a late player's mechanism is set to the host's end state on top of its own save", "[activation_end_state]")
{
    using Clock = std::chrono::steady_clock;
    const auto cStart = Clock::now();
    StatePacer pacer;

    SECTION("a pillar the late player turned once solo needs one use, not the host's two")
    {
        Mechanism pillar{1, 3};
        pacer.Add(7, PoseAt(2), cStart);
        CHECK(Run(pacer, pillar, cStart, 30) == StatePacer::Action::Done);
        CHECK(pillar.Position == 2);
        CHECK(pillar.Uses == 1);
        CHECK(pacer.Size() == 0);
    }

    SECTION("a lever already pulled in the late player's save is left alone")
    {
        Mechanism lever{1, 2};
        pacer.Add(7, PoseAt(1), cStart);
        CHECK(Run(pacer, lever, cStart, 30) == StatePacer::Action::Done);
        CHECK(lever.Uses == 0);
    }

    SECTION("nothing is used until the mechanism has been loaded for the gap, so its script has set it up")
    {
        Mechanism lever{0, 2};
        pacer.Add(7, PoseAt(1), cStart);
        CHECK(pacer.Next(cStart, [&lever](uint32_t) { return lever.Look(); }).What == StatePacer::Action::None);
        CHECK(pacer.Next(cStart + kGap - std::chrono::milliseconds(1), [&lever](uint32_t) { return lever.Look(); }).What == StatePacer::Action::None);
        CHECK(pacer.Next(cStart + kGap, [&lever](uint32_t) { return lever.Look(); }).What == StatePacer::Action::Use);
    }

    SECTION("a mechanism that cannot reach the state comes round and stops where it started")
    {
        Mechanism lever{0, 2};
        pacer.Add(7, PoseAt(5), cStart);
        CHECK(Run(pacer, lever, cStart, 60) == StatePacer::Action::GaveUp);
        CHECK(lever.Position == 0);
        CHECK(lever.Uses == 2);
    }

    SECTION("one that never loads is dropped after kWait")
    {
        Mechanism lever{0, 2, Readiness::Wait};
        pacer.Add(7, PoseAt(1), cStart);
        CHECK(Run(pacer, lever, cStart, 40) == StatePacer::Action::Dropped);
        CHECK(lever.Uses == 0);
    }

    SECTION("a newer state for the same mechanism replaces the waiting one")
    {
        Mechanism pillar{0, 3};
        pacer.Add(7, PoseAt(1), cStart);
        pacer.Add(7, PoseAt(2), cStart);
        CHECK(pacer.Size() == 1);
        CHECK(Run(pacer, pillar, cStart, 30) == StatePacer::Action::Done);
        CHECK(pillar.Position == 2);
    }

    SECTION("while one is being set, the pacer says so")
    {
        Mechanism pillar{0, 3};
        pacer.Add(7, PoseAt(2), cStart);
        CHECK(pacer.Next(cStart, [&pillar](uint32_t) { return pillar.Look(); }).What == StatePacer::Action::None);
        CHECK_FALSE(pacer.Busy());
        CHECK(pacer.Next(cStart + kGap, [&pillar](uint32_t) { return pillar.Look(); }).What == StatePacer::Action::Use);
        CHECK(pacer.Busy());
        pacer.Clear();
        CHECK_FALSE(pacer.Busy());
        CHECK(pacer.Size() == 0);
    }
}

TEST_CASE("the game that used a mechanism reports where it came to rest, unless it sprang back", "[activation_end_state]")
{
    using Clock = std::chrono::steady_clock;
    const auto cStart = Clock::now();
    StateReporter reporter;
    std::vector<std::pair<uint32_t, Pose>> sent;
    const auto cSend = [&sent](uint32_t aFormId, Pose aPose)
    {
        sent.emplace_back(aFormId, std::move(aPose));
    };

    SECTION("a lever pulled down reports its new pose a gap after its use")
    {
        reporter.Used(7, PoseAt(0), cStart);
        reporter.Update(cStart + std::chrono::seconds(1), [](uint32_t) { return PoseAt(1); }, cSend);
        CHECK(sent.empty());
        reporter.Update(cStart + kGap, [](uint32_t) { return PoseAt(1); }, cSend);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].first == 7);
        CHECK(PoseMatches(sent[0].second, PoseAt(1)));
        CHECK(reporter.Size() == 0);
    }

    SECTION("a pull chain that springs back reports nothing, and keeps its uses replayed")
    {
        reporter.Used(7, PoseAt(0), cStart);
        reporter.Update(cStart + kGap, [](uint32_t) { return PoseAt(0); }, cSend);
        CHECK(sent.empty());
    }

    SECTION("a second use before the report waits a new gap and compares with the pose before the first")
    {
        reporter.Used(7, PoseAt(0), cStart);
        reporter.Used(7, PoseAt(1), cStart + std::chrono::seconds(2));
        reporter.Update(cStart + kGap, [](uint32_t) { return PoseAt(2); }, cSend);
        CHECK(sent.empty());
        reporter.Update(cStart + std::chrono::seconds(2) + kGap, [](uint32_t) { return PoseAt(2); }, cSend);
        CHECK(sent.size() == 1);
    }

    SECTION("one unloaded by then reports nothing")
    {
        reporter.Used(7, PoseAt(0), cStart);
        reporter.Update(cStart + kGap, [](uint32_t) { return Pose{}; }, cSend);
        CHECK(sent.empty());
        CHECK(reporter.Size() == 0);
    }

    SECTION("it keeps a bounded number waiting")
    {
        for (uint32_t i = 0; i < StateReporter::kMaxPending + 5; ++i)
            reporter.Used(i, PoseAt(0), cStart);
        CHECK(reporter.Size() == StateReporter::kMaxPending);
    }
}

TEST_CASE("the server sends a mechanism's end state instead of its uses", "[activation_end_state]")
{
    ActivationLedger<Place> ledger;
    constexpr uint64_t cPillar = 0x1717B;
    constexpr uint64_t cChain = 0x26858;

    SECTION("a late player gets the state once, and none of that pillar's uses")
    {
        ledger.Record(cPillar, {1}, {"Owner"});
        ledger.Record(cChain, {1}, {"Owner"});
        ledger.Record(cPillar, {1}, {"Owner"});
        CHECK(ledger.RecordState(cPillar, {1}, PoseAt(2), {"Owner"}));

        const auto cStates = ledger.CatchUpStates("Brother", InCell(1));
        REQUIRE(cStates.size() == 1);
        CHECK(cStates[0].Ref == cPillar);
        CHECK(PoseMatches(cStates[0].Pose, PoseAt(2)));
        CHECK(ledger.CatchUp("Brother", InCell(1)) == std::vector<uint64_t>{cChain});
        CHECK(ledger.CatchUpStates("Brother", InCell(1)).empty());
        CHECK(ledger.CatchUpStates("Owner", InCell(1)).empty());
    }

    SECTION("a newer state replaces the older one and goes again to those who had the old one")
    {
        CHECK(ledger.RecordState(cPillar, {1}, PoseAt(1), {"Owner"}));
        CHECK(ledger.CatchUpStates("Brother", InCell(1)).size() == 1);
        CHECK(ledger.RecordState(cPillar, {1}, PoseAt(2), {"Owner"}));
        CHECK(ledger.StateCount() == 1);
        const auto cStates = ledger.CatchUpStates("Brother", InCell(1));
        REQUIRE(cStates.size() == 1);
        CHECK(PoseMatches(cStates[0].Pose, PoseAt(2)));
    }

    SECTION("only states near the player, at most a message's worth at once")
    {
        for (uint64_t i = 0; i < ActivationLedger<Place>::kMaxStateCatchUp + 3; ++i)
            CHECK(ledger.RecordState(0x1000 + i, {1}, PoseAt(1), {"Owner"}));
        CHECK(ledger.RecordState(cPillar, {2}, PoseAt(1), {"Owner"}));
        CHECK(ledger.CatchUpStates("Brother", InCell(1)).size() == ActivationLedger<Place>::kMaxStateCatchUp);
        CHECK(ledger.CatchUpStates("Brother", InCell(1)).size() == 3);
        CHECK(ledger.CatchUpStates("Brother", InCell(2)).size() == 1);
    }

    SECTION("a pose no mechanism has is refused, and the store is bounded")
    {
        CHECK_FALSE(ledger.RecordState(cPillar, {1}, Pose{}, {"Owner"}));
        CHECK_FALSE(ledger.RecordState(cPillar, {1}, Pose(kMaxPoseValues + kPoseValuesPerObject, 0), {"Owner"}));
        for (uint64_t i = 0; i < ActivationLedger<Place>::kMaxStates + 10; ++i)
            ledger.RecordState(0x1000 + i, {1}, PoseAt(1), {"Owner"});
        CHECK(ledger.StateCount() == ActivationLedger<Place>::kMaxStates);
        CHECK_FALSE(ledger.HasState(0x1000));
    }

    SECTION("the last player out ends the session")
    {
        CHECK(ledger.RecordState(cPillar, {1}, PoseAt(1), {"Owner"}));
        ledger.Clear();
        CHECK(ledger.Size() == 0);
        CHECK(ledger.CatchUpStates("Brother", InCell(1)).empty());
    }
}

TEST_CASE("end states go over the network whole, and a pose no mechanism has is dropped", "[activation_end_state]")
{
    static_assert(ActivationState::kMaxPoseValues == kMaxPoseValues);

    SECTION("RequestActivationState")
    {
        Buffer buff(2000);
        RequestActivationState sendMessage;
        sendMessage.CellId = GameId(0, 0x1F2E);
        sendMessage.State.Id = GameId(0, 0x1717B);
        const Pose cPose = PoseAt(2);
        sendMessage.State.Pose.assign(cPose.begin(), cPose.end());
        sendMessage.State.Pose[3] = -1234;
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ClientMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == kRequestActivationState);
        const auto pRecv = CastUnique<RequestActivationState>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
        CHECK(pRecv->State.Pose[3] == -1234);
    }

    SECTION("NotifyActivationCatchUp carries uses and states")
    {
        Buffer buff(4000);
        NotifyActivationCatchUp sendMessage;
        sendMessage.Activations = {GameId(0, 0x26858)};
        ActivationState& state = sendMessage.States.emplace_back();
        state.Id = GameId(0, 0x1717B);
        const Pose cPose = PoseAt(1);
        state.Pose.assign(cPose.begin(), cPose.end());
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);
        Buffer::Reader reader(&buff);
        auto pMessage = ServerMessageFactory{}.Extract(reader);
        REQUIRE(pMessage);
        const auto pRecv = CastUnique<NotifyActivationCatchUp>(std::move(pMessage));
        REQUIRE(*pRecv == sendMessage);
    }

    SECTION("a pose longer than any mechanism's is dropped with its message")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kRequestActivationState, 8);
        GameId(0, 1).Serialize(writer);
        GameId(0, 2).Serialize(writer);
        Serialization::WriteVarInt(writer, ActivationState::kMaxPoseValues + 1);
        Buffer::Reader reader(&buff);
        REQUIRE(!ClientMessageFactory{}.Extract(reader));
    }

    SECTION("more states than a message holds are dropped")
    {
        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        writer.WriteBits(kNotifyActivationCatchUp, 8);
        Serialization::WriteVarInt(writer, 0);
        Serialization::WriteVarInt(writer, NotifyActivationCatchUp::kMaxStates + 1);
        Buffer::Reader reader(&buff);
        REQUIRE(!ServerMessageFactory{}.Extract(reader));
    }
}

TEST_CASE("An end state is taken only for a reference the same player used just now", "[activation_end_state]")
{
    using Ledger = ActivationLedger<Place>;
    const auto cStart = Ledger::Clock::now();
    Ledger ledger;
    ledger.NoteUse("Brother", 7, cStart);

    CHECK(ledger.UsedRecently("Brother", 7, cStart));
    CHECK(ledger.UsedRecently("Brother", 7, cStart + Ledger::kStateWindow));
    CHECK_FALSE(ledger.UsedRecently("Brother", 7, cStart + Ledger::kStateWindow + std::chrono::seconds(1)));
    // Another player, or another reference, is not that use.
    CHECK_FALSE(ledger.UsedRecently("Sister", 7, cStart));
    CHECK_FALSE(ledger.UsedRecently("Brother", 8, cStart));

    // Using it again renews the window, and the ledger's end of a session forgets it.
    ledger.NoteUse("Brother", 7, cStart + std::chrono::seconds(8));
    CHECK(ledger.UsedRecently("Brother", 7, cStart + std::chrono::seconds(15)));
    ledger.Clear();
    CHECK_FALSE(ledger.UsedRecently("Brother", 7, cStart + std::chrono::seconds(9)));
}

TEST_CASE("The recent uses are kept up to a limit, the oldest going first", "[activation_end_state]")
{
    using Ledger = ActivationLedger<Place>;
    const auto cStart = Ledger::Clock::now();
    Ledger ledger;
    for (uint64_t i = 0; i < Ledger::kMaxRecentUses + 1; ++i)
        ledger.NoteUse("Brother", i, cStart);

    CHECK_FALSE(ledger.UsedRecently("Brother", 0, cStart));
    CHECK(ledger.UsedRecently("Brother", 1, cStart));
    CHECK(ledger.UsedRecently("Brother", Ledger::kMaxRecentUses, cStart));
}
