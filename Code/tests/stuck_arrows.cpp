#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <DecodeGuard.h>
#include <LimbMask.h>
#include <StuckArrow.h>
#include <StuckArrowStore.h>
#include <Games/Skyrim/StuckArrowPlacement.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestStuckArrow.h>
#include <Messages/NotifyStuckArrow.h>
#include <Messages/RequestDismember.h>
#include <Messages/NotifyDismember.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/AssignCharacterResponse.h>

#include <glm/gtc/matrix_transform.hpp>

using TiltedPhoques::Buffer;

namespace
{
template <typename TMessage, typename TFactory> TiltedPhoques::UniquePtr<TMessage> RoundTrip(const TMessage& acMessage)
{
    Buffer buff(1000);
    Buffer::Writer writer(&buff);
    acMessage.Serialize(writer);
    Buffer::Reader reader(&buff);

    const TFactory factory;
    auto pMessage = factory.Extract(reader);
    if (!pMessage)
        return {nullptr};
    return TiltedPhoques::CastUnique<TMessage>(std::move(pMessage));
}

template <typename TMessage> void FillStuckArrow(TMessage& aMessage)
{
    aMessage.ServerId = 0x1234;
    aMessage.Ammo = GameId(0, 0x1F3EC);
    aMessage.Bone = "NPC Head [Head]";
    aMessage.Position = glm::vec3{1.5f, -2.25f, 3.f};
    aMessage.Rotation = glm::normalize(glm::quat{0.9f, 0.1f, -0.2f, 0.3f});
    aMessage.Scale = 0.75f;
    aMessage.Sequence = 3;
    aMessage.Removed = true;
}

bool Near(const glm::vec3& acA, const glm::vec3& acB, const float aWithin = 0.001f)
{
    return glm::distance(acA, acB) <= aWithin;
}

bool Near(const glm::mat3& acA, const glm::mat3& acB, const float aWithin = 0.001f)
{
    for (int i = 0; i < 3; ++i)
        if (!Near(acA[i], acB[i], aWithin))
            return false;
    return true;
}
} // namespace

TEST_CASE("a stuck arrow is carried whole by its request and its notify", "[stuck-arrows][encoding.packets]")
{
    RequestStuckArrow request;
    FillStuckArrow(request);
    const auto pRequest = RoundTrip<RequestStuckArrow, ClientMessageFactory>(request);
    REQUIRE(pRequest);
    REQUIRE(pRequest->GetOpcode() == kRequestStuckArrow);
    REQUIRE(*pRequest == request);

    NotifyStuckArrow notify;
    FillStuckArrow(notify);
    const auto pNotify = RoundTrip<NotifyStuckArrow, ServerMessageFactory>(notify);
    REQUIRE(pNotify);
    REQUIRE(pNotify->GetOpcode() == kNotifyStuckArrow);
    REQUIRE(*pNotify == notify);
    REQUIRE(pNotify->Bone == "NPC Head [Head]");
}

TEST_CASE("a bone name the engine has no skeleton for is dropped with the message", "[stuck-arrows][encoding.packets]")
{
    const std::string cLongest(StuckArrow::kMaxBoneNameLength, 'a');
    const std::string cTooLong(StuckArrow::kMaxBoneNameLength + 1, 'a');

    RequestStuckArrow request;
    FillStuckArrow(request);
    request.Bone = cLongest.c_str();
    REQUIRE((RoundTrip<RequestStuckArrow, ClientMessageFactory>(request)));
    request.Bone = cTooLong.c_str();
    REQUIRE_FALSE((RoundTrip<RequestStuckArrow, ClientMessageFactory>(request)));

    NotifyStuckArrow notify;
    FillStuckArrow(notify);
    notify.Bone = cTooLong.c_str();
    REQUIRE_FALSE((RoundTrip<NotifyStuckArrow, ServerMessageFactory>(notify)));
    DecodeGuard::t_failed = false;
}

TEST_CASE("a dismember carries the actor, the limbs and the last limb hit", "[stuck-arrows][encoding.packets]")
{
    RequestDismember request;
    request.ServerId = 0x2A;
    request.Limbs = 0x8001;
    request.LastHitLimb = 1;
    const auto pRequest = RoundTrip<RequestDismember, ClientMessageFactory>(request);
    REQUIRE(pRequest);
    REQUIRE(pRequest->GetOpcode() == kRequestDismember);
    REQUIRE(*pRequest == request);
    REQUIRE(pRequest->Limbs == 0x8001);

    NotifyDismember notify;
    notify.ServerId = 0x2A;
    notify.Limbs = 0x0002;
    notify.LastHitLimb = 255;
    const auto pNotify = RoundTrip<NotifyDismember, ServerMessageFactory>(notify);
    REQUIRE(pNotify);
    REQUIRE(pNotify->GetOpcode() == kNotifyDismember);
    REQUIRE(*pNotify == notify);
}

