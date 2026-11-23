#include <catch2/catch.hpp>
#include <Systems/BodyPoseOrder.h>
#include <array>
#include <cstddef>

namespace
{
using Graph = BodyPoseOrder::Graph<5>;
// Wire order deliberately puts pelvis last. Angles model world-yaw writes and descendant propagation.
constexpr size_t kSpine = 0, kHead = 1, kHand = 2, kUnused = 3, kPelvis = 4;
struct Fixture
{
    std::array<int, 5> Nodes{};
    Graph graph;
    Fixture()
    {
        for (size_t i : {kSpine, kHead, kHand, kPelvis})
            graph.Bones[i].Node = &Nodes[i];
    }
    void Link(size_t aParent, size_t aChild) { REQUIRE(graph.AddDescendant(aParent, graph.Bones[aChild])); }
};
std::array<float, 5> Apply(const Graph& acGraph, const std::array<uint8_t, 5>& acOrder, size_t aCount, float aPelvisYaw)
{
    std::array<float, 5> world{};
    std::array<float, 5> targets{};
    targets[kPelvis] = aPelvisYaw;
    for (size_t n = 0; n < aCount; ++n)
    {
        const size_t bone = acOrder[n];
        const float delta = targets[bone] - world[bone];
        world[bone] = targets[bone];
        for (size_t child = 0; child < world.size(); ++child)
            if (acGraph.Descendants[bone][child])
                world[child] += delta;
    }
    return world;
}
} // namespace

TEST_CASE("Actual ancestor order keeps root-relative head and hand targets under pelvis yaw", "[body_pose_order]")
{
    Fixture f;
    f.Link(kPelvis, kSpine);
    f.Link(kPelvis, kHead);
    f.Link(kPelvis, kHand);
    f.Link(kSpine, kHead);
    f.Link(kSpine, kHand);
    std::array<uint8_t, 5> order{};
    size_t count = 0;
    REQUIRE(f.graph.Sort(order, count));
    REQUIRE(count == 4);
    for (float yaw : {0.f, 90.f, -45.f})
    {
        const auto world = Apply(f.graph, order, count, yaw);
        CHECK(world[kPelvis] == yaw);
        CHECK(world[kHead] == 0.f);
        CHECK(world[kHand] == 0.f);
    }
    const std::array<uint8_t, 5> wireOrder{kSpine, kHead, kHand, kPelvis, kUnused};
    const auto wrong = Apply(f.graph, wireOrder, 4, 90.f);
    CHECK(wrong[kHead] == 90.f);
    CHECK(wrong[kHand] == 90.f);
}

TEST_CASE("Ordering handles sibling pelvis, nested parents, missing bones and invalid graphs", "[body_pose_order]")
{
    Fixture f;
    f.Link(kSpine, kHead);
    f.Link(kSpine, kHand);
    f.Link(kHead, kHand);
    std::array<uint8_t, 5> order{};
    size_t count = 0;
    REQUIRE(f.graph.Sort(order, count));
    REQUIRE(count == 4);
    CHECK(order[0] == kSpine);
    CHECK(order[1] == kHead);
    CHECK(order[2] == kHand);
    CHECK(order[3] == kPelvis);
    CHECK(Apply(f.graph, order, count, 90.f)[kHand] == 0.f);
    f.Link(kHand, kSpine);
    CHECK_FALSE(f.graph.Sort(order, count));
    CHECK(count == 0);

    Fixture alias;
    alias.graph.Bones[kHead].Node = alias.graph.Bones[kSpine].Node;
    CHECK_FALSE(alias.graph.Sort(order, count));
    Fixture split;
    int entry = 0;
    split.graph.Bones[kHead].Entry = &entry;
    CHECK_FALSE(split.graph.AddDescendant(kPelvis, {split.graph.Bones[kSpine].Node, &entry}));
    Fixture self;
    self.Link(kSpine, kSpine);
    CHECK_FALSE(self.graph.Sort(order, count));
}
