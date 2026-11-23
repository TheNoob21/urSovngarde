#include <catch2/catch.hpp>

#include <Services/GrabbedBody.h>

namespace
{
constexpr uint32_t cBody = 0xFF000D42;
constexpr uint32_t cOtherBody = 0xFF000D43;
} // namespace

TEST_CASE("a body grabbed here is asked for once per grab", "[grab]")
{
    GrabbedBody grabbed;

    grabbed.Update(cBody, true);
    REQUIRE(grabbed.ShouldAsk(cBody));
    grabbed.Asked(cBody);

    // Still held, frame after frame: not asked again, so a player who grabs it after us keeps it.
    for (int frame = 0; frame < 120; ++frame)
    {
        grabbed.Update(cBody, true);
        CHECK_FALSE(grabbed.ShouldAsk(cBody));
    }

    // Let go, then grabbed again: a new grab, asked again.
    grabbed.Update(0, false);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));
    grabbed.Update(cBody, true);
    CHECK(grabbed.ShouldAsk(cBody));
}

TEST_CASE("only the body the player holds is asked for", "[grab]")
{
    GrabbedBody grabbed;

    // Nothing held.
    grabbed.Update(0, false);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));
    CHECK_FALSE(grabbed.ShouldAsk(0));

    grabbed.Update(cBody, true);
    CHECK_FALSE(grabbed.ShouldAsk(cOtherBody));
    CHECK(grabbed.ShouldAsk(cBody));
    grabbed.Asked(cBody);

    // Straight from one body to another, with no empty frame between: the second one is a new grab.
    grabbed.Update(cOtherBody, true);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));
    CHECK(grabbed.ShouldAsk(cOtherBody));
}

TEST_CASE("a body ours when grabbed is not taken back from a player who grabs it after us", "[grab]")
{
    GrabbedBody grabbed;

    // Ours at the grab: nothing to ask.
    grabbed.Update(cBody, false);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));

    // The other player grabs it too and gets it, while we still hold it: it stays theirs.
    grabbed.Update(cBody, true);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));

    // Let go and grabbed again, now theirs: a new grab, asked for.
    grabbed.Update(0, false);
    grabbed.Update(cBody, true);
    CHECK(grabbed.ShouldAsk(cBody));
}

TEST_CASE("a request that could not be sent is tried again next frame", "[grab]")
{
    GrabbedBody grabbed;

    // The body's owner is not known here yet, so nothing went out.
    grabbed.Update(cBody, true);
    REQUIRE(grabbed.ShouldAsk(cBody));

    grabbed.Update(cBody, true);
    CHECK(grabbed.ShouldAsk(cBody));
    grabbed.Asked(cBody);

    grabbed.Update(cBody, true);
    CHECK_FALSE(grabbed.ShouldAsk(cBody));
}

TEST_CASE("a VR hand asks for a dead body, or a living NPC lying in its ragdoll", "[grab]")
{
    // Dead: always, as before.
    CHECK(AsksForGrabbed(true, false, false, false));
    CHECK(AsksForGrabbed(true, true, false, false));
    // Alive in its ragdoll (knocked down, paralysed): carried here, so asked for, as the grab key does in Skyrim SE.
    CHECK(AsksForGrabbed(false, true, false, false));
    // Standing, kneeling in its bleedout, or another player or a summon of one: never.
    CHECK_FALSE(AsksForGrabbed(false, false, false, false));
    CHECK_FALSE(AsksForGrabbed(false, true, true, false));
    CHECK_FALSE(AsksForGrabbed(false, true, false, true));
}

TEST_CASE("a VR hand holds a living NPC only after staying on it and moving with it", "[grab]")
{
    const glm::vec3 cStep{8.f, 0.f, 0.f};
    const std::array<glm::vec3, 2> cWithIt{glm::vec3{}, cStep};

    // Carried: held once the hand has moved with it for kHeldFor ms over kSamples samples.
    HandHold carried;
    double now = 0.0;
    carried.Sample(now, false, {}, {});
    int heldAt = -1;
    for (int i = 1; i <= 6 && heldAt < 0; ++i)
    {
        now += 100.0;
        if (carried.Sample(now, true, cWithIt, cStep))
            heldAt = i;
    }
    CHECK(heldAt == static_cast<int>(HandHold::kSamples));

    // A hand near it at one sample only: never held.
    HandHold once;
    now = 0.0;
    once.Sample(now, false, {}, {});
    for (int i = 0; i < 10; ++i)
    {
        now += 100.0;
        CHECK_FALSE(once.Sample(now, i == 3, cWithIt, cStep));
    }

    // Hit: near it, but it flies off its own way while the hand stops.
    HandHold hit;
    now = 0.0;
    hit.Sample(now, false, {}, {});
    for (int i = 0; i < 10; ++i)
    {
        now += 100.0;
        CHECK_FALSE(hit.Sample(now, true, {glm::vec3{}, glm::vec3{2.f, 0.f, 0.f}}, {60.f, 0.f, 0.f}));
    }

    // Moving with the hand, but the hand drops off it for a sample: counted again from there.
    HandHold broken;
    now = 0.0;
    broken.Sample(now, false, {}, {});
    for (int i = 0; i < 2; ++i)
    {
        now += 100.0;
        CHECK_FALSE(broken.Sample(now, true, cWithIt, cStep));
    }
    now += 100.0;
    CHECK_FALSE(broken.Sample(now, false, cWithIt, cStep));
    for (uint32_t i = 1; i < HandHold::kSamples; ++i)
    {
        now += 100.0;
        CHECK_FALSE(broken.Sample(now, true, cWithIt, cStep));
    }
    now += 100.0;
    CHECK(broken.Sample(now, true, cWithIt, cStep));

    // Samples too close together to make up kHeldFor: not yet.
    HandHold quick;
    now = 0.0;
    quick.Sample(now, false, {}, {});
    for (uint32_t i = 0; i < HandHold::kSamples + 2; ++i)
    {
        now += 30.0;
        CHECK_FALSE(quick.Sample(now, true, cWithIt, cStep));
    }
}