TEST_CASE("the limbs the server keeps go with a character's spawn and assignment", "[stuck-arrows][encoding.packets]")
{
    // Every field around the limbs is set, so a limb field read at another place than it was written shows in them.
    for (const uint16_t cLimbs : {uint16_t(0), uint16_t(0x0101), uint16_t(0xFFFF)})
    {
        CharacterSpawnRequest spawn;
        spawn.ServerId = 0x1A;
        spawn.FormId = GameId(1, 0x1234);
        spawn.BaseId = GameId(2, 0x5678);
        spawn.CellId = GameId(3, 0x9ABC);
        spawn.LeveledNpcPickId = GameId(4, 0xDEF0);
        spawn.ChangeFlags = 0x80000001;
        spawn.AppearanceBuffer = "appearance";
        spawn.PlayerId = 7;
        spawn.OwnershipEpoch = 300;
        spawn.FollowerOfPlayerId = 5;
        spawn.IsDead = true;
        spawn.IsPlayer = false;
        spawn.IsWeaponDrawn = true;
        spawn.IsPlayerSummon = false;
        spawn.IsDragon = true;
        spawn.RevivedSinceDeath = true;
        spawn.DismemberedLimbs = cLimbs;
        const auto pSpawned = RoundTrip<CharacterSpawnRequest, ServerMessageFactory>(spawn);
        REQUIRE(pSpawned);
        REQUIRE(*pSpawned == spawn);
        REQUIRE(pSpawned->RevivedSinceDeath);
        REQUIRE(pSpawned->FollowerOfPlayerId == 5);
        REQUIRE(pSpawned->LeveledNpcPickId == GameId(4, 0xDEF0));
        REQUIRE(pSpawned->DismemberedLimbs == cLimbs);

        AssignCharacterResponse answer;
        answer.Cookie = 9;
        answer.ServerId = 0x100014;
        answer.PlayerId = 3;
        answer.CellId = GameId(1, 0x11);
        answer.WorldSpaceId = GameId(2, 0x22);
        answer.BaseId = GameId(3, 0x33);
        answer.LeveledNpcPickId = GameId(4, 0x44);
        answer.OwnershipEpoch = 41;
        answer.FollowerOfPlayerId = 6;
        answer.Owner = true;
        answer.IsDead = true;
        answer.IsWeaponDrawn = false;
        answer.RevivedSinceDeath = true;
        answer.DismemberedLimbs = cLimbs;
        const auto pAnswered = RoundTrip<AssignCharacterResponse, ServerMessageFactory>(answer);
        REQUIRE(pAnswered);
        REQUIRE(*pAnswered == answer);
        // Written after the limbs: read back right only if the limbs took exactly their own place.
        REQUIRE(pAnswered->LeveledNpcPickId == GameId(4, 0x44));
        REQUIRE(pAnswered->FollowerOfPlayerId == 6);
        REQUIRE(pAnswered->RevivedSinceDeath);
        REQUIRE(pAnswered->DismemberedLimbs == cLimbs);
    }
}

TEST_CASE("a limb cut off stays cut off as more are reported", "[stuck-arrows]")
{
    REQUIRE(LimbMask::Merge(0, 0) == 0);
    REQUIRE(LimbMask::Merge(0, 0x0001) == 0x0001);
    REQUIRE(LimbMask::Merge(0x0001, 0x0004) == 0x0005);
    // Told again, or told a part of what is kept.
    REQUIRE(LimbMask::Merge(0x0005, 0x0004) == 0x0005);
    REQUIRE(LimbMask::Merge(0x0005, 0) == 0x0005);
    REQUIRE(LimbMask::Merge(0x00FF, 0x8000) == 0x80FF);
}

TEST_CASE("an arrow in a bone is placed back where the owner's game had it", "[stuck-arrows]")
{
    using StuckArrowPlacement::Frame;

    // A bone turned and scaled, as a scaled actor's skeleton is.
    Frame bone;
    bone.Rotation = glm::mat3(glm::rotate(glm::rotate(glm::mat4(1.f), 0.8f, glm::vec3{0.f, 0.f, 1.f}), -0.4f, glm::vec3{1.f, 0.f, 0.f}));
    bone.Position = glm::vec3{120.f, -45.f, 300.f};
    bone.Scale = 1.25f;

    Frame arrow;
    arrow.Rotation = glm::mat3(glm::rotate(glm::mat4(1.f), 1.9f, glm::normalize(glm::vec3{1.f, 2.f, 3.f})));
    arrow.Position = glm::vec3{131.f, -30.f, 312.f};
    arrow.Scale = 0.9f;

    const Frame cLocal = StuckArrowPlacement::ToBoneLocal(bone, arrow);
    const Frame cBack = StuckArrowPlacement::FromBoneLocal(bone, cLocal);
    REQUIRE(Near(cBack.Position, arrow.Position));
    REQUIRE(Near(cBack.Rotation, arrow.Rotation));
    REQUIRE(cBack.Scale == Approx(arrow.Scale));

    // The convention itself: the offset is before the bone's scale, so it is not the world offset.
    REQUIRE(Near(cLocal.Position * bone.Scale, glm::transpose(bone.Rotation) * (arrow.Position - bone.Position)));
    REQUIRE(cLocal.Scale == Approx(arrow.Scale / bone.Scale));

    // The other direction: a place sent for the bone, put on another game's bone, and read back there.
    Frame otherBone;
    otherBone.Rotation = glm::mat3(glm::rotate(glm::mat4(1.f), -1.1f, glm::vec3{0.f, 1.f, 0.f}));
    otherBone.Position = glm::vec3{-500.f, 80.f, 64.f};
    otherBone.Scale = 0.8f;
    const Frame cThere = StuckArrowPlacement::FromBoneLocal(otherBone, cLocal);
    const Frame cAgain = StuckArrowPlacement::ToBoneLocal(otherBone, cThere);
    REQUIRE(Near(cAgain.Position, cLocal.Position));
    REQUIRE(Near(cAgain.Rotation, cLocal.Rotation));
    REQUIRE(cAgain.Scale == Approx(cLocal.Scale));
}

TEST_CASE("a rotation goes on the wire as a quaternion and comes back", "[stuck-arrows]")
{
    const glm::mat3 cRotation = glm::mat3(glm::rotate(glm::mat4(1.f), 2.4f, glm::normalize(glm::vec3{-1.f, 0.5f, 2.f})));
    REQUIRE(Near(StuckArrowPlacement::ToMatrix(StuckArrowPlacement::ToQuaternion(cRotation)), cRotation));
}

TEST_CASE("an actor keeps at most three stuck arrows, the oldest dropped", "[stuck-arrows]")
{
    StuckArrowStore store;
    REQUIRE(store.Arrows(7) == nullptr);

    for (uint32_t sequence = 1; sequence <= 5; ++sequence)
    {
        StuckArrowStore::Arrow arrow;
        arrow.Sequence = sequence;
        arrow.Bone = "NPC Spine2 [Spn2]";
        store.AddArrow(7, arrow);
    }
    const auto* pArrows = store.Arrows(7);
    REQUIRE(pArrows);
    REQUIRE(pArrows->size() == StuckArrowStore::kMaxArrowsPerActor);
    REQUIRE(pArrows->front().Sequence == 3);
    REQUIRE(pArrows->back().Sequence == 5);
    // Another actor's are its own.
    REQUIRE(store.Arrows(8) == nullptr);
}

TEST_CASE("the limbs told for an actor add up and the spawn data sets them", "[stuck-arrows]")
{
    StuckArrowStore store;
    REQUIRE(store.Limbs(7) == 0);
    store.AddLimbs(7, 0x0001);
    store.AddLimbs(7, 0x0004);
    store.AddLimbs(7, 0);
    REQUIRE(store.Limbs(7) == 0x0005);
    REQUIRE(store.Limbs(8) == 0);

    // The server's own count is the whole truth: a later spawn of a revived actor has none.
    store.SetLimbs(7, 0x0007);
    REQUIRE(store.Limbs(7) == 0x0007);
    store.SetLimbs(7, 0);
    REQUIRE(store.Limbs(7) == 0);

    store.AddLimbs(9, 1);
    store.Clear();
    REQUIRE(store.Limbs(9) == 0);
}

TEST_CASE("a head told cut off waits for the copy, and is given up after a while", "[stuck-arrows]")
{
    StuckArrowStore store;
    REQUIRE_FALSE(store.HasLimbsToCutOff());

    store.AddLimbs(7, StuckArrowStore::kHeadBit, 10.0);
    REQUIRE(store.HasLimbsToCutOff());
    auto toCut = store.LimbsToCutOff();
    REQUIRE(toCut.size() == 1);
    REQUIRE(toCut[0].first == 7);
    REQUIRE(toCut[0].second == StuckArrowStore::kHeadBit);

    // Cut off the copy: told again, it is not cut a second time.
    store.LimbsCutOff(7);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 11.0);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    REQUIRE(store.Limbs(7) == StuckArrowStore::kHeadBit);

    // A new limb waits on its own, from when it was told.
    store.AddLimbs(7, 0x0004, 12.0);
    toCut = store.LimbsToCutOff();
    REQUIRE(toCut.size() == 1);
    REQUIRE(toCut[0].second == 0x0004);
    REQUIRE(store.DropStaleLimbs(12.0 + StuckArrowStore::kLimbWait).empty());
    const auto dropped = store.DropStaleLimbs(12.1 + StuckArrowStore::kLimbWait);
    REQUIRE(dropped.size() == 1);
    REQUIRE(dropped[0] == 7);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    // Given up here, still kept: a later spawn of the body carries it again.
    REQUIRE(store.Limbs(7) == (StuckArrowStore::kHeadBit | 0x0004));
}

TEST_CASE("a spawn makes a new copy with all its limbs still to cut, and a revive clears them", "[stuck-arrows]")
{
    StuckArrowStore store;
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 1.0);
    store.LimbsCutOff(7);

    store.SetLimbs(7, StuckArrowStore::kHeadBit, 50.0);
    auto toCut = store.LimbsToCutOff();
    REQUIRE(toCut.size() == 1);
    REQUIRE(toCut[0].second == StuckArrowStore::kHeadBit);
    // Its wait starts at the spawn.
    REQUIRE(store.DropStaleLimbs(50.0 + StuckArrowStore::kLimbWait).empty());

    store.SetLimbs(7, 0, 51.0);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    REQUIRE(store.Limbs(7) == 0);

    store.AddLimbs(8, StuckArrowStore::kHeadBit, 1.0);
    store.Clear();
    REQUIRE_FALSE(store.HasLimbsToCutOff());
}

TEST_CASE("a revive drops the limbs waiting for the copy, and a later beheading is cut again", "[stuck-arrows]")
{
    StuckArrowStore store;
    StuckArrowStore::Arrow arrow;
    arrow.Sequence = 1;
    store.AddArrow(7, arrow);
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 1.0);
    REQUIRE(store.HasLimbsToCutOff());

    // Brought back before its copy here was dead and loaded: nothing is cut off it.
    store.Forget(7);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    REQUIRE(store.LimbsToCutOff().empty());
    REQUIRE(store.Limbs(7) == 0);
    REQUIRE_FALSE(store.HasWaitingArrows());
    REQUIRE(store.Arrows(7) == nullptr);

    // Beheaded once, cut off, brought back and beheaded again: the second is not taken for the first.
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 2.0);
    store.LimbsCutOff(7);
    store.Forget(7);
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 3.0);
    const auto toCut = store.LimbsToCutOff();
    REQUIRE(toCut.size() == 1);
    REQUIRE(toCut[0].first == 7);
    REQUIRE(toCut[0].second == StuckArrowStore::kHeadBit);
}

TEST_CASE("an actor whose copy or server id is gone is forgotten, and no other", "[stuck-arrows]")
{
    StuckArrowStore store;
    store.AddLimbs(7, StuckArrowStore::kHeadBit, 1.0);
    store.AddLimbs(8, StuckArrowStore::kHeadBit, 1.0);
    StuckArrowStore::Arrow arrow;
    arrow.Sequence = 1;
    store.AddArrow(7, arrow);
    store.AddArrow(8, arrow);

    store.Forget(7);
    REQUIRE(store.Limbs(7) == 0);
    REQUIRE(store.Arrows(7) == nullptr);
    auto toCut = store.LimbsToCutOff();
    REQUIRE(toCut.size() == 1);
    REQUIRE(toCut[0].first == 8);
    REQUIRE(store.Limbs(8) == StuckArrowStore::kHeadBit);
    REQUIRE(store.Arrows(8)->size() == 1);
    // Its own wait still runs.
    REQUIRE(store.DropStaleLimbs(2.0 + StuckArrowStore::kLimbWait) == std::vector<uint32_t>{8});

    // The id given to another actor starts with nothing; its spawn data says what it has lost.
    store.Forget(8);
    store.SetLimbs(8, 0, 5.0);
    REQUIRE_FALSE(store.HasLimbsToCutOff());
    REQUIRE(store.Limbs(8) == 0);
    // Forgetting an actor never told of changes nothing.
    store.Forget(9);
    REQUIRE_FALSE(store.HasWaitingArrows());
}

TEST_CASE("an arrow told waits until it is placed or given up", "[stuck-arrows]")
{
    StuckArrowStore store;
    REQUIRE_FALSE(store.HasWaitingArrows());

    for (uint32_t sequence = 1; sequence <= 2; ++sequence)
    {
        StuckArrowStore::Arrow arrow;
        arrow.Sequence = sequence;
        arrow.Received = 10.0 * sequence;
        store.AddArrow(7, arrow);
    }
    REQUIRE(store.HasWaitingArrows());
    auto waiting = store.WaitingArrows();
    REQUIRE(waiting.size() == 2);
    REQUIRE(waiting[0].first == 7);
    REQUIRE(waiting[0].second.Sequence == 1);
    REQUIRE(waiting[1].second.Received == 20.0);

    store.ArrowDone(7, 1);
    waiting = store.WaitingArrows();
    REQUIRE(waiting.size() == 1);
    REQUIRE(waiting[0].second.Sequence == 2);
    // An actor or arrow not kept changes nothing.
    store.ArrowDone(8, 2);
    store.ArrowDone(7, 9);
    REQUIRE(store.WaitingArrows().size() == 1);

    store.ArrowDone(7, 2);
    REQUIRE_FALSE(store.HasWaitingArrows());
    // Done, still kept: the cap counts it.
    REQUIRE(store.Arrows(7)->size() == 2);
}

namespace
{
using Tracker = StuckArrowTracker;
using Counts = std::unordered_map<uint32_t, int64_t>;

constexpr uint32_t kIron = 0x100;
constexpr uint32_t kSteel = 0x200;

std::vector<uintptr_t> ObjectsOf(const std::vector<Tracker::Entry>& acEntries)
{
    std::vector<uintptr_t> objects;
    for (const auto& cEntry : acEntries)
        objects.push_back(cEntry.Object);
    return objects;
}

std::vector<uintptr_t> Sorted(std::vector<uintptr_t> aObjects)
{
    std::sort(aObjects.begin(), aObjects.end());
    return aObjects;
}
} // namespace

TEST_CASE("a body's arrows are hidden one for each of their ammunition taken, and all once none is left", "[stuck-arrows]")
{
    Tracker tracker;
    const Tracker::Life cLife{7, 70, 0x9000};
    const Tracker::Entry cA{0x1000, 1, kIron};
    const Tracker::Entry cB{0x2000, 2, kIron};
    const Tracker::Entry cC{0x3000, 3, kIron};
    const Tracker::Entry cSteel{0x4000, 4, kSteel};

    tracker.Update(cLife, {cC, cA, cB, cSteel}, 1.0);
    REQUIRE(tracker.Loot(7, {{kIron, 5}, {kSteel, 1}}, true, 1.0).empty());

    // Nothing taken, nothing hidden.
    tracker.Update(cLife, {cC, cA, cB, cSteel}, 2.0);
    REQUIRE(tracker.Loot(7, {{kIron, 5}, {kSteel, 1}}, true, 2.0).empty());

    // One taken, the oldest of its kind goes; then one more.
    tracker.Update(cLife, {cC, cA, cB, cSteel}, 3.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 4}, {kSteel, 1}}, true, 3.0)) == std::vector<uintptr_t>{cA.Object});
    tracker.Update(cLife, {cC, cA, cB, cSteel}, 4.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 3}, {kSteel, 1}}, true, 4.0)) == std::vector<uintptr_t>{cB.Object});

    // Put back, the hidden stay hidden, and none is shown again.
    tracker.Update(cLife, {cC, cA, cB, cSteel}, 5.0);
    REQUIRE(tracker.Loot(7, {{kIron, 6}, {kSteel, 1}}, true, 5.0).empty());
    REQUIRE(Sorted(tracker.HiddenOf(7)) == std::vector<uintptr_t>{cA.Object, cB.Object});

    // None left hides them all, however few were taken; the other kind is left.
    tracker.Update(cLife, {cC, cA, cB, cSteel}, 6.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kSteel, 1}}, true, 6.0)) == std::vector<uintptr_t>{cC.Object});
    REQUIRE(Sorted(tracker.HiddenOf(7)) == std::vector<uintptr_t>{cA.Object, cB.Object, cC.Object});

    // A body that never had any of a kind has nothing to take.
    Tracker none;
    none.Update({8, 80, 0x9100}, {cA}, 1.0);
    REQUIRE(none.Loot(8, {}, true, 1.0).empty());
    none.Update({8, 80, 0x9100}, {cA}, 2.0);
    REQUIRE(none.Loot(8, {}, true, 2.0).empty());
}

TEST_CASE("ammunition a living actor uses hides none of its arrows", "[stuck-arrows]")
{
    Tracker tracker;
    const Tracker::Life cLife{7, 70, 0x9000};
    const Tracker::Entry cA{0x1000, 1, kIron};

    tracker.Update(cLife, {cA}, 1.0);
    tracker.Loot(7, {{kIron, 5}}, false, 1.0);
    tracker.Update(cLife, {cA}, 2.0);
    REQUIRE(tracker.Loot(7, {{kIron, 2}}, false, 2.0).empty());
    // Dead, it is compared with what it had when it died.
    tracker.Update(cLife, {cA}, 3.0);
    REQUIRE(tracker.Loot(7, {{kIron, 2}}, true, 3.0).empty());
    tracker.Update(cLife, {cA}, 4.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 1}}, true, 4.0)) == std::vector<uintptr_t>{cA.Object});
}

TEST_CASE("a body looted right after its arrow is first seen, or just before, loses the arrow", "[stuck-arrows]")
{
    const Tracker::Entry cA{0x1000, 1, kIron};
    const Tracker::Entry cB{0x2000, 2, kIron};

    SECTION("looted on the read after the arrow was first seen")
    {
        Tracker tracker;
        const Tracker::Life cLife{7, 70, 0x9000};
        // Watched before the shot, alive, with no arrow: its ammunition is not read.
        tracker.Update(cLife, {}, 1.0);
        // The killing arrow is first seen with the body; its ammunition is read with it.
        tracker.Update(cLife, {cA}, 1.1);
        REQUIRE(tracker.Loot(7, {{kIron, 1}}, true, 1.1).empty());
        // Taken at once.
        tracker.Update(cLife, {cA}, 1.2);
        REQUIRE(ObjectsOf(tracker.Loot(7, {}, true, 1.2)) == std::vector<uintptr_t>{cA.Object});
    }

    SECTION("looted, here or through the inventory sync, before the arrow was first seen")
    {
        Tracker tracker;
        const Tracker::Life cLife{7, 70, 0x9000};
        // The body is watched with an arrow of another shot in it, and its count of the ammunition read.
        tracker.Update(cLife, {cA}, 1.0);
        tracker.Loot(7, {{kIron, 3}}, true, 1.0);
        // Two taken: the arrow there goes, and one more is owed.
        tracker.Update(cLife, {cA}, 2.0);
        REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 1}}, true, 2.0)) == std::vector<uintptr_t>{cA.Object});
        // The replayed shot's arrow is first seen just after: it goes too.
        tracker.Update(cLife, {cA, cB}, 2.5);
        REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 1}}, true, 2.5)) == std::vector<uintptr_t>{cB.Object});
    }

    SECTION("emptied before the arrow was first seen")
    {
        Tracker tracker;
        const Tracker::Life cLife{7, 70, 0x9000};
        tracker.Update(cLife, {}, 1.0);
        tracker.Loot(7, {{kIron, 2}}, true, 1.0);
        tracker.Update(cLife, {}, 1.1);
        REQUIRE(tracker.Loot(7, {}, true, 1.1).empty());
        tracker.Update(cLife, {cA}, 1.3);
        REQUIRE(ObjectsOf(tracker.Loot(7, {}, true, 1.3)) == std::vector<uintptr_t>{cA.Object});
    }

    SECTION("an arrow shot into a body looted a while before stays")
    {
        Tracker tracker;
        const Tracker::Life cLife{7, 70, 0x9000};
        tracker.Update(cLife, {}, 1.0);
        tracker.Loot(7, {{kIron, 2}}, true, 1.0);
        tracker.Update(cLife, {}, 1.1);
        tracker.Loot(7, {}, true, 1.1);
        tracker.Update(cLife, {cA}, 1.1 + Tracker::kRecentLoot + 0.5);
        REQUIRE(tracker.Loot(7, {}, true, 1.1 + Tracker::kRecentLoot + 0.5).empty());
    }

    SECTION("a read whose ammunition was not read is not compared with")
    {
        Tracker tracker;
        const Tracker::Life cLife{7, 70, 0x9000};
        tracker.Update(cLife, {cA}, 1.0);
        tracker.Loot(7, {{kIron, 4}}, false, 1.0);
        tracker.Update(cLife, {}, 2.0);
        // Gone and back without its ammunition read in between: what it had then is no baseline.
        tracker.Update(cLife, {cB}, 3.0);
        REQUIRE(tracker.Loot(7, {{kIron, 1}}, true, 3.0).empty());
    }
}

TEST_CASE("what is decided for an arrow stays with it as the engine reorders, drops and adds arrows", "[stuck-arrows]")
{
    Tracker tracker;
    const Tracker::Life cLife{7, 70, 0x9000};
    const Tracker::Entry cA{0x1000, 1, kIron};
    const Tracker::Entry cB{0x2000, 2, kIron};
    const Tracker::Entry cC{0x3000, 3, kIron};
    const Tracker::Entry cD{0x4000, 4, kIron};

    tracker.Update(cLife, {cA, cB, cC}, 1.0);
    tracker.Loot(7, {{kIron, 3}}, true, 1.0);
    tracker.Update(cLife, {cA, cB, cC}, 2.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 2}}, true, 2.0)) == std::vector<uintptr_t>{cA.Object});

    // Listed in another order: the same one is hidden, and no other.
    tracker.Update(cLife, {cC, cB, cA}, 3.0);
    REQUIRE(tracker.Loot(7, {{kIron, 2}}, true, 3.0).empty());
    REQUIRE(tracker.HiddenOf(7) == std::vector<uintptr_t>{cA.Object});

    // The engine drops the hidden one: nothing hidden passes to the others.
    tracker.Update(cLife, {cB, cC}, 4.0);
    REQUIRE(tracker.Loot(7, {{kIron, 2}}, true, 4.0).empty());
    REQUIRE(tracker.HiddenOf(7).empty());

    // A new one comes in: it is shown, and is the newest.
    const auto cRead = tracker.Update(cLife, {cD, cB, cC}, 5.0);
    REQUIRE(ObjectsOf(cRead.Fresh) == std::vector<uintptr_t>{cD.Object});
    REQUIRE(tracker.Loot(7, {{kIron, 2}}, true, 5.0).empty());
    REQUIRE(tracker.HiddenOf(7).empty());
    tracker.Update(cLife, {cD, cB, cC}, 6.0);
    REQUIRE(ObjectsOf(tracker.Loot(7, {{kIron, 1}}, true, 6.0)) == std::vector<uintptr_t>{cB.Object});

    // The same 3D made again for another arrow is a new one, with nothing decided for it.
    const Tracker::Entry cReused{cB.Object, 9, kIron};
    tracker.Update(cLife, {cD, cReused, cC}, 7.0);
    REQUIRE(tracker.Loot(7, {{kIron, 1}}, true, 7.0).empty());
    REQUIRE(tracker.HiddenOf(7).empty());
    const auto* pReused = tracker.Find(7, cReused);
    REQUIRE(pReused != nullptr);
    REQUIRE(pReused->FirstSeen == 7.0);
    REQUIRE_FALSE(pReused->Hidden);

    // Claimed once, it stays claimed while listed, wherever it is listed.
    tracker.Claim(7, cC);
    tracker.Update(cLife, {cC, cD, cReused}, 8.0);
    REQUIRE(tracker.Find(7, cC)->Claimed);
    REQUIRE_FALSE(tracker.Find(7, cD)->Claimed);

    // Oldest first, as first seen.
    const auto cArrows = tracker.ArrowsOf(7);
    REQUIRE(cArrows.size() == 3);
    REQUIRE(cArrows[0].Id.Object == cC.Object);
    REQUIRE(cArrows[1].Id.Object == cD.Object);
    REQUIRE(cArrows[2].Id.Is(cReused));
}

TEST_CASE("an actor's stuck arrows are new only once, and not those there before it was watched", "[stuck-arrows]")
{
    Tracker tracker;
    const Tracker::Life cLife{7, 70, 0x9000};
    const Tracker::Entry cOld{0x1000, 5, kIron};
    const Tracker::Entry cNew{0x2000, 6, kIron};

    auto read = tracker.Update(cLife, {cOld}, 1.0);
    REQUIRE(read.Fresh.empty());
    REQUIRE(read.NewLife);
    REQUIRE(tracker.Find(7, cOld)->FirstSeen == Tracker::kBeforeWatched);

    read = tracker.Update(cLife, {cOld, cNew}, 2.0);
    REQUIRE_FALSE(read.NewLife);
    REQUIRE(ObjectsOf(read.Fresh) == std::vector<uintptr_t>{cNew.Object});
    REQUIRE(tracker.Find(7, cNew)->FirstSeen == 2.0);
    REQUIRE(tracker.Update(cLife, {cNew, cOld}, 3.0).Fresh.empty());
    REQUIRE(tracker.Find(7, cNew)->FirstSeen == 2.0);

    // Another actor is watched on its own.
    REQUIRE(tracker.Update({8, 80, 0x9100}, {cNew}, 5.0).Fresh.empty());
    REQUIRE(tracker.Size() == 2);

    // An actor not read for a while is forgotten, and watched afresh after.
    tracker.Prune(10.0, 7.5);
    REQUIRE(tracker.Size() == 2);
    tracker.Update({8, 80, 0x9100}, {cNew}, 12.0);
    tracker.Prune(13.0, 7.5);
    REQUIRE(tracker.Size() == 1);
    REQUIRE(tracker.Update(cLife, {cOld, Tracker::Entry{0x3000, 10, kIron}}, 13.0).Fresh.empty());

    tracker.Clear();
    REQUIRE(tracker.Size() == 0);
}

TEST_CASE("a new 3D or another server id for an actor starts its arrows afresh", "[stuck-arrows]")
{
    Tracker tracker;
    const Tracker::Entry cA{0x1000, 1, kIron};
    const Tracker::Entry cB{0x2000, 2, kIron};

    tracker.Update({7, 70, 0x9000}, {cA}, 1.0);
    tracker.Loot(7, {{kIron, 2}}, true, 1.0);
    tracker.Update({7, 70, 0x9000}, {cA, cB}, 2.0);
    REQUIRE(tracker.Loot(7, {{kIron, 1}}, true, 2.0).size() == 1);
    const uint64_t cFirstLife = tracker.LifeOf(7);
    REQUIRE(cFirstLife != 0);

    // The 3D is made again: nothing hidden or seen is kept, and its arrows were there before it was watched again.
    auto read = tracker.Update({7, 70, 0x9500}, {cA, cB}, 3.0);
    REQUIRE(read.NewLife);
    REQUIRE(read.Fresh.empty());
    REQUIRE(tracker.LifeOf(7) != cFirstLife);
    REQUIRE(tracker.HiddenOf(7).empty());
    REQUIRE(tracker.Find(7, cA)->FirstSeen == Tracker::kBeforeWatched);
    // Its ammunition before is not compared with.
    REQUIRE(tracker.Loot(7, {}, true, 3.0).empty());

    // Another server id for the same form id is another actor.
    const uint64_t cSecondLife = tracker.LifeOf(7);
    read = tracker.Update({7, 71, 0x9500}, {cA, cB}, 4.0);
    REQUIRE(read.NewLife);
    REQUIRE(tracker.LifeOf(7) != cSecondLife);

    // A retired actor is not watched, and one retired by its server id takes none other with it.
    tracker.Update({8, 80, 0x9100}, {cA}, 4.0);
    tracker.Retire(7);
    REQUIRE(tracker.LifeOf(7) == 0);
    tracker.RetireServer(81);
    REQUIRE(tracker.LifeOf(8) != 0);
    tracker.RetireServer(80);
    REQUIRE(tracker.Size() == 0);
}

TEST_CASE("a beheading waiting to be sent is dropped once its actor changed hands, came back or has its head again", "[stuck-arrows]")
{
    using Beheadings = StuckArrowBeheadings;
    Beheadings beheadings;
    const Beheadings::Current cAsCut{true, 70, 3, true, true};

    // Not run here as of the last frame: not kept.
    REQUIRE_FALSE(beheadings.Push(7));
    beheadings.SetOwned({{7, {70, 3}}, {8, {80, 1}}});

    SECTION("sent when nothing changed")
    {
        REQUIRE(beheadings.Push(7));
        const auto cTaken = beheadings.Take();
        REQUIRE(cTaken.size() == 1);
        REQUIRE(beheadings.IsCurrent(cTaken[0], cAsCut));
        REQUIRE(beheadings.Waiting() == 0);
    }

    SECTION("not once it is run by another game, or by this one under another ownership or server id")
    {
        REQUIRE(beheadings.Push(7));
        const auto cTaken = beheadings.Take();
        REQUIRE_FALSE(beheadings.IsCurrent(cTaken[0], {false, 0, 0, false}));
        REQUIRE_FALSE(beheadings.IsCurrent(cTaken[0], {true, 70, 4, true}));
        REQUIRE_FALSE(beheadings.IsCurrent(cTaken[0], {true, 71, 3, true}));
    }

    SECTION("not once it was brought back or its id let go, even cut again in a later life")
    {
        REQUIRE(beheadings.Push(7));
        beheadings.NewLife(70);
        const auto cTaken = beheadings.Take();
        REQUIRE_FALSE(beheadings.IsCurrent(cTaken[0], cAsCut));
        // Another actor's life is its own.
        REQUIRE(beheadings.Push(8));
        beheadings.NewLife(70);
        REQUIRE(beheadings.IsCurrent(beheadings.Take()[0], {true, 80, 1, true, true}));
        // Cut in the new life, it is sent.
        REQUIRE(beheadings.Push(7));
        REQUIRE(beheadings.IsCurrent(beheadings.Take()[0], cAsCut));
    }

    SECTION("not once it has its head again")
    {
        REQUIRE(beheadings.Push(7));
        REQUIRE_FALSE(beheadings.IsCurrent(beheadings.Take()[0], {true, 70, 3, false}));
    }

    SECTION("a bounded number wait, and a disconnect drops them all")
    {
        for (size_t i = 0; i < Beheadings::kMaxWaiting; ++i)
            REQUIRE(beheadings.Push(7));
        REQUIRE_FALSE(beheadings.Push(7));
        beheadings.Clear();
        REQUIRE(beheadings.Waiting() == 0);
        REQUIRE_FALSE(beheadings.Push(7));
    }
}

TEST_CASE("validated native ammo removal needs no pre-loot corpse inventory sample", "[stuck-arrows]")
{
    StuckArrowTracker tracker;
    const StuckArrowTracker::Life life{7, 70, 700};
    const StuckArrowTracker::Entry killingArrow{1, 100, 500};
    const StuckArrowTracker::Entry otherAmmo{2, 101, 600};

    SECTION("first corpse read follows removal, with no prior inventory read")
    {
        tracker.Update(life, {}, 1.0); // living, no arrows; production deliberately skips inventory
        tracker.Update(life, {killingArrow, otherAmmo}, 1.1);
        tracker.Removed(7, 500, 1, 0, 1.05); // from the real native RemoveItem call, before that read
        tracker.Loot(7, {}, true, 1.1);
        REQUIRE(tracker.Find(7, killingArrow)->Hidden);
        REQUIRE_FALSE(tracker.Find(7, otherAmmo)->Hidden);
    }
    SECTION("inventory replication precedes even the recipient's first attached-arrow observation")
    {
        tracker.Update(life, {}, 2.0);
        tracker.Removed(7, 500, 1, 0, 2.0);
        tracker.Loot(7, {}, true, 2.0);
        tracker.Update(life, {killingArrow}, 2.2);
        tracker.Loot(7, {}, true, 2.2);
        REQUIRE(tracker.Find(7, killingArrow)->Hidden);
    }
    SECTION("the poll does not count the validated removal twice")
    {
        const StuckArrowTracker::Entry second{3, 102, 500};
        tracker.Update(life, {killingArrow, second}, 3.0);
        tracker.Loot(7, {{500, 2}}, true, 3.0);
        tracker.Update(life, {killingArrow, second}, 3.1);
        tracker.Removed(7, 500, 2, 1, 3.05);
        tracker.Loot(7, {{500, 1}}, true, 3.1);
        REQUIRE(tracker.Find(7, killingArrow)->Hidden);
        REQUIRE_FALSE(tracker.Find(7, second)->Hidden);
    }
    SECTION("unsuccessful removal and an increase hide nothing")
    {
        tracker.Update(life, {killingArrow}, 4.0);
        tracker.Removed(7, 500, 1, 1, 4.0);
        tracker.Removed(7, 500, 1, 2, 4.0);
        tracker.Loot(7, {{500, 1}}, true, 4.0);
        REQUIRE_FALSE(tracker.Find(7, killingArrow)->Hidden);
    }
}

TEST_CASE("a native replay arriving after a made fallback replaces that mesh once, preserving removal", "[stuck-arrows]")
{
    StuckArrowTracker tracker;
    const StuckArrowTracker::Life life{7, 70, 700};
    const StuckArrowTracker::Entry made{10, 0, 500};
    const StuckArrowTracker::Entry native{11, 100, 500};
    tracker.Update(life, {}, 1.0);
    REQUIRE(tracker.AddMade(7, made, 1.1).empty());
    tracker.Removed(7, 500, 1, 0, 1.2);
    const auto replay = tracker.Update(life, {native}, 1.3);
    REQUIRE(replay.Replayed.size() == 1);
    REQUIRE(replay.Replayed[0].first == made.Object);
    REQUIRE(replay.LetGo == std::vector<StuckArrowTracker::Released>{{7, made.Object}});
    REQUIRE(tracker.ArrowsOf(7).size() == 1);
    REQUIRE(tracker.Find(7, native)->Hidden);
    REQUIRE(tracker.Find(7, native)->Claimed);
    REQUIRE(tracker.Update(life, {native}, 1.4).LetGo.empty());
}

TEST_CASE("native resurrection invalidates a queued cut even between observations", "[stuck-arrows]")
{
    StuckArrowBeheadings beheadings;
    beheadings.SetOwned({{7, {70, 3}}});
    REQUIRE(beheadings.Push(7));
    beheadings.NewLifeOfForm(7); // exactly the API invoked at native Resurrect entry
    REQUIRE(beheadings.Push(7)); // cut again before a polling update
    const auto queued = beheadings.Take();
    REQUIRE(queued.size() == 2);
    const StuckArrowBeheadings::Current dead{true, 70, 3, true, true};
    REQUIRE_FALSE(beheadings.IsCurrent(queued[0], dead));
    REQUIRE(beheadings.IsCurrent(queued[1], dead));
    REQUIRE_FALSE(beheadings.IsCurrent(queued[1], {true, 70, 3, true, false})); // retained native head bit while alive
}

TEST_CASE("an authoritative removal is monotonic for its shot and survives inventory-before-first-read ordering", "[stuck-arrows]")
{
    StuckArrowStore store;
    StuckArrowStore::Arrow shot;
    shot.Sequence = 1;
    shot.Received = 1.0;
    store.AddArrow(70, shot);
    store.ArrowDone(70, 1);
    shot.Removed = true;
    shot.Received = 1.1;
    store.AddArrow(70, shot);
    REQUIRE(store.Arrows(70)->size() == 1);
    REQUIRE(store.WaitingArrows().size() == 1);
    REQUIRE(store.Arrows(70)->front().Removed);
    shot.Removed = false; // an older placement/duplicate cannot restore it
    store.AddArrow(70, shot);
    REQUIRE(store.Arrows(70)->front().Removed);

    StuckArrowTracker tracker;
    const StuckArrowTracker::Life remote{7, 70, 700, false};
    const StuckArrowTracker::Entry first{1, 100, 500}, second{2, 101, 500};
    tracker.Update(remote, {first, second}, 1.2); // first read already has post-loot inventory: no decrease to infer
    tracker.Claim(7, first, 1);
    tracker.Claim(7, second, 2);
    REQUIRE(tracker.HideSequence(7, 1));
    tracker.Loot(7, {}, false, 1.2); // copies never charge inventory changes to a different shot
    REQUIRE(tracker.Find(7, first)->Hidden);
    REQUIRE_FALSE(tracker.Find(7, second)->Hidden);
    REQUIRE(tracker.HideSequence(7, 1)); // duplicated tombstone is idempotent
    REQUIRE_FALSE(tracker.Find(7, second)->Hidden);
}

TEST_CASE("what was told before the game revived an actor itself is not applied to its next life", "[stuck-arrows]")
{
    StuckArrowStore store;
    StuckArrowStore::Arrow shot;
    shot.Sequence = 1;
    store.AddArrow(70, shot);
    store.AddLimbs(70, StuckArrowStore::kHeadBit, 1.0);
    const StuckArrowStore::Arrow queued = store.WaitingArrows().front().second;
    REQUIRE_FALSE(store.IsOfPastLife(700, queued.Epoch));
    REQUIRE_FALSE(store.LimbsOfPastLife(70, 700));

    store.NativeRevival(700); // the native resurrection, before any owner notification and before the next update
    REQUIRE(store.IsOfPastLife(700, queued.Epoch));
    REQUIRE(store.LimbsOfPastLife(70, 700));
    REQUIRE_FALSE(store.IsOfPastLife(701, queued.Epoch)); // another actor's copy is untouched
    REQUIRE_FALSE(store.LimbsOfPastLife(70, 701));

    SECTION("a shot and a cut told on the new life still apply")
    {
        StuckArrowStore::Arrow next;
        next.Sequence = 2;
        store.AddArrow(70, next);
        store.AddLimbs(70, StuckArrowStore::kHeadBit, 2.0);
        const auto waiting = store.WaitingArrows();
        const auto fresh = std::find_if(waiting.begin(), waiting.end(), [](const auto& a) { return a.second.Sequence == 2; });
        REQUIRE(fresh != waiting.end());
        REQUIRE_FALSE(store.IsOfPastLife(700, fresh->second.Epoch));
        REQUIRE_FALSE(store.LimbsOfPastLife(70, 700)); // the limbs told again carry the new life's stamp
    }
    SECTION("a head cut told for the old life, discarded, is cut again when told for the new life")
    {
        REQUIRE(store.LimbsOfPastLife(70, 700));
        store.DiscardLimbs(70); // CutOffLimbs finds the pending cut is of the life the game ended
        REQUIRE_FALSE(store.HasLimbsToCutOff());
        store.AddLimbs(70, StuckArrowStore::kHeadBit, 3.0); // the same head cut, told for the new life
        REQUIRE(store.HasLimbsToCutOff());
        REQUIRE_FALSE(store.LimbsOfPastLife(70, 700));
        const auto toCut = store.LimbsToCutOff();
        REQUIRE(toCut.size() == 1);
        REQUIRE(toCut.front().first == 70);
        REQUIRE((toCut.front().second & StuckArrowStore::kHeadBit) != 0);
    }
    SECTION("a second revival drops the new life's too, and the old arrow stays dropped")
    {
        StuckArrowStore::Arrow next;
        next.Sequence = 2;
        store.AddArrow(70, next);
        const uint64_t cEpoch = store.Arrows(70)->back().Epoch;
        store.NativeRevival(700);
        REQUIRE(store.IsOfPastLife(700, cEpoch));
        REQUIRE(store.IsOfPastLife(700, queued.Epoch));
    }
    SECTION("a cleared store forgets the revivals")
    {
        store.Clear();
        REQUIRE_FALSE(store.IsOfPastLife(700, queued.Epoch));
    }
}

TEST_CASE("the stuck arrow watch reads a few actors a frame, in turn, round the whole list", "[stuck-arrows]")
{
    std::vector<size_t> read;
    const auto cRead = [&read](const size_t aAt)
    {
        read.push_back(aAt);
    };
    const auto cNone = [](size_t)
    {
        return false;
    };

    SECTION("a frame reads its budget from where the last one stopped")
    {
        const auto cStep = StuckArrowWatch::Next(40, 4, 16, cNone, cRead);
        REQUIRE(read.size() == 16);
        REQUIRE(read.front() == 4);
        REQUIRE(read.back() == 19);
        REQUIRE(cStep.Next == 20);
        REQUIRE_FALSE(cStep.RoundDone);
    }

    SECTION("passing the end finishes a round and goes on from the start")
    {
        const auto cStep = StuckArrowWatch::Next(20, 16, 16, cNone, cRead);
        REQUIRE(read == std::vector<size_t>{16, 17, 18, 19, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
        REQUIRE(cStep.Next == 12);
        REQUIRE(cStep.RoundDone);
    }

    SECTION("a short list is read once a frame, never twice")
    {
        const auto cStep = StuckArrowWatch::Next(5, 2, 16, cNone, cRead);
        REQUIRE(read == std::vector<size_t>{2, 3, 4, 0, 1});
        REQUIRE(cStep.Next == 2);
        REQUIRE(cStep.RoundDone);
    }

    SECTION("objects are passed over without using the budget")
    {
        // Every entry but each fourth is an object.
        const auto cObject = [](const size_t aAt)
        {
            return aAt % 4 != 0;
        };
        const auto cStep = StuckArrowWatch::Next(100, 0, 3, cObject, cRead);
        REQUIRE(read == std::vector<size_t>{0, 4, 8});
        REQUIRE(cStep.Next == 9);
        REQUIRE_FALSE(cStep.RoundDone);
    }

    SECTION("a list of objects only is looked over once and none is read")
    {
        const auto cStep = StuckArrowWatch::Next(8, 3, 16, [](size_t) { return true; }, cRead);
        REQUIRE(read.empty());
        REQUIRE(cStep.Next == 3);
        REQUIRE(cStep.RoundDone);
    }

    SECTION("a list that shrank below the cursor starts a new round from the start")
    {
        const auto cStep = StuckArrowWatch::Next(4, 10, 2, cNone, cRead);
        REQUIRE(read == std::vector<size_t>{0, 1});
        REQUIRE(cStep.Next == 2);
        REQUIRE(cStep.RoundDone);
    }

    SECTION("an empty list reads nothing and keeps the cursor")
    {
        const auto cStep = StuckArrowWatch::Next(0, 7, 16, cNone, cRead);
        REQUIRE(read.empty());
        REQUIRE(cStep.Next == 7);
        REQUIRE_FALSE(cStep.RoundDone);
    }
}
