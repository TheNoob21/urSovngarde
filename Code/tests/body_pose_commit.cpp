#include <catch2/catch.hpp>
#include <Systems/BodyPoseCommit.h>
#include <Systems/BodyPosePresentation.h>
#include <Systems/RemotePoseInterpolation.h> // the receiver's interpolation and wire unpack drive the guard tests
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace BodyPoseCommit;
namespace
{
Transform Identity(float aX = 0.f, float aY = 0.f, float aZ = 0.f, float aScale = 1.f)
{
    return {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, aX, aY, aZ, aScale};
}
Transform Yawed(float aRadians, const Transform& acBase)
{
    Transform t = acBase;
    const float c = std::cos(aRadians), s = std::sin(aRadians);
    t[0] = c;
    t[1] = -s;
    t[2] = 0.f;
    t[3] = s;
    t[4] = c;
    t[5] = 0.f;
    t[6] = 0.f;
    t[7] = 0.f;
    t[8] = 1.f;
    return t;
}

Transform Posed(const glm::quat& acQ, const glm::vec3& acT, float aScale = 1.f)
{
    Transform out = Identity(acT.x, acT.y, acT.z, aScale);
    PackRotation(glm::normalize(acQ), out);
    return out;
}

glm::vec3 At(const Transform& acT)
{
    return {acT[9], acT[10], acT[11]};
}

glm::mat3 Rows(const Transform& acT)
{
    glm::mat3 m{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            m[column][row] = acT[row * 3 + column];
    return m;
}

GraceKey GraceIdentity()
{
    return {11, 17, (uint64_t{23} << 32) | 0xFF000123u, 31, 0x1000, 0x2000, 0xFF000123u};
}
} // namespace

TEST_CASE("Native flattened-tree layout matches the installed SE helper", "[body_pose_commit]")
{
    CHECK(kEntryStride == 0x80);
    CHECK(kEntryWorld == 0x34);
    CHECK(kEntryParent == 0x68);
    CHECK(kEntryNode == 0x70);
    CHECK(kNodeWorld == 0x7C);
    CHECK(kNodeParent == 0x30);
    CHECK(kTreeCount == 0x128);
    CHECK(kTreeEntries == 0x130);
    CHECK(kEntryWorld + kTransformBytes == kEntryParent);
}

TEST_CASE("The actor root entry is excluded from the hip delta and from writes", "[body_pose_commit]")
{
    std::array<uintptr_t, 8> nodes{0x1000, 0x2000, 0, 0x3000, 0x1000, 0, 0, 0};
    std::array<bool, 8> excluded{};
    CHECK(MarkRootExcluded(nodes, 5, 0x1000, excluded) == 2);
    CHECK(excluded[0]);
    CHECK(excluded[4]);
    CHECK_FALSE(excluded[1]);
    CHECK_FALSE(excluded[2]);                            // flat-only entries never match a root
    CHECK(MarkRootExcluded(nodes, 5, 0, excluded) == 0); // no root, nothing excluded
    CHECK(MarkRootExcluded(nodes, 0, 0x1000, excluded) == 0);

    // Solver: entry 0 is the root (no parent), 1 pelvis, 2 a skinned child of the pelvis.
    using namespace BodyPosePresentation;
    Input in;
    in.Count = 3;
    in.HasData = true;
    in.HasLegs = true;
    in.HasHips = true;
    in.HipOffset = {0, 0, -5};
    in.Parent[1] = 0;
    in.Parent[2] = 1;
    in.Bone[kPelvis] = 1;
    in.Baseline[0].Translation = {0, 0, 0};
    in.Baseline[1].Translation = {0, 0, 10};
    in.Baseline[2].Translation = {1, 0, 10};
    std::array<bool, kMaxNodes> hipExcluded{};
    Output plain, masked;
    REQUIRE(Solve(in, plain));
    CHECK(plain.world[0].Translation.z == Approx(-15.f)); // the generic solver moves every entry, root included
    hipExcluded[0] = true;
    REQUIRE(Solve(in, masked, &hipExcluded));
    CHECK(masked.world[0].Translation.z == Approx(0.f));  // root keeps its native world
    CHECK(masked.world[1].Translation.z == Approx(-5.f)); // pelvis lands on root + hip offset
    CHECK(masked.world[2].Translation.z == Approx(-5.f));
    CHECK(masked.world[2].Translation.x == Approx(1.f));
    // An all-false mask is the unmasked solver.
    hipExcluded.fill(false);
    REQUIRE(Solve(in, masked, &hipExcluded));
    for (size_t i = 0; i < 3; ++i)
        CHECK(masked.world[i].Translation.z == Approx(plain.world[i].Translation.z));
}

TEST_CASE("Linked entries write the node world and its flat copy; flat-only entries write one", "[body_pose_commit]")
{
    constexpr uintptr_t entries = 0x100000;
    std::array<uintptr_t, 6> nodes{0x5000, 0, 0x6000, 0, 0x7000, 0};
    std::array<bool, 6> selected{true, true, false, true, true, false};
    std::array<Write, 16> out{};
    size_t written{};
    REQUIRE(SelectWrites(entries, nodes, 6, selected, out, written));
    REQUIRE(written == 6);
    CHECK(out[0].Address == 0x5000 + 0x7C);
    CHECK(out[0].NodeWorld);
    CHECK(out[0].Entry == 0);
    CHECK(out[1].Address == entries + 0x34);
    CHECK_FALSE(out[1].NodeWorld);
    CHECK(out[1].Entry == 0);
    CHECK(out[2].Address == entries + 1 * 0x80 + 0x34);
    CHECK(out[2].Entry == 1);
    CHECK(out[3].Address == entries + 3 * 0x80 + 0x34);
    CHECK(out[3].Entry == 3);
    CHECK(out[4].Address == 0x7000 + 0x7C);
    CHECK(out[4].Entry == 4);
    CHECK(out[5].Address == entries + 4 * 0x80 + 0x34);
    CHECK(out[5].Entry == 4);
    for (size_t i = 0; i < written; ++i)
        CHECK(out[i].Entry != 2); // unselected linked entry: neither address

    std::array<Write, 5> small{};
    CHECK_FALSE(SelectWrites(entries, nodes, 6, selected, small, written)); // capacity is a hard failure
    CHECK_FALSE(SelectWrites(0, nodes, 6, selected, out, written));
    CHECK_FALSE(SelectWrites(UINTPTR_MAX - 0x100, nodes, 6, selected, out, written));
    selected.fill(false);
    REQUIRE(SelectWrites(entries, nodes, 6, selected, out, written));
    CHECK(written == 0);
}

TEST_CASE("Only finite proper rotations with the native scale bits are writable", "[body_pose_commit]")
{
    const auto base = Identity(10.f, 20.f, 30.f, 1.25f);
    auto target = Yawed(glm::half_pi<float>(), base);
    target[9] += 40.f;
    CHECK(ValidTarget(target, base, 512.f));
    CHECK(ValidTarget(base, base, 0.f));
    CHECK_FALSE(ValidTarget(target, base, 39.f)); // travel bound
    for (size_t i = 0; i < 13; ++i)
    {
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
        {
            auto t = target;
            t[i] = bad;
            CHECK_FALSE(ValidTarget(t, base, 512.f));
            auto b = base;
            b[i] = bad;
            CHECK_FALSE(ValidTarget(target, b, 512.f));
        }
    }
    auto scaled = target;
    scaled[0] *= 2.f;
    scaled[4] *= 2.f;
    scaled[8] *= 2.f;
    CHECK_FALSE(ValidTarget(scaled, base, 512.f)); // scale/shear inside the matrix
    auto mirrored = target;
    mirrored[2] = 0.f;
    mirrored[5] = 0.f;
    mirrored[8] = -1.f;
    CHECK_FALSE(ValidTarget(mirrored, base, 512.f)); // reflection
    auto rescaled = target;
    rescaled[12] = 1.f;
    CHECK_FALSE(ValidTarget(rescaled, base, 512.f)); // scale is never posed
    auto zero = base;
    zero[12] = 0.f;
    CHECK_FALSE(ValidTarget(zero, zero, 512.f));
    CHECK_FALSE(ValidTarget(target, base, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(ValidTarget(target, base, -1.f));
}

TEST_CASE("The fixed address table finds exactly what was inserted", "[body_pose_commit]")
{
    AddressTable<64> table;
    table.Clear();
    CHECK(table.Find(0x1234) == -1);
    CHECK_FALSE(table.Insert(0, 1));
    CHECK_FALSE(table.Insert(0x10, -1));
    for (int i = 0; i < 32; ++i)
        REQUIRE(table.Insert(0x200000 + static_cast<uintptr_t>(i) * 0x80 + 0x34, static_cast<int16_t>(i)));
    CHECK_FALSE(table.Insert(0x900000, 99)); // at most half full
    for (int i = 0; i < 32; ++i)
        CHECK(table.Find(0x200000 + static_cast<uintptr_t>(i) * 0x80 + 0x34) == i);
    CHECK(table.Find(0x200000 + 0x34 + 4) == -1);
    CHECK(table.Find(0) == -1);
    table.Clear();
    REQUIRE(table.Insert(0x500, 3));
    CHECK_FALSE(table.Insert(0x500, 4)); // duplicates are an aliasing error, not an update
    CHECK(table.Find(0x500) == 3);
}

TEST_CASE("actor-subtree set: every reachable object once, kinds kept, incompleteness reported", "[body_pose_commit]")
{
    // A small scene: 1 root -> {2, 3}; 2 -> {4, 5}; 3 -> {5 (shared), 6}; 6 -> {1 (cycle back to root), 0 (null slot)}.
    std::map<uintptr_t, std::vector<uintptr_t>> children{{0x10, {0x20, 0x30}}, {0x20, {0x40, 0x50}}, {0x30, {0x50, 0x60}}, {0x60, {0x10, 0}}};
    const auto kindOf = [](uintptr_t node)
    {
        return node == 0x40 || node == 0x50 ? 2 : 1;
    }; // leaves are "geometry"
    auto visit = [&](uintptr_t node, uint32_t, auto& push)
    {
        const auto it = children.find(node);
        if (it != children.end())
            for (const auto child : it->second)
                push(child);
        return kindOf(node);
    };
    AddressTable<64> set;
    std::array<uintptr_t, 32> order{};
    std::array<SubtreePending, 32> stack{};
    auto stats = CollectSubtree(0x10, 64, set, order, stack, visit);
    CHECK(stats.Objects == 6);
    CHECK(stats.Complete());
    CHECK(stats.Duplicates >= 2); // 5 via 2 and 3, the root via 6
    CHECK(stats.MaxDepth == 2);
    for (const uintptr_t node : {0x10, 0x20, 0x30, 0x40, 0x50, 0x60})
    {
        CAPTURE(node);
        CHECK(set.Find(node) == kindOf(node));
    }
    CHECK(set.Find(0x70) == -1);
    CHECK(set.Find(0) == -1);

    // Depth cap: children of depth-1 objects cannot be queued.
    stats = CollectSubtree(0x10, 1, set, order, stack, visit);
    CHECK(stats.DepthCapped);
    CHECK_FALSE(stats.Complete());
    CHECK(set.Find(0x40) == -1);
    CHECK(set.Find(0x20) == 1);

    // Unreadable objects stay members (kind 0) but make the set incomplete.
    auto unreadable = [&](uintptr_t node, uint32_t depth, auto& push)
    {
        return node == 0x30 ? -1 : visit(node, depth, push);
    };
    stats = CollectSubtree(0x10, 64, set, order, stack, unreadable);
    CHECK(stats.Unreadable == 1);
    CHECK_FALSE(stats.Complete());
    CHECK(set.Find(0x30) == 0);
    CHECK(set.Find(0x60) == -1); // only reachable through the unreadable node

    // Capacity: a fan-out larger than the order/stack arrays truncates instead of overflowing.
    std::map<uintptr_t, std::vector<uintptr_t>> wide{{0x1000, {}}};
    for (uintptr_t i = 1; i <= 100; ++i)
        wide[0x1000].push_back(0x1000 + i * 0x10);
    auto visitWide = [&](uintptr_t node, uint32_t, auto& push)
    {
        const auto it = wide.find(node);
        if (it != wide.end())
            for (const auto child : it->second)
                push(child);
        return 0;
    };
    stats = CollectSubtree(0x1000, 64, set, order, stack, visitWide);
    CHECK(stats.Truncated);
    CHECK_FALSE(stats.Complete());
    CHECK(stats.Objects <= order.size());
    stats = CollectSubtree(0, 64, set, order, stack, visit);
    CHECK(stats.Objects == 0);
    CHECK_FALSE(stats.Complete());

    // Full-size table as the client uses it: 2048 members, all found, others not.
    AddressTable<4096> big;
    std::array<uintptr_t, 2048> bigOrder{};
    std::array<SubtreePending, 2048> bigStack{};
    auto chain = [](uintptr_t node, uint32_t, auto& push)
    {
        if (node < 0x40000000 + 2047 * 0x140)
            push(node + 0x140);
        return 1;
    };
    stats = CollectSubtree(0x40000000, 4096, big, bigOrder, bigStack, chain);
    CHECK(stats.Objects == 2048);
    CHECK(stats.Complete());
    for (uintptr_t i = 0; i < 2048; ++i)
        REQUIRE(big.Find(0x40000000 + i * 0x140) == 1);
    CHECK(big.Find(0x40000000 + 2048 * 0x140) == -1);
    CHECK(big.Find(0x40000000 + 0x8) == -1);
}

TEST_CASE("rotation packing round-trips in the native row-major convention and detects a transpose", "[body_pose_commit]")
{
    const glm::quat q = glm::normalize(glm::angleAxis(1.1f, glm::normalize(glm::vec3{.3f, -.8f, .5f})));
    Transform t = Identity(5.f, 6.f, 7.f, 1.3f);
    PackRotation(q, t);
    CHECK(t[9] == 5.f);
    CHECK(t[12] == 1.3f); // translation and scale untouched
    glm::quat back{};
    REQUIRE(UnpackRotation(t, back));
    CHECK(AngleDeg(q, back) < .01f);
    // Native rows: rotating the x axis by q must give the first COLUMN of the row-major matrix.
    const glm::vec3 x = q * glm::vec3{1.f, 0.f, 0.f};
    CHECK(t[0] == Approx(x.x).margin(1e-5));
    CHECK(t[3] == Approx(x.y).margin(1e-5));
    CHECK(t[6] == Approx(x.z).margin(1e-5));
    Transform transposed = t;
    std::swap(transposed[1], transposed[3]);
    std::swap(transposed[2], transposed[6]);
    std::swap(transposed[5], transposed[7]);
    REQUIRE(UnpackRotation(transposed, back));
    CHECK(AngleDeg(q, back) > 100.f); // the inverse rotation: a packing error would be seen by the identity arm
    Transform broken = t;
    broken[0] *= 2.f;
    CHECK_FALSE(UnpackRotation(broken, back));
    CHECK(AngleDeg(q, q) == Approx(0.f).margin(1e-3));
    CHECK(AngleDeg(q, -q) == Approx(0.f).margin(1e-3)); // the double cover is the same rotation

    AngleStats<8> stats;
    CHECK(stats.Median() == 0.f);
    for (const float v : {3.f, .5f, 2.f, std::numeric_limits<float>::quiet_NaN(), .1f})
        stats.Add(v);
    CHECK(stats.Count == 4);
    CHECK(stats.Max == 3.f);
    CHECK(stats.Over1 == 2);
    CHECK(stats.Median() == Approx(1.25f));
    stats.Add(10.f);
    CHECK(stats.Median() == Approx(2.f));
    stats.Clear();
    CHECK(stats.Count == 0);
    CHECK(stats.Max == 0.f);
}

TEST_CASE("rigid layout constants match the installed SE NiAVObject", "[body_pose_commit]")
{
    CHECK(kNodeCollision == 0x40);
    CHECK(kNodeLocal == 0x48);
    CHECK(kNodeLocal + kTransformBytes == kNodeWorld);
    CHECK(kNodeFlags == 0xF4);
    CHECK(kGeometrySkin == 0x130);
    CHECK(kIgnoreLocalFlag == 0x200);
    REQUIRE(kRigidSkipNames.size() == static_cast<size_t>(RigidSkip::Count));
    std::map<std::string, int> seen;
    for (const char* name : kRigidSkipNames)
    {
        REQUIRE(name);
        CHECK(++seen[name] == 1);
    }
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::ParentUnwritten)]) == "parent_unwritten");
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::NotComposed)]) == "not_composed");
    // appended only, so every index and name is unchanged.
    CHECK(static_cast<size_t>(RigidSkip::Invalid) == 11);
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::ParentRefused)]) == "parent_refused");
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::OwnUpdate)]) == "own_update");
    CHECK(static_cast<size_t>(RigidSkip::OwnUpdate) == 13);
    // collision_driven appended; every index and name unchanged.
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::Collision)]) == "collision");
    CHECK(static_cast<size_t>(RigidSkip::Collision) == 4);
    CHECK(std::string(kRigidSkipNames[static_cast<size_t>(RigidSkip::CollisionDriven)]) == "collision_driven");
    CHECK(static_cast<size_t>(RigidSkip::OwnUpdate) + 1 == static_cast<size_t>(RigidSkip::CollisionDriven));
    CHECK(static_cast<size_t>(RigidSkip::CollisionDriven) + 1 == static_cast<size_t>(RigidSkip::Count));
    CHECK(kNodeUpdateWorldSlot == 0x180); // EE2518 calls [rax+180h] for the node's own world
    // per-item outcome codes: the refusal names, then written / unchanged / collision_composed, all distinct.
    REQUIRE(kRigidOutcomeCount == kRigidSkipNames.size() + 3);
    std::map<std::string, int> outcomes;
    for (uint8_t code = 0; code < kRigidOutcomeCount; ++code)
    {
        REQUIRE(RigidOutcomeName(code));
        CHECK(++outcomes[RigidOutcomeName(code)] == 1);
    }
    CHECK(std::string(RigidOutcomeName(kRigidOutcomeWritten)) == "written");
    CHECK(std::string(RigidOutcomeName(kRigidOutcomeUnchanged)) == "unchanged");
    CHECK(std::string(RigidOutcomeName(kRigidOutcomeCollisionComposed)) == "collision_composed");
    CHECK(std::string(RigidOutcomeName(static_cast<uint8_t>(RigidSkip::CollisionDriven))) == "collision_driven");
    CHECK(std::string(RigidOutcomeName(kRigidOutcomeCount)) == "unknown");
}

TEST_CASE("ComposeWorld is the native compose, operation order included", "[body_pose_commit]")
{
    // Identity parent: the local comes back bit for bit.
    const Transform local = Posed(glm::angleAxis(.7f, glm::normalize(glm::vec3{1.f, 2.f, -.5f})), {3.f, -4.f, 12.f}, 1.25f);
    CHECK(SameBits(ComposeWorld(Identity(), local), local));
    // Against an independent glm reference: R = Rp Rl, t = tp + sp (Rp tl), s = sp sl.
    const Transform parent = Posed(glm::angleAxis(-1.9f, glm::normalize(glm::vec3{.2f, -.9f, .3f})), {11063.f, -71489.f, 5551.f}, 1.1f);
    const Transform out = ComposeWorld(parent, local);
    const glm::mat3 expectedR = Rows(parent) * Rows(local);
    const glm::vec3 expectedT = At(parent) + parent[12] * (Rows(parent) * At(local));
    const glm::mat3 gotR = Rows(out);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            CHECK(gotR[c][r] == Approx(expectedR[c][r]).margin(1e-6));
    CHECK(out[9] == Approx(expectedT.x).margin(.01));
    CHECK(out[10] == Approx(expectedT.y).margin(.01));
    CHECK(out[11] == Approx(expectedT.z).margin(.01));
    CHECK(out[12] == local[12] * parent[12]);
    // The association order is the native one (3059B8-305C99): ((a + b) + c). With row (1e8, -1e8, 1) and all-ones
    // columns any other order loses the 1 entirely.
    Transform p = Identity(0.f, 0.f, 0.f, 1.f), l = Identity(1.f, 1.f, 1.f, 1.f);
    p[0] = 1e8f;
    p[1] = -1e8f;
    p[2] = 1.f;
    l[0] = l[3] = l[6] = 1.f;
    const Transform ordered = ComposeWorld(p, l);
    CHECK(ordered[0] == 1.f);           // (1e8 - 1e8) + 1
    CHECK(ordered[9] == 1.f);           // ((1e8 - 1e8) + 1) * 1 + 0
    CHECK(1e8f + (-1e8f + 1.f) == 0.f); // the other association really differs in float
    // A composed world passes the closeness test against itself and fails with a moved translation.
    CHECK(NearTransform(out, out, 1e-4f, .05f, 1e-5f));
    Transform moved = out;
    moved[10] += 1.f;
    CHECK_FALSE(NearTransform(out, moved, 1e-4f, .05f, 1e-5f));
    Transform turned = out;
    turned[4] += .01f;
    CHECK_FALSE(NearTransform(out, turned, 1e-4f, .05f, 1e-5f));
    Transform scaled = out;
    scaled[12] *= 1.001f;
    CHECK_FALSE(NearTransform(out, scaled, 1e-4f, .05f, 1e-5f));
    Transform bad = out;
    bad[3] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(NearTransform(bad, bad, 1.f, 1.f, 1.f));
}

TEST_CASE("rigid descendants follow the committed hand and refuse everything else", "[body_pose_commit]")
{
    // Flat entries: 0 = right hand (written: turned 90 degrees and moved), 1 = left hand (left native by the solver).
    std::array<bool, 4> written{true, false, false, false};
    std::array<Transform, 4> native{}, target{};
    native[0] = Posed(glm::angleAxis(.4f, glm::vec3{0.f, 1.f, 0.f}), {11000.f, -71000.f, 5600.f});
    target[0] = Posed(glm::angleAxis(1.9f, glm::normalize(glm::vec3{.3f, 1.f, .2f})), {11020.f, -70980.f, 5640.f});
    native[1] = target[1] = Posed(glm::angleAxis(-.4f, glm::vec3{0.f, 1.f, 0.f}), {11000.f, -71040.f, 5600.f});
    // Items, preorder: 0 weapon node (hand), 1 axe geometry (weapon), 2 skinned mesh (hand), 3 collision node (hand;
    // static refusal: keeps it only for an unanalysed collision class), 4 its child, 5 node under the native left
    // hand, 6 its child, 7 a node with the ignore-local flag (hand).
    std::array<RigidItem, 8> items{};
    items[0].ParentEntry = 0;
    items[1].ParentRigid = 0;
    items[2].ParentEntry = 0;
    items[2].Static = RigidSkip::Skinned;
    items[3].ParentEntry = 0;
    items[3].Static = RigidSkip::Collision;
    items[4].ParentRigid = 3;
    items[5].ParentEntry = 1;
    items[6].ParentRigid = 5;
    items[7].ParentEntry = 0;
    const Transform weaponLocal = Posed(glm::angleAxis(.3f, glm::vec3{1.f, 0.f, 0.f}), {2.f, 1.f, -8.f});
    const Transform axeLocal = Posed(glm::angleAxis(1.2f, glm::vec3{0.f, 0.f, 1.f}), {0.f, 0.f, 30.f});
    std::array<RigidRead, 8> reads{};
    const auto readOf = [](const Transform& parentWorld, const Transform& local, uint32_t flags = 0)
    {
        RigidRead r{};
        r.Readable = r.Matches = true;
        r.Flags = flags;
        r.Local = local;
        r.World = flags & kIgnoreLocalFlag ? parentWorld : ComposeWorld(parentWorld, local);
        return r;
    };
    reads[0] = readOf(native[0], weaponLocal);
    reads[1] = readOf(reads[0].World, axeLocal);
    reads[4] = readOf(native[0], weaponLocal);
    reads[5] = readOf(native[1], weaponLocal);
    reads[6] = readOf(reads[5].World, axeLocal);
    reads[7] = readOf(native[0], weaponLocal, kIgnoreLocalFlag);
    std::array<RigidResult, 8> out{};
    const std::array<bool, 4> refusedNone{};
    const size_t stores = SolveRigid(items, items.size(), reads, written, refusedNone, target, native, 4, out, 512.f);
    CHECK(stores == 3); // weapon, axe, ignore-local node

    REQUIRE(out[0].Valid);
    CHECK(out[0].Store);
    CHECK(out[0].Exact);
    CHECK(SameBits(out[0].Native, reads[0].World));
    Transform expectedWeapon = ComposeWorld(target[0], weaponLocal);
    expectedWeapon[12] = reads[0].World[12];
    CHECK(SameBits(out[0].Target, expectedWeapon));
    // Same as moving the native child rigidly with the hand: its offset in hand space is unchanged.
    const glm::vec3 inHandNative = glm::transpose(Rows(native[0])) * (At(reads[0].World) - At(native[0]));
    const glm::vec3 inHandTarget = glm::transpose(Rows(target[0])) * (At(out[0].Target) - At(target[0]));
    CHECK(glm::length(inHandNative - inHandTarget) < .01f);
    CHECK(out[0].Travel > 10.f);
    CHECK(out[0].AngleDeg > 40.f);
    REQUIRE(out[1].Valid);
    CHECK(out[1].Store);
    CHECK(out[1].Exact);
    Transform expectedAxe = ComposeWorld(out[0].Target, axeLocal);
    expectedAxe[12] = reads[1].World[12];
    CHECK(SameBits(out[1].Target, expectedAxe)); // the chain uses the committed parent, not the native one
    CHECK(out[2].Reason == RigidSkip::Skinned);
    CHECK_FALSE(out[2].Valid);
    CHECK(out[3].Reason == RigidSkip::Collision);
    CHECK(out[4].Reason == RigidSkip::ParentSkipped);   // under a refused parent
    CHECK(out[5].Reason == RigidSkip::ParentUnwritten); // the solver left that hand native: nothing to do
    CHECK(out[6].Reason == RigidSkip::ParentUnwritten); // natural all the way down
    REQUIRE(out[7].Valid);
    CHECK(out[7].Store);
    CHECK(SameBits(out[7].Target, target[0])); // ignore-local: the parent world itself

    // Refusals of a single item under the committed hand.
    const auto one = [&](const RigidRead& r, float maxTravel = 512.f)
    {
        return DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], r, maxTravel);
    };
    RigidRead r = reads[0];
    r.Readable = false;
    CHECK(one(r).Reason == RigidSkip::Unreadable);
    r = reads[0];
    r.Matches = false;
    CHECK(one(r).Reason == RigidSkip::Changed);
    // a collision object no longer refuses at runtime; an exact compose is written as collision_composed (the
    // static collision refusal, items[3] above, remains for an unanalysed class).
    r = reads[0];
    r.Collision = true;
    CHECK(one(r).Valid);
    CHECK(one(r).CollisionComposed);
    CHECK(one(r).Store);
    CHECK(RigidOutcome(one(r)) == kRigidOutcomeCollisionComposed);
    r.World[10] += .01f; // close but not exact: the non-collision rule writes it, the collision rule does not
    CHECK(one(r).Reason == RigidSkip::CollisionDriven);
    r = reads[0];
    r.World[10] += 1.f; // not what the engine composed from the parent and local
    CHECK(one(r).Reason == RigidSkip::NotComposed);
    CHECK_FALSE(one(r).Exact);
    r = reads[0];
    r.World[10] += .01f; // float noise: close, not exact, still written
    CHECK(one(r).Valid);
    CHECK_FALSE(one(r).Exact);
    r = reads[0];
    r.Local[2] = std::numeric_limits<float>::infinity();
    CHECK(one(r).Reason == RigidSkip::Invalid);
    CHECK(one(reads[0], 5.f).Reason == RigidSkip::Invalid); // travel bound
    r = reads[0];
    r.Local[0] *= 3.f;
    r.World = ComposeWorld(native[0], r.Local); // composed, but not a rotation
    CHECK(one(r).Reason == RigidSkip::Invalid);
    // Unchanged parent: valid, nothing to store; the scale bits stay native while the parent's committed scale bits are its
    // native ones (every commit).
    const auto same = DecideRigid(items[0], true, RigidSkip::None, native[0], native[0], reads[0], 512.f);
    CHECK(same.Valid);
    CHECK_FALSE(same.Store);
    const Transform nativeScaled = Posed(glm::angleAxis(.4f, glm::vec3{0.f, 1.f, 0.f}), {11000.f, -71000.f, 5600.f}, 1.03f);
    r = readOf(nativeScaled, weaponLocal);
    Transform targetSameScale = target[0];
    targetSameScale[12] = nativeScaled[12];
    const auto keptScale = DecideRigid(items[0], true, RigidSkip::None, targetSameScale, nativeScaled, r, 512.f);
    REQUIRE(keptScale.Valid);
    CHECK(std::memcmp(&keptScale.Target[12], &r.World[12], sizeof(float)) == 0);
    // (changed rule): a parent whose committed scale differs from its native scale (only the root-scale pass does that)
    // passes it on as the engine's compose would: the target is ComposeWorld(committed parent, local), scale included.
    const auto composedScale = DecideRigid(items[0], true, RigidSkip::None, target[0], nativeScaled, r, 512.f);
    REQUIRE(composedScale.Valid);
    CHECK(SameBits(composedScale.Target, ComposeWorld(target[0], weaponLocal)));
    CHECK(composedScale.Target[12] == weaponLocal[12] * target[0][12]);
    // Identity-like commit (parent moved by float noise only): the rigid world moves by float noise only.
    Transform nudged = native[0];
    nudged[9] += .004f;
    const auto tiny = DecideRigid(items[0], true, RigidSkip::None, nudged, native[0], reads[0], 512.f);
    REQUIRE(tiny.Valid);
    CHECK(tiny.Travel < .01f);
    CHECK(tiny.AngleDeg < .01f);
    // An item whose parent index is not earlier (malformed list) is refused, never read through.
    std::array<RigidItem, 8> malformed = items;
    malformed[0].ParentEntry = -1;
    malformed[0].ParentRigid = 3;
    std::array<RigidResult, 8> out2{};
    SolveRigid(malformed, 1, reads, written, refusedNone, target, native, 4, out2, 512.f);
    CHECK(out2[0].Reason == RigidSkip::Unverified);
    CHECK_FALSE(out2[0].Store);

    // (review F10): a parent entry the flat validation refused is counted apart from the natural case, down the chain.
    std::array<bool, 4> refusedLeft{false, true, false, false};
    std::array<RigidResult, 8> out3{};
    CHECK(SolveRigid(items, items.size(), reads, written, refusedLeft, target, native, 4, out3, 512.f) == 3); // the hand's items unchanged
    CHECK(out3[5].Reason == RigidSkip::ParentRefused);
    CHECK(out3[6].Reason == RigidSkip::ParentRefused);
    CHECK_FALSE(out3[5].Store);
    CHECK_FALSE(out3[6].Store);
    CHECK(out3[4].Reason == RigidSkip::ParentSkipped); // a rigid-level refusal still reads parent_skipped below it
    CHECK(SameBits(out3[1].Target, out[1].Target));
    // A written entry is never ParentRefused, whatever the refused flag says (it cannot be both).
    std::array<bool, 4> refusedHand{true, false, false, false};
    std::array<RigidResult, 8> out4{};
    CHECK(SolveRigid(items, items.size(), reads, written, refusedHand, target, native, 4, out4, 512.f) == 3);
    CHECK(out4[0].Valid);
}

TEST_CASE("a collision-owning node is written only when its native world is the exact compose", "[body_pose_commit]")
{
    // The K4 rehearsal's rigid topology (topology 1): flat entries 0 = WEAPON (right-hand weapon bone) and 1 = WeaponAxe
    // (belt), both written. Items: 0 "Weapon  (00013790)" (node owning a collision object, under WEAPON), 1-3 its
    // geometries (WarAxe01:0, WarAxeBloodFX01, WarAxeBloodLighting01), 4 "Scb" (geometry under WeaponAxe).
    const std::array<bool, 2> written{true, true}, refusedNone{};
    std::array<Transform, 2> native{}, target{};
    native[0] = Posed(glm::angleAxis(.4f, glm::normalize(glm::vec3{.1f, 1.f, .2f})), {11003.5f, -71012.25f, 5604.75f});
    target[0] = Posed(glm::angleAxis(1.7f, glm::normalize(glm::vec3{.3f, 1.f, -.2f})), {11021.f, -70985.5f, 5638.25f});
    native[1] = Posed(glm::angleAxis(-.9f, glm::vec3{0.f, 0.f, 1.f}), {10990.f, -71030.f, 5580.f});
    target[1] = Posed(glm::angleAxis(-.6f, glm::vec3{0.f, 0.f, 1.f}), {10992.f, -71027.f, 5583.f});
    std::array<RigidItem, 5> items{};
    items[0].ParentEntry = 0;
    items[1].ParentRigid = items[2].ParentRigid = items[3].ParentRigid = 0;
    items[4].ParentEntry = 1;
    const Transform weaponLocal = Posed(glm::angleAxis(-1.3f, glm::normalize(glm::vec3{1.f, .2f, .1f})), {-1.5f, 2.25f, -9.f});
    const std::array<Transform, 3> geometryLocal{Identity(), Posed(glm::angleAxis(.2f, glm::vec3{0.f, 0.f, 1.f}), {0.f, 0.f, 24.f}), Identity(0.f, 1.f, 26.f)};
    const Transform scbLocal = Posed(glm::angleAxis(.5f, glm::vec3{1.f, 0.f, 0.f}), {3.f, -2.f, 1.f});
    const auto readOf = [](const Transform& parentWorld, const Transform& local, bool collision = false)
    {
        RigidRead r{};
        r.Readable = r.Matches = true;
        r.Collision = collision;
        r.Local = local;
        r.World = ComposeWorld(parentWorld, local); // what EEC1A0 (collision arm) and EEC042 store: the engine's compose
        return r;
    };
    std::array<RigidRead, 5> reads{};
    reads[0] = readOf(native[0], weaponLocal, true);
    for (size_t g = 0; g < 3; ++g)
        reads[1 + g] = readOf(reads[0].World, geometryLocal[g]);
    reads[4] = readOf(native[1], scbLocal);
    const auto outcomes = [](const std::array<RigidResult, 5>& results)
    {
        std::vector<std::string> names;
        for (const auto& r : results)
            names.emplace_back(RigidOutcomeName(RigidOutcome(r)));
        return names;
    };

    // 1. Composed by the engine this pass (EEC1A0 alone, or just before its push into the Havok body): written, and its
    //    children follow from the committed weapon world. Only worlds are produced: the collision object is no input.
    std::array<RigidResult, 5> out{};
    CHECK(SolveRigid(items, items.size(), reads, written, refusedNone, target, native, 2, out, 512.f) == 5);
    CHECK(outcomes(out) == std::vector<std::string>{"collision_composed", "written", "written", "written", "written"});
    REQUIRE(out[0].Valid);
    CHECK(out[0].Exact);
    CHECK(out[0].Collision);
    CHECK(out[0].CollisionComposed);
    CHECK(out[0].CollisionDevRotation == 0.f);
    CHECK(out[0].CollisionDevTravel == 0.f);
    Transform expectedWeapon = ComposeWorld(target[0], weaponLocal);
    expectedWeapon[12] = reads[0].World[12];
    CHECK(SameBits(out[0].Target, expectedWeapon));
    for (size_t g = 0; g < 3; ++g)
    {
        REQUIRE(out[1 + g].Valid);
        CHECK_FALSE(out[1 + g].Collision);
        CHECK_FALSE(out[1 + g].CollisionComposed);
        Transform expected = ComposeWorld(out[0].Target, geometryLocal[g]);
        expected[12] = reads[1 + g].World[12];
        CHECK(SameBits(out[1 + g].Target, expected));
    }
    // The axe head (item 3) moves rigidly with the committed hand: its offset in hand space is the native one.
    const glm::vec3 headNative = glm::transpose(Rows(native[0])) * (At(reads[3].World) - At(native[0]));
    const glm::vec3 headTarget = glm::transpose(Rows(target[0])) * (At(out[3].Target) - At(target[0]));
    CHECK(glm::length(headNative - headTarget) < .01f);
    CHECK(out[3].Travel > 20.f);

    // 2. One ulp off in one element: not the engine's compose of these inputs, so the collision object set it. Refused;
    //    its children are parent_skipped. The same world on a non-collision item is written (the tolerance rule).
    auto off = reads;
    off[0].World[10] = std::nextafter(off[0].World[10], 0.f);
    std::array<RigidResult, 5> out2{};
    CHECK(SolveRigid(items, items.size(), off, written, refusedNone, target, native, 2, out2, 512.f) == 1); // Scb only
    CHECK(outcomes(out2) == std::vector<std::string>{"collision_driven", "parent_skipped", "parent_skipped", "parent_skipped", "written"});
    CHECK(out2[0].Collision);
    CHECK_FALSE(out2[0].CollisionComposed);
    CHECK_FALSE(out2[0].Store);
    CHECK_FALSE(out2[0].Exact);
    CHECK(out2[0].CollisionDevTravel > 0.f);
    CHECK(out2[0].CollisionDevTravel < .01f);
    CHECK(out2[0].CollisionDevRotation == 0.f);
    auto plain = off[0];
    plain.Collision = false;
    CHECK(DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], plain, 512.f).Valid);

    // 3. Driven by a dynamic body (105CE50 -> 106B160): the world is the body's, a step behind the moving hand.
    auto lag = reads[0];
    lag.World = ComposeWorld(Posed(glm::angleAxis(.43f, glm::normalize(glm::vec3{.1f, 1.f, .2f})), {11005.f, -71010.f, 5606.f}), weaponLocal);
    const auto driven = DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], lag, 512.f);
    CHECK(driven.Reason == RigidSkip::CollisionDriven);
    CHECK(driven.CollisionDevTravel > 1.f);
    CHECK(driven.CollisionDevRotation > .01f);

    // 4. kSetLocal (collision flags bit 3): 106B160 also sets local = inverse(parent) x body. parent x local is then
    //    within rounding of the world, inside the not_composed tolerance: only bit identity separates the two.
    {
        const Transform body = Posed(glm::angleAxis(2.1f, glm::normalize(glm::vec3{-.4f, .3f, 1.f})), {11007.37f, -71008.91f, 5611.43f});
        Transform local = Identity();
        const glm::mat3 p = Rows(native[0]);
        const glm::mat3 l = glm::transpose(p) * Rows(body);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                local[row * 3 + column] = l[column][row];
        const glm::vec3 t = glm::transpose(p) * (At(body) - At(native[0])) / native[0][12];
        local[9] = t.x;
        local[10] = t.y;
        local[11] = t.z;
        local[12] = body[12] / native[0][12];
        RigidRead setLocal{};
        setLocal.Readable = setLocal.Matches = setLocal.Collision = true;
        setLocal.Local = local;
        setLocal.World = body;
        const Transform recomposed = ComposeWorld(native[0], local);
        REQUIRE_FALSE(SameBits(recomposed, body));
        CHECK(NearTransform(recomposed, body, kRigidRotationTolerance, kRigidTravelTolerance, kRigidScaleTolerance));
        const auto decided = DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], setLocal, 512.f);
        CHECK(decided.Reason == RigidSkip::CollisionDriven);
        CHECK(decided.CollisionDevTravel < kRigidTravelTolerance);
        auto asPlain = setLocal;
        asPlain.Collision = false;
        CHECK(DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], asPlain, 512.f).Valid); // rule would write it
    }

    // 5. Everything else is as for any item: the static refusal (unanalysed class or failed guards) wins and never reaches
    //    the compose test; a changed collision object (Matches false), invalid values, the travel bound and an unchanged
    //    parent behave as before; under a parent the solver left native nothing is evaluated.
    auto staticItems = items;
    staticItems[0].Static = RigidSkip::Collision;
    std::array<RigidResult, 5> out3{};
    CHECK(SolveRigid(staticItems, staticItems.size(), reads, written, refusedNone, target, native, 2, out3, 512.f) == 1);
    CHECK(outcomes(out3) == std::vector<std::string>{"collision", "parent_skipped", "parent_skipped", "parent_skipped", "written"});
    CHECK_FALSE(out3[0].Collision);
    auto r = reads[0];
    r.Matches = false;
    CHECK(DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], r, 512.f).Reason == RigidSkip::Changed);
    r = reads[0];
    r.World[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], r, 512.f).Reason == RigidSkip::Invalid);
    const auto bounded = DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], reads[0], 5.f);
    CHECK(bounded.Reason == RigidSkip::Invalid);
    CHECK(bounded.CollisionComposed); // composed, but over the travel bound
    CHECK(RigidOutcome(bounded) == static_cast<uint8_t>(RigidSkip::Invalid));
    const auto same = DecideRigid(items[0], true, RigidSkip::None, native[0], native[0], reads[0], 512.f);
    CHECK(same.Valid);
    CHECK_FALSE(same.Store);
    CHECK(RigidOutcome(same) == kRigidOutcomeUnchanged);
    const std::array<bool, 2> handNative{false, true};
    std::array<RigidResult, 5> out4{};
    CHECK(SolveRigid(items, items.size(), reads, handNative, refusedNone, target, native, 2, out4, 512.f) == 1);
    CHECK(outcomes(out4) == std::vector<std::string>{"parent_unwritten", "parent_unwritten", "parent_unwritten", "parent_unwritten", "written"});
    CHECK_FALSE(out4[0].Collision);
    // Ignore-local (node flags bit 9) on a collision node: EEC1A0 copies the parent world; exact, so it follows the parent.
    r = reads[0];
    r.Flags = kIgnoreLocalFlag;
    r.World = native[0];
    const auto copied = DecideRigid(items[0], true, RigidSkip::None, target[0], native[0], r, 512.f);
    REQUIRE(copied.Valid);
    CHECK(copied.CollisionComposed);
    CHECK(SameBits(copied.Target, target[0]));
}

namespace
{
struct ArchiveFrame
{
    int Index;
    uint32_t Ms;
    std::array<uint32_t, VRPose::kBoneCount> Words;
};
// Real wire words from the M1 replay archive, so the plausibility rule is tested on the data it was derived from.
const ArchiveFrame kArchive[] = {
    // source.pose (SHA256 632b938f3aed139157b076a7cb161ec84e0d211e6e2e716c89caa725f68bc551, 458 frames), decoded with make_held_archive.py: frame index, archive ms, 19 wire words.
    {0,
     52,
     {0x8220169F, 0x84E03547, 0x842014F7, 0x7C2016BB, 0x19CE498D, 0x5BAAF83D, 0x74767966, 0xAA2B5099, 0xE58EC629, 0xA5A93835, 0x8E7516BE, 0x57EBCF44, 0x7FDEF75B, 0x7BE8F6AD,
      0x805D76BD, 0x78B4B771, 0x8D140AC9, 0x8910EAC9, 0x90B289F9}},
    {1,
     104,
     {0x699F4B34, 0x7EA0DB94, 0x83A14BA0, 0x7BE016BB, 0x7029B0E2, 0x41A5CB08, 0x83A9ABE0, 0xAA2B5099, 0x46368C8E, 0x6A9958F9, 0x7631B989, 0x57EBCF48, 0x7FDF0757, 0x7C28E6AD,
      0x7F9F56BD, 0x7934A771, 0x8C948AC5, 0x88D16AC1, 0x90B2B9F9}},
    {2,
     166,
     {0x825FB647, 0x849FD4F3, 0x839FB4A3, 0x7C2016BB, 0x1A4D49D1, 0x5B2B5839, 0x73F65966, 0xAA2B5099, 0xE68DC5F9, 0xA92947F1, 0x8D35566E, 0x57EBCF48, 0x7FDF074F, 0x7CA8F6B1,
      0x7F6126C5, 0x79749771, 0x89962AB5, 0x8612DAA9, 0x9072D9F5}},
    {3,
     229,
     {0x825FC67F, 0x849FD52B, 0x839FB4D7, 0x7C2016BB, 0x1A8DF9AD, 0x5B2B0835, 0x73B67966, 0xAA2B5095, 0xE64E661D, 0xA7291805, 0x8DB5768E, 0x57EBCF48, 0x7FDF1747, 0x7C6936B5,
      0x7FDF76C5, 0x79B48771, 0x8A566AB1, 0x86D30AA5, 0x8FF309F5}},
    {4,
     282,
     {0x825FC69F, 0x845FD547, 0x839FB4F7, 0x7C6016BB, 0x1ACE4995, 0x5AEB082D, 0x73B63966, 0xAA2B5095, 0xE64EB635, 0xA6693819, 0x8D7536A2, 0x57EBCF48, 0x7FDF273F, 0x7BE976B9,
      0x801E26C9, 0x79F47771, 0x8D1A0A95, 0x834FFA9D, 0x903339F1}},
    {5,
     345,
     {0x825FC6B7, 0x845FD55F, 0x835FB50F, 0x7C6016BB, 0x1ACE9985, 0x5AAB0825, 0x73B5F962, 0xAA2B6095, 0xE64EF649, 0xA5E94821, 0x8D34F6AE, 0x57EBCF4C, 0x7FDF3737, 0x7B69A6C1,
      0x801D36CD, 0x7A345775, 0x8E9CBA7D, 0x810E8A91, 0x90B379ED}},
    {70,
     3976,
     {0x825FF73F, 0x83DFF5E3, 0x82DFE58F, 0x7CA016BB, 0x1B102929, 0x5AEA3819, 0x73B6195E, 0xA9EB6091, 0xE55076A1, 0xA568A831, 0x8DF526B6, 0x57EBCF4C, 0x7FDF972B, 0x7AE97705,
      0x7FDA0709, 0x77B54775, 0x8C9FBA75, 0x8294AA81, 0x8FB4E9D9}},
    {71,
     4039,
     {0x825FE743, 0x841FE5E7, 0x831FD593, 0x7CA016BB, 0x1B503929, 0x5AEA3819, 0x73B61962, 0xA9EB6091, 0xE55076A5, 0xA5289831, 0x8DF526BA, 0x57EBCF4C, 0x7FDF872F, 0x7AE99705,
      0x7FD9F70D, 0x76F58769, 0x8C9FCA75, 0x82549A81, 0x8F34E9D9}},
    {72,
     4091,
     {0x825FE747, 0x841FE5EB, 0x831FD597, 0x7CA016BB, 0x1B504925, 0x5AAA3819, 0x7376095E, 0xA9EB6091, 0xE59086A5, 0xA5289831, 0x8DF526BA, 0x57EBCF4C, 0x7FDF7733, 0x7AE9B6F9,
      0x7FD9D701, 0x76B55769, 0x8C9FDA75, 0x82548A7D, 0x8E74D9D5}},
    {73,
     4144,
     {0x7EDFC747, 0x7E9FD5EF, 0x7DDFC59B, 0x7CA016BB, 0x1A90C959, 0x5869B819, 0x73B5C97E, 0xA9EB6091, 0x2FF7D6AB, 0xF06CE84E, 0xF869977A, 0x57EBCF4C, 0x7FDF7733, 0x7AE9B6ED,
      0x8019C6F5, 0x76B4E765, 0x8C5FDA71, 0x82547A7D, 0x8E34C9D5}},
    {74,
     4197,
     {0x84200747, 0x86E005EF, 0x85DFE59B, 0x7CA016BB, 0x1B900901, 0x5C2A7815, 0x7376394E, 0xA9EB6091, 0x34B8066F, 0xF12CA862, 0xF9295792, 0x57EBCF4C, 0x7FDF6733, 0x7AA9C6E1,
      0x8059C6E9, 0x7734A765, 0x8C5FDA71, 0x82548A79, 0x8E34A9D5}},
    {75,
     4249,
     {0x83E00747, 0x86A005EF, 0x859FF59B, 0x7CE016BB, 0x1B500905, 0x5BAA6819, 0x73763952, 0xA9EB6091, 0x3478066F, 0xF0ACA866, 0xF8A95792, 0x57ABCF4C, 0x7FDF6733, 0x7AA9C6D5,
      0x8099B6DD, 0x77B47765, 0x8C5FDA6D, 0x82548A75, 0x8EB499D9}},
    {76,
     4312,
     {0x83A00747, 0x866015EF, 0x855FF59B, 0x7CE016BB, 0x1B500909, 0x5B6A5819, 0x73763956, 0xA9EB6091, 0x34780673, 0xF06CA862, 0xF8695792, 0x57ABCF50, 0x7FDF6733, 0x7AA9D6CD,
      0x80D9B6D5, 0x78345765, 0x8C5FDA69, 0x82548A75, 0x8FF499DD}},
    {162,
     9004,
     {0x85DF9753, 0x89DF95F7, 0x88DF75A7, 0x7EA006BB, 0x1D8FB8DD, 0x5B2A9801, 0x7236294A, 0x5694707C, 0x3777E65F, 0xEEECA866, 0xF7295796, 0x576BAF60, 0x7FDEF75F, 0x7A29E6B1,
      0x811986B9, 0x7874C771, 0x8CE05A69, 0x8193CA75, 0x90B269F9}},
    {163,
     9057,
     {0x85DF9753, 0x899F95F7, 0x889F75A7, 0x7EA006BB, 0x1D4FC8E1, 0x5AEA8801, 0x7236194A, 0x5694707C, 0x3777E65F, 0xEEECA866, 0xF7294796, 0x576BAF64, 0x7FDEF75B, 0x7A29F6AD,
      0x811976B9, 0x78B4B771, 0x8CE06A69, 0x8193CA75, 0x90B289F9}},
    {164,
     9109,
     {0x859F9753, 0x895F95F7, 0x885F75A3, 0x7EA006BB, 0x1D4FC8E1, 0x5AEA8801, 0x7236194A, 0x5694707C, 0x3777E663, 0xEEECA866, 0xF7295796, 0x576BAF64, 0x7FDF0757, 0x7A29F6AD,
      0x811976B9, 0x78F4A771, 0x8CE06A65, 0x8193CA71, 0x90B2A9F9}},
    {165,
     9161,
     {0x81DF160B, 0x835F34B7, 0x825F1467, 0x7DE006BB, 0x1BCC29F5, 0x54AE17FD, 0x7872396A, 0xA9AB7085, 0xE88C95FD, 0xA4ACF845, 0x847126F2, 0x576BBF5C, 0x7FDF074F, 0x6EBC0765,
      0x869036ED, 0x7934A771, 0x986EA9C5, 0x6E817A25, 0x9072D9F5}},
    {166,
     9215,
     {0x831F057B, 0x851F242B, 0x841F03DF, 0x7DE006BB, 0x1C4A6A4D, 0x56ADF809, 0x7673495A, 0xA9AB7085, 0xE88B458D, 0xA5AC9825, 0x863206D2, 0x576BBF5C, 0x7FDF1747, 0x6EFC2769,
      0x865106F5, 0x79B49771, 0x9530999D, 0x6E0439E9, 0x9032F9F5}},
    {167,
     9268,
     {0x82DED4F7, 0x84DF03AB, 0x83DEE35F, 0x7E2006BB, 0x1D08FAA9, 0x56EDB815, 0x74742966, 0xA9AB7081, 0xE889D539, 0xA5AC3809, 0x87F306BE, 0x576BBF5C, 0x7FDF1743, 0x6F7BE769,
      0x861166F9, 0x79B47771, 0x92F1C985, 0x6D86E9C1, 0x8FB319F1}},
    {168,
     9319,
     {0x82DEB48B, 0x849EE347, 0x839EC2FB, 0x7E2006BB, 0x1DC7DAF5, 0x572D7821, 0x72F4D96E, 0xA9AB7081, 0xE808B4F9, 0xA56BE7F9, 0x8973D6B2, 0x576BBF5C, 0x7FDF273B, 0x6FFB4769,
      0x861156FD, 0x79F46771, 0x91724975, 0x6D8919A9, 0x907359F1}},
    {436,
     23849,
     {0x859F174B, 0x885F05EF, 0x875EF59F, 0x849FD6BB, 0x1F0F78E9, 0x58A9B79D, 0x6DF5C926, 0x581420C0, 0xE9117671, 0xA36927B5, 0x8835667E, 0x55AB4F5D, 0x7FDF2753, 0x7A69A6A9,
      0x8119E6B5, 0x7934E765, 0x8C1FDA55, 0x82544A5D, 0x8CF3C9E1}},
    {437,
     23911,
     {0x855F174B, 0x881F15EF, 0x871EF59F, 0x84DFD6BB, 0x1F0F78E9, 0x58A9A79D, 0x6DF5C926, 0x581420C0, 0xE9117671, 0xA36927B1, 0x8835667E, 0x55AB4F5D, 0x7FDF274F, 0x7A69A6A9,
      0x8119E6B5, 0x7974E765, 0x8C1FDA51, 0x82544A5D, 0x8DB3C9E1}},
    {438,
     23975,
     {0x855F174B, 0x87DF15EF, 0x86DF059F, 0x84DFD6BB, 0x1ECF88ED, 0x58A9979D, 0x6DF5C92A, 0x581420C0, 0xE8D16675, 0xA36937B1, 0x8835667E, 0x55AB4F5D, 0x7FDF2747, 0x7A69B6A9,
      0x8119D6B5, 0x79F4D769, 0x8BDFDA51, 0x82544A55, 0x8E73E9E1}},
    {439,
     24028,
     {0x851F274B, 0x87DF15EF, 0x869F059F, 0x84DFD6BB, 0x1ECF88ED, 0x5869979D, 0x6DF5B92A, 0x581420C0, 0xE8D16679, 0xA36937B1, 0x87F5667E, 0x55AB4F5D, 0x7FDF3743, 0x7A69C6A9,
      0x8159D6B5, 0x7A74D769, 0x8BDFDA4D, 0x82544A55, 0x8F3409E1}},
    {440,
     24090,
     {0x851F174F, 0x879F15F3, 0x869F059F, 0x84DFD6BB, 0x1ECF98ED, 0x58698799, 0x6DF5B92A, 0x581420C0, 0xE8D16679, 0xA32947AD, 0x87F5667E, 0x55AB4F59, 0x7FDF373B, 0x7A69D6A9,
      0x8159C6B5, 0x7AF4C76D, 0x8BDFDA45, 0x82544A4D, 0x8FB439E1}},
    {441,
     24143,
     {0x699F5998, 0x7F201970, 0x84604980, 0x851FD6BB, 0x822CE15E, 0x4665B1D4, 0x8ED9C158, 0x581420C4, 0x5C72FD96, 0x5156CE65, 0x89A82E95, 0x55AB4F59, 0x7FDF3733, 0x7BA69695,
      0x7F9D26A5, 0x7B74B76D, 0x891C4A65, 0x8658CA59, 0x903469E1}},
    {442,
     24197,
     {0x699F5978, 0x7F20094C, 0x84603958, 0x851FD6BB, 0x892EF18E, 0x43610110, 0x78D6C0F0, 0x581420C4, 0x5A73ED82, 0x4D9CAD11, 0x752CAD85, 0x55AB4F59, 0x7FDF372F, 0x7D26B69D,
      0x7E6346B1, 0x7BF4A771, 0x88A15A2D, 0x80971A29, 0x9034A9DD}},
    {443,
     24249,
     {0x695F595C, 0x7F200920, 0x84602930, 0x851FD6BB, 0x8EB091BA, 0x4A5B9074, 0x61962050, 0x581420C4, 0x5934AD72, 0x4EA2ACB5, 0x65ECFCF5, 0x55AB4F59, 0x7F9F4727, 0x7D68B6B5,
      0x7EA536C5, 0x7C749771, 0x89A4BA05, 0x7D14AA09, 0x9074B9DD}},
    {444,
     24302,
     {0x695F5940, 0x7F1FF900, 0x84601910, 0x851FD6BB, 0x92B1A1DE, 0xB3A7A0D5, 0xA5AB40ED, 0x585410C4, 0x58352D66, 0x50E78CB5, 0x5AEBDC99, 0x55AB4F59, 0x7FDF4723, 0x7DAA16C1,
      0x7EA696D5, 0x7CF49775, 0x8A6679ED, 0x7B5329F9, 0x9074C9DD}},
    {445,
     24354,
     {0x695F5934, 0x7F1FE8EC, 0x846008FC, 0x851FD6BB, 0x94B221F2, 0xB4695139, 0xA72CE149, 0x585410C4, 0x57758D5E, 0x4F692C95, 0x592D5C7D, 0x55AB4F59, 0x7FDF471F, 0x7DAB06CD,
      0x7EA796DD, 0x7D349775, 0x8B6729E5, 0x7A9219F5, 0x9074B9E1}},
    {446,
     24406,
     {0x695F5930, 0x7F1FE8E8, 0x846008F8, 0x851FD6BB, 0x953231FE, 0xB4EA6171, 0xA82DF17D, 0x585410C4, 0x57358D56, 0x4E6A3C75, 0x57EE5C61, 0x55AB4F59, 0x7FDF471F, 0x7DABA6D1,
      0x7EA846E5, 0x7D34A775, 0x8BE709E9, 0x7A9159FD, 0x8FF4A9E1}},
    {447,
     24458,
     {0x695F593C, 0x7F1FF8F8, 0x84600904, 0x851FD6BB, 0x94F23202, 0xB62B1175, 0xA96E9181, 0x585410C4, 0x5675AD4A, 0x4EAB2C39, 0x57AF3C29, 0x55AB4F59, 0x7FDF471F, 0x7DAC16D9,
      0x7EA8B6E9, 0x7CB4C771, 0x8C6639F5, 0x7B910A0D, 0x9034A9E1}},
    {448,
     24511,
     {0x695F5950, 0x7F1FF914, 0x84601924, 0x851FD6BB, 0x943201FE, 0xB82B5159, 0xAB2ED165, 0x585410C4, 0x5575CD3A, 0x4FEBFBE9, 0x582FFBDD, 0x55AB4F59, 0x7FDF371F, 0x7DAC66D9,
      0x7EA906ED, 0x7C74E771, 0x8CA4CA09, 0x7D115A21, 0x90B4A9E1}},
    {449,
     24574,
     {0x699F5974, 0x7F200940, 0x8460294C, 0x851FD6BB, 0x92B1D1F6, 0xBAEB511D, 0xAD6EB129, 0x581410C4, 0x53F60D2A, 0x522CBB81, 0x59B0BB7D, 0x55AB4F59, 0x7FDF3723, 0x7DAC96D9,
      0x7EA946ED, 0x7BF5076D, 0x8CA29A25, 0x7F525A35, 0x9134A9E1}},
    {450,
     24626,
     {0x699F499C, 0x7F201970, 0x84603980, 0x851FD6BB, 0x913181EA, 0xBE6AF0C9, 0xB06E40D5, 0x581420C4, 0x52364D16, 0x556D8B01, 0x5BF17B01, 0x55AB4F59, 0x7F9F272B, 0x7DACB6DD,
      0x7EA966ED, 0x7BB5376D, 0x8B9F5A49, 0x82D49A51, 0x91B4A9E5}},
    {451,
     24679,
     {0x699F39C4, 0x7F20199C, 0x846049A8, 0x84DFD6BB, 0x8FB141E2, 0x3DD5B138, 0x4C527104, 0x581420C0, 0x50F67D0A, 0x58AE2A8D, 0x5DF21A95, 0x55AB4F59, 0x7FDF2733, 0x7DACC6D9,
      0x7EA976ED, 0x7B75676D, 0x8A1B8A6D, 0x87180A65, 0x91B4B9E5}},
    {452,
     24731,
     {0x699F29E0, 0x7F2019B4, 0x846049C4, 0x84DFD6BB, 0x8EB121DE, 0x39D6A1CC, 0x49537180, 0x581420C0, 0x4FF69D02, 0x5B2E9A39, 0x5FB28A49, 0x55AB4F5D, 0x7FDF173F, 0x7DACA6D5,
      0x7EA946E9, 0x7AF5876D, 0x89DBAA6D, 0x86D81A65, 0x91B4B9E5}},
    {453,
     24783,
     {0x699F19E4, 0x7F1FF9A8, 0x846029B8, 0x849FD6BB, 0x8F3141EA, 0x3AD63194, 0x49D30150, 0x581420C0, 0x50767D0A, 0x5AAEBA49, 0x5FB2AA55, 0x55AB4F5D, 0x7F9F0747, 0x7D6C36D1,
      0x7E68B6E1, 0x7AB5B76D, 0x899BBA71, 0x86983A65, 0x91B4B9E5}},
    {454,
     24836,
     {0x699EF9C8, 0x7F1FD974, 0x845FF980, 0x831FE6BB, 0x9131C206, 0x3C95B118, 0x4B1260EC, 0x579430B0, 0x52362D2A, 0x59EECA95, 0x5F72AA9D, 0x562B6F6D, 0x7FDEF753, 0x7C6E36E1,
      0x802466ED, 0x7A75D76D, 0x891B0A79, 0x87593A69, 0x9174B9E5}},
    {455,
     24889,
     {0x69DED980, 0x7F5F98F8, 0x849FA908, 0x7C2016BB, 0x9672D246, 0x3AD670D0, 0xB76CF0DD, 0xAA2B5099, 0x57753D72, 0x5CAF5AFD, 0x62B32B05, 0x57EBCF48, 0x7FDEF75B, 0x7DEFC6FD,
      0x841FF715, 0x79F5E76D, 0x889B9A71, 0x85981A65, 0x90F4A9E5}},
    {456,
     24941,
     {0x69DED9B0, 0x7F5FA938, 0x849FC948, 0x752046BB, 0x94F2A242, 0x2F5B52E4, 0x3ED8023C, 0xABEAF0E5, 0x56755D6E, 0x6BF0799D, 0x6E7449B9, 0x59EC2EF8, 0x7FDEF763, 0x80EFE711,
      0x879D673D, 0x7975F76D, 0x861B5A71, 0x8357DA61, 0x907489E5}},
    {457,
     24994,
     {0x69DED9C8, 0x7F5FB960, 0x849FD96C, 0x6FE076BB, 0x93F2723A, 0x2BDE6450, 0x3B1B3360, 0xAD2A9121, 0x55B56D66, 0x73F0C8D5, 0x74349901, 0x5B2C7EBC, 0x7FDEF763, 0x82EF5719,
      0x891BA755, 0x78F5E769, 0x831B3A6D, 0x8057AA59, 0x903469E5}},
};
const ArchiveFrame& ArchiveAt(int aIndex)
{
    for (const auto& f : kArchive)
        if (f.Index == aIndex)
            return f;
    FAIL("frame " << aIndex << " is not in the fixture");
    return kArchive[0];
}
VRPose ArchivePose(int aIndex)
{
    const auto& f = ArchiveAt(aIndex);
    VRPose p{};
    p.HasData = true;
    p.HasLegs = true;
    p.HasHips = true;
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        p.Bones[i].Unpack(f.Words[i]); // the receiver's own unpack
    p.HipOffset[0] = 0.f;
    p.HipOffset[1] = -18.f;
    p.HipOffset[2] = 65.f;
    return p;
}
// The commit's solver input from a receiver sample (CommitBuildAndStore, rotations/flags/hip only).
void FillInput(const VRPose& acPose, BodyPosePresentation::Input& aIn)
{
    aIn.HasData = acPose.HasData;
    aIn.NoBones = acPose.NoBones;
    aIn.HasLegs = acPose.HasLegs;
    aIn.HasHips = acPose.HasHips;
    aIn.HipOffset = {acPose.HipOffset[0], acPose.HipOffset[1], acPose.HipOffset[2]};
    for (size_t wire = 0; wire < VRPose::kBoneCount; ++wire)
        aIn.Rotations[wire] = static_cast<glm::quat>(acPose.Bones[wire]);
}
struct StreamPoint
{
    uint64_t Tick;
    VRPose VRPoseData;
};
struct StreamStats
{
    uint32_t Passes{}, Admitted{}, Held{}, Released{}, NoCandidate{}, Timeouts{}, Implausible{}, Contaminated{}, Episodes{}, Ends{};
    uint32_t DrawnImplausible{}, AdmittedOver60{}, ArmHeld{};
    float MaxDrawnNeckHead{}, MaxAdmittedNeckHead{}, MaxHeldNeckHead{};
    uint64_t MaxHeldTick{};
    bool LastAdmitted{};
};
// Plays points through RemotePoseInterpolation::Sample at a render rate, with the endpoints SampleWithProvenance reports
// (Before = last point <= tick, After = first point > tick, each the other when missing), into the guard as the commit does.
template <size_t N>
StreamStats PlayStream(
    FlipGuard<N>& aGuard, const std::vector<StreamPoint>& acPoints, double aFromTick, double aToTick, double aFrameMs, uint64_t aGeneration, uint64_t aArmFrom = UINT64_MAX,
    uint64_t aArmTo = 0)
{
    static BodyPosePresentation::Input in; // large: kept off the stack
    StreamStats s{};
    for (double t = aFromTick; t <= aToTick; t += aFrameMs)
    {
        const auto tick = static_cast<uint64_t>(t);
        const StreamPoint* before = nullptr;
        const StreamPoint* after = nullptr;
        for (const auto& p : acPoints)
        {
            if (p.Tick <= tick)
                before = &p;
            else
            {
                after = &p;
                break;
            }
        }
        if (!before)
            before = after;
        if (!after)
            after = before;
        FillInput(RemotePoseInterpolation::Sample(acPoints, tick), in);
        const auto candidate = FlipPoseFrom(in, t + 100., aGeneration, before->Tick, after->Tick); // pass time = playback + delay
        FlipPose use{};
        const auto d = aGuard.Step(candidate, use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
        ++s.Passes;
        s.Implausible += d.Implausible ? 1u : 0u;
        s.Contaminated += d.Contaminated ? 1u : 0u;
        s.Episodes += d.EpisodeStart ? 1u : 0u;
        s.Ends += d.EpisodeEnd ? 1u : 0u;
        s.Timeouts += d.TimeoutStart ? 1u : 0u;
        s.LastAdmitted = !d.Hold && !d.Release && d.Checked;
        if (d.Release)
        {
            ++s.Released;
            s.NoCandidate += d.NoCandidate ? 1u : 0u;
            continue; // nothing drawn from the source: native
        }
        const auto drawn = Plausibility(use, kFlipImplausibleDeg);
        s.DrawnImplausible += drawn.Reason != FlipReason::None ? 1u : 0u;
        s.MaxDrawnNeckHead = std::max(s.MaxDrawnNeckHead, drawn.NeckHeadDeg);
        if (d.Hold)
        {
            ++s.Held;
            s.MaxHeldTick = std::max(s.MaxHeldTick, d.HeldTick);
            s.MaxHeldNeckHead = std::max(s.MaxHeldNeckHead, drawn.NeckHeadDeg);
            if (candidate.BeforeTick <= aArmTo && candidate.AfterTick >= aArmFrom)
                ++s.ArmHeld;
            CHECK(use.AfterTick == d.HeldTick);
        }
        else
        {
            ++s.Admitted;
            CHECK(use.TimeMs == candidate.TimeMs); // admitted: the sample itself is solved
            s.MaxAdmittedNeckHead = std::max(s.MaxAdmittedNeckHead, d.NeckHeadDeg);
            s.AdmittedOver60 += d.NeckHeadDeg > 60.f ? 1u : 0u;
        }
    }
    return s;
}
std::vector<StreamPoint> ArchiveStream(int aFirst, int aLast, uint64_t aOffset = 1000)
{
    std::vector<StreamPoint> points;
    for (int k = aFirst; k <= aLast; ++k)
        points.push_back({aOffset + ArchiveAt(k).Ms, ArchivePose(k)});
    return points;
}
uint64_t TickOf(const std::vector<StreamPoint>& acPoints, size_t aI)
{
    return acPoints[aI].Tick;
}
float BoneStepDeg(int aA, int aB, size_t aBone)
{
    return AngleDeg(static_cast<glm::quat>(ArchivePose(aA).Bones[aBone]), static_cast<glm::quat>(ArchivePose(aB).Bones[aBone]));
}
FlipPose Synthetic(double aMs, uint64_t aBefore, uint64_t aAfter, float aNeckDeg = 0.f, float aPelvisDeg = 0.f, bool aLegs = true, uint64_t aGeneration = 1)
{
    BodyPosePresentation::Input in;
    in.HasData = true;
    in.HasLegs = aLegs;
    in.HasHips = true;
    in.HipOffset = {0.f, -18.f, 65.f};
    in.Rotations[kFlipNeck] = glm::angleAxis(glm::radians(aNeckDeg), glm::vec3{0.f, 0.f, 1.f});
    in.Rotations[kFlipPelvis] = glm::angleAxis(glm::radians(aPelvisDeg), glm::vec3{1.f, 0.f, 0.f});
    return FlipPoseFrom(in, aMs, aGeneration, aBefore, aAfter);
}
} // namespace

TEST_CASE("plausibility rule separates the archive's flipped frames from every normal one", "[body_pose_commit]")
{
    CHECK(kFlipImplausibleDeg == 110.f);
    CHECK(kFlipHoldTimeoutMs == 2000.0);
    CHECK(kFlipSpine1 == 0);
    CHECK(kFlipNeck == 2);
    CHECK(kFlipHead == 3);
    CHECK(kFlipPelvis == 12);
    REQUIRE(kFlipReasonNames.size() == static_cast<size_t>(FlipReason::Count));
    CHECK(std::string(kFlipReasonNames[static_cast<size_t>(FlipReason::NeckHead)]) == "neck_head");
    CHECK(std::string(kFlipReasonNames[static_cast<size_t>(FlipReason::Contaminated)]) == "contaminated");
    static BodyPosePresentation::Input in;
    float normalNeckHead{}, normalSpinePelvis{}, flippedNeckHead = 999.f, flippedSpinePelvis = 999.f;
    uint32_t flipped{}, normal{};
    for (const auto& f : kArchive)
    {
        FillInput(ArchivePose(f.Index), in);
        const auto p = Plausibility(FlipPoseFrom(in, f.Ms, 1, f.Ms, f.Ms), kFlipImplausibleDeg);
        const bool expectFlipped = f.Index == 1 || f.Index >= 441;
        CAPTURE(f.Index, p.NeckHeadDeg, p.Spine1PelvisDeg);
        CHECK((p.Reason != FlipReason::None) == expectFlipped);
        if (expectFlipped)
        {
            ++flipped;
            CHECK(p.Reason == FlipReason::NeckHead);
            flippedNeckHead = std::min(flippedNeckHead, p.NeckHeadDeg);
            flippedSpinePelvis = std::min(flippedSpinePelvis, p.Spine1PelvisDeg);
        }
        else
        {
            ++normal;
            normalNeckHead = std::max(normalNeckHead, p.NeckHeadDeg);
            normalSpinePelvis = std::max(normalSpinePelvis, p.Spine1PelvisDeg);
        }
    }
    CHECK(flipped == 18);
    CHECK(normal == 24);
    // Either test alone separates (whole archive, decoded offline: normal <= 57.1 / 44.8, flipped >= 169.8 / 156.5).
    CHECK(normalNeckHead < 60.f);
    CHECK(normalSpinePelvis < 50.f);
    CHECK(flippedNeckHead > 165.f);
    CHECK(flippedSpinePelvis > 150.f);
    // The fixtures hold the archive's two fastest real motions: the right upper arm turns 114.5 and 106.4 degrees in one
    // 53 / 52 ms source step. Plausible: the rule never looks at arms.
    CHECK(BoneStepDeg(72, 73, VRPose::kRightUpperArm) == Approx(114.5f).margin(.6f));
    CHECK(BoneStepDeg(164, 165, VRPose::kRightUpperArm) == Approx(106.4f).margin(.6f));
    CHECK(ArchiveAt(73).Ms - ArchiveAt(72).Ms == 53);
    CHECK(ArchiveAt(165).Ms - ArchiveAt(164).Ms == 52);
    // Spine1-to-Pelvis alone (legs present) and Neck-to-Head alone each trigger; legs absent: Spine1-Pelvis is not measured.
    CHECK(Plausibility(Synthetic(0., 1, 1, 0.f, 120.f), kFlipImplausibleDeg).Reason == FlipReason::Spine1Pelvis);
    CHECK(Plausibility(Synthetic(0., 1, 1, 111.f, 0.f), kFlipImplausibleDeg).Reason == FlipReason::NeckHead);
    CHECK(Plausibility(Synthetic(0., 1, 1, 109.f, 109.f), kFlipImplausibleDeg).Reason == FlipReason::None);
    const auto noLegs = Plausibility(Synthetic(0., 1, 1, 0.f, 170.f, false), kFlipImplausibleDeg);
    CHECK(noLegs.Reason == FlipReason::None);
    CHECK(noLegs.Spine1PelvisDeg == -1.f);
    // A non-unit (non-finite) wire does not trigger: the solver refuses that sample itself.
    FlipPose nan = Synthetic(0., 1, 1);
    nan.Rotation[kFlipNeck] = glm::quat{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f, 0.f};
    CHECK(Plausibility(nan, kFlipImplausibleDeg).Reason == FlipReason::None);
    // The double cover is no rotation.
    FlipPose cover = Synthetic(0., 1, 1);
    cover.Rotation[kFlipHead] = -cover.Rotation[kFlipHead];
    CHECK(Plausibility(cover, kFlipImplausibleDeg).NeckHeadDeg < .01f);
}

TEST_CASE("fast but plausible arm raises are never held", "[body_pose_commit]")
{
    // 114.5 and 106.4 degrees of right upper arm in 53 / 52 ms, through the receiver's interpolation at 60 and 144 fps.
    for (const double frameMs : {1000. / 60., 1000. / 144.})
    {
        CAPTURE(frameMs);
        for (const auto& range : {std::pair<int, int>{70, 76}, std::pair<int, int>{162, 168}})
        {
            FlipGuard<8> guard;
            const auto points = ArchiveStream(range.first, range.second);
            const auto s = PlayStream(guard, points, static_cast<double>(points.front().Tick), static_cast<double>(points.back().Tick) + 30., frameMs, 1);
            CHECK(s.Passes > 15);
            CHECK(s.Admitted == s.Passes);
            CHECK(s.Held == 0);
            CHECK(s.Released == 0);
            CHECK(s.Implausible == 0);
            CHECK(s.Contaminated == 0);
            CHECK(s.Episodes == 0);
        }
    }
    // Synthetic: a 114 degree arm raise in one 53 ms step, the torso unchanged; and an arm that swings 179 degrees.
    FlipGuard<8> guard;
    FlipPose use{};
    FlipPose rest = Synthetic(0., 10, 10);
    CHECK_FALSE(guard.Step(rest, use, kFlipImplausibleDeg, kFlipHoldTimeoutMs).Hold);
    for (const float deg : {114.f, 179.f, 0.f})
    {
        FlipPose raised = Synthetic(53., 10, 11);
        raised.Rotation[VRPose::kRightUpperArm] = glm::angleAxis(glm::radians(deg), glm::vec3{0.f, 1.f, 0.f});
        raised.Rotation[VRPose::kLeftThigh] = glm::angleAxis(glm::radians(deg), glm::vec3{1.f, 0.f, 0.f});
        const auto d = guard.Step(raised, use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
        CHECK(d.Checked);
        CHECK_FALSE(d.Hold);
        CHECK_FALSE(d.Release);
        CHECK(d.Reason == FlipReason::None);
        CHECK(use.TimeMs == 53.);
    }
}

TEST_CASE("the archive's torso flip is held from before the flipped pair, never drawn, and ends on clean ticks", "[body_pose_commit]")
{
    // Frames 436-457 (the 851 ms flip from frame 441 to the end), then the replay loop restarts: frames 0-5 of the next
    // loop, including the one-frame glitch at frame 1. 60 and 144 fps.
    for (const double frameMs : {1000. / 60., 1000. / 144.})
    {
        CAPTURE(frameMs);
        auto points = ArchiveStream(436, 457);
        const uint64_t loop = 1000 + ArchiveAt(457).Ms + 53; // the next loop starts one source interval after frame 457
        for (int k = 0; k <= 5; ++k)
            points.push_back({loop + ArchiveAt(k).Ms, ArchivePose(k)});
        FlipGuard<8> guard;
        const auto s = PlayStream(guard, points, static_cast<double>(points.front().Tick), static_cast<double>(points.back().Tick) + 30., frameMs, 1);
        CHECK(s.Held > 50);     // ~0.9 s at 60 fps
        CHECK(s.Released == 0); // a candidate from before the flip is always there and younger than 2 s
        CHECK(s.Timeouts == 0);
        CHECK(s.DrawnImplausible == 0);            // no implausible pose is ever solved from
        CHECK(s.MaxHeldNeckHead < 60.f);           // the held pose is a normal one
        CHECK(s.MaxHeldTick <= TickOf(points, 3)); // from before the flipped pair (439, 440) -> 441: frame 439 or earlier
        CHECK(s.Episodes >= 1);
        CHECK(s.Ends == s.Episodes); // every episode ended
        CHECK(s.LastAdmitted);       // admitted again on the next loop's clean ticks
        // Residual (documented): before the first implausible pass the receiver already interpolates part of the way
        // toward the flipped tick; those passes are plausible and drawn. At most 3 render frames, never past the limit.
        CHECK(s.AdmittedOver60 <= 3);
        CHECK(s.MaxAdmittedNeckHead < kFlipImplausibleDeg);
    }
}

TEST_CASE("a stream that starts with a flip has nothing to hold and releases to native", "[body_pose_commit]")
{
    FlipGuard<8> guard;
    const auto points = ArchiveStream(0, 5);
    const auto s = PlayStream(guard, points, static_cast<double>(points.front().Tick) - 20., static_cast<double>(points.back().Tick) + 30., 1000. / 60., 1);
    CHECK(s.Released > 0);
    CHECK(s.NoCandidate == s.Released); // no timeout: nothing plausible was left to hold
    CHECK(s.Held == 0);
    CHECK(s.DrawnImplausible == 0);
    CHECK(s.LastAdmitted);
    CHECK(s.Ends == s.Episodes);
}

TEST_CASE("guard mechanics: purge, contamination, timeout, reset, untested samples, ring", "[body_pose_commit]")
{
    FlipGuard<8> g;
    FlipPose use{};
    const auto step = [&](const FlipPose& p)
    {
        return g.Step(p, use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    };
    // Admitted candidates per After tick: (1,2) twice (the newer replaces the older), (2,3), (3,4).
    CHECK_FALSE(step(Synthetic(0., 1, 2, 10.f)).Hold);
    CHECK_FALSE(step(Synthetic(10., 1, 2, 11.f)).Hold);
    CHECK_FALSE(step(Synthetic(20., 2, 3, 12.f)).Hold);
    CHECK_FALSE(step(Synthetic(30., 3, 4, 40.f)).Hold); // interpolating toward tick 4 (which turns out flipped)
    CHECK(g.Count == 3);
    // Implausible in pair (4, 5): candidates with After >= 4 are suspect and purged; the hold is the tick-3 candidate.
    auto d = step(Synthetic(40., 4, 5, 170.f));
    REQUIRE(d.Hold);
    CHECK(d.Implausible);
    CHECK(d.EpisodeStart);
    CHECK(d.Reason == FlipReason::NeckHead);
    CHECK(d.Purged == 1);
    CHECK(d.HeldTick == 3);
    CHECK(use.TimeMs == 20.);
    CHECK(d.HeldAgeMs == Approx(20.));
    CHECK(d.NeckHeadDeg == Approx(170.f).margin(.01f));
    CHECK(d.Spine1PelvisDeg == Approx(0.f).margin(.01f));
    // Plausible again but interpolated out of the bad tick 5: contaminated, still held.
    d = step(Synthetic(56., 5, 6, 30.f));
    CHECK(d.Hold);
    CHECK(d.Contaminated);
    CHECK_FALSE(d.Implausible);
    CHECK(d.Reason == FlipReason::Contaminated);
    CHECK_FALSE(d.EpisodeStart);
    CHECK(d.EpisodeMs == Approx(16.));
    CHECK(use.TimeMs == 20.);
    // Clean (Before 6 > bad tick 5): admitted, the episode ends.
    d = step(Synthetic(70., 6, 7, 20.f));
    CHECK_FALSE(d.Hold);
    CHECK(d.EpisodeEnd);
    CHECK(d.Reason == FlipReason::None);
    CHECK(d.EpisodeMs == Approx(30.));
    CHECK(use.TimeMs == 70.);
    CHECK(g.Newest()->AfterTick == 7);

    // Timeout: the held candidate may be at most 2 s older than the pass; then the pass is released (sticky, counted once).
    d = step(Synthetic(100., 7, 8, 170.f));
    REQUIRE(d.Hold);
    CHECK(d.HeldTick == 3); // the tick-7 candidate is purged (its After 7 >= Before 7); tick 3 is the newest older one
    d = step(Synthetic(2020., 8, 9, 170.f));
    CHECK(d.Hold);
    CHECK(d.HeldAgeMs == Approx(2000.)); // exactly 2 s: still held
    d = step(Synthetic(2021., 8, 9, 170.f));
    CHECK(d.Release);
    CHECK(d.Timeout);
    CHECK(d.TimeoutStart);
    CHECK_FALSE(d.NoCandidate);
    CHECK_FALSE(d.Hold);
    CHECK(use.TimeMs == 2021.);              // released: use is the (unused) candidate
    d = step(Synthetic(2040., 9, 10, 20.f)); // plausible but contaminated: still released, not counted again
    CHECK(d.Release);
    CHECK(d.Timeout);
    CHECK_FALSE(d.TimeoutStart);
    CHECK(d.Contaminated);
    d = step(Synthetic(2060., 10, 11, 20.f)); // clean
    CHECK_FALSE(d.Release);
    CHECK_FALSE(d.Hold);
    CHECK(d.EpisodeEnd);
    // A new episode: the tick-11 candidate is purged (After 11 >= Before 11), the newest older one is tick 3 from 20 ms,
    // more than 2 s ago, so this episode is released at once and counted as a timeout of its own.
    d = step(Synthetic(2080., 11, 12, 175.f));
    CHECK(d.EpisodeStart);
    CHECK(d.Release);
    CHECK(d.Timeout);
    CHECK(d.TimeoutStart);
    CHECK(d.HeldTick == 3);

    // A new generation starts empty: an implausible first sample has nothing to hold.
    d = step(Synthetic(3000., 1, 1, 170.f, 0.f, true, 2));
    CHECK(d.Reset);
    CHECK(d.Release);
    CHECK(d.NoCandidate);
    CHECK_FALSE(d.Timeout);
    CHECK(g.Count == 0);
    // Boneless / empty samples are passed through untested, not admitted, and do not end an episode.
    BodyPosePresentation::Input boneless;
    boneless.HasData = true;
    boneless.NoBones = true;
    d = g.Step(FlipPoseFrom(boneless, 3010., 2, 2, 3), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    CHECK_FALSE(d.Checked);
    CHECK_FALSE(d.Hold);
    CHECK_FALSE(d.Release);
    CHECK_FALSE(d.EpisodeEnd);
    CHECK(g.Count == 0);
    CHECK(g.Episode);
    CHECK(use.TimeMs == 3010.);
    BodyPosePresentation::Input empty;
    d = g.Step(FlipPoseFrom(empty, 3020., 2, 2, 3), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    CHECK_FALSE(d.Checked);
    d = step(Synthetic(3030., 3, 4, 20.f, 0.f, true, 2)); // clean (Before 3 > bad tick 1)
    CHECK(d.EpisodeEnd);
    CHECK(g.Count == 1);

    // The ring keeps the newest N distinct After ticks; equal ticks replace; an older tick drops the newer ones.
    FlipGuard<4> small;
    for (uint64_t k = 0; k < 7; ++k)
        small.Step(Synthetic(k * 50., k, k + 1), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    CHECK(small.Count == 4);
    CHECK(small.Ring[0].AfterTick == 4);
    CHECK(small.Newest()->AfterTick == 7);
    small.Step(Synthetic(400., 4, 5), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs); // tick order broken: ticks >= 5 dropped
    CHECK(small.Count == 2);
    CHECK(small.Ring[0].AfterTick == 4);
    CHECK(small.Newest()->AfterTick == 5);
    CHECK(small.Newest()->TimeMs == 400.);

    // Applying a held candidate restores its rotations, hip offset and flags into the solver input.
    BodyPosePresentation::Input base;
    base.HasData = true;
    base.HasLegs = true;
    base.HasHips = true;
    base.HipOffset = {0.f, -18.f, 65.f};
    BodyPosePresentation::Input target = base;
    target.Rotations[0] = glm::angleAxis(glm::radians(160.f), glm::vec3{0.f, 0.f, 1.f});
    target.HipOffset = {0.f, -49.f, 40.f};
    target.HasLegs = false;
    const FlipPose held = FlipPoseFrom(base, 0., 1, 4, 5);
    CHECK(held.BeforeTick == 4);
    CHECK(held.AfterTick == 5);
    ApplyFlipPose(held, target);
    CHECK(AngleDeg(target.Rotations[0], base.Rotations[0]) < .01f);
    CHECK(target.HipOffset == base.HipOffset);
    CHECK(target.HasLegs);
    CHECK(target.HasHips);
    CHECK(held.Valid == kFlipAllMask);
    BodyPosePresentation::Input upper = base;
    upper.HasLegs = false;
    CHECK(FlipPoseFrom(upper, 0., 1, 0, 0).Valid == kFlipUpperMask);
    CHECK(FlipPoseFrom(boneless, 0., 1, 0, 0).Valid == 0u);
}

namespace
{
// The SE copy's flat tree (94 entries): skeleton.nif's NPC Root subtree without NPC Root itself, depth-first, as the
// logged boneEntries show (Pelvis 1, LThg 2, LClf 3, LFt 4, RThg 6, RClf 7, RFt 8, Spine1 27, Spine2 28, Neck 29, Head 30,
// RClv 33 .. RHnd 36, LClv 60 .. LHnd 63). Parent indices derived from vanilla skeleton.nif (Skyrim - Meshes0.bsa).
constexpr std::array<int16_t, 94> kVanillaFlatParents{-1, 0,  1,  2,  3,  4,  1,  6,  7,  8,  1,  1,  1,  1,  1,  14, 15, 1,  17, 18, 1,  20, 21, 1,
                                                      23, 24, 0,  26, 27, 28, 29, 30, 30, 28, 33, 34, 35, 36, 37, 38, 36, 40, 41, 36, 43, 44, 36, 46,
                                                      47, 36, 49, 50, 36, 36, 36, 35, 35, 34, 57, 33, 28, 60, 61, 62, 63, 64, 65, 63, 67, 68, 63, 70,
                                                      71, 63, 73, 74, 63, 76, 77, 63, 63, 63, 62, 62, 61, 84, 60, 28, 28, 28, 26, -1, -1, -1};
} // namespace
TEST_CASE("a scaled target carries native scale x ratio and nothing else", "[body_pose_commit]")
{
    const auto base = Identity(10.f, 20.f, 30.f, 1.03f);
    const float ratio = 1.f / 1.03f;
    auto target = Yawed(.5f, base);
    target[9] = 10.f * ratio;
    target[11] = 30.f * ratio;
    target[12] = base[12] * ratio;
    CHECK(ValidTargetScale(target, base, base[12] * ratio, 512.f));
    CHECK_FALSE(ValidTarget(target, base, 512.f)); // the rule (native scale bits) refuses it
    CHECK_FALSE(ValidTargetScale(target, base, base[12], 512.f));
    auto nudged = target;
    nudged[12] = std::nextafter(target[12], 2.f);
    CHECK_FALSE(ValidTargetScale(nudged, base, base[12] * ratio, 512.f)); // bit-identical, not near
    auto negative = target;
    negative[12] = -target[12];
    CHECK_FALSE(ValidTargetScale(negative, base, negative[12], 512.f));
    CHECK_FALSE(ValidTargetScale(target, base, std::numeric_limits<float>::quiet_NaN(), 512.f));
    // The overload is exactly the native-scale case.
    auto plain = Yawed(.5f, base);
    CHECK(ValidTarget(plain, base, 512.f) == ValidTargetScale(plain, base, base[12], 512.f));
    plain[12] = 1.f;
    CHECK(ValidTarget(plain, base, 512.f) == ValidTargetScale(plain, base, base[12], 512.f));
}
TEST_CASE("rigid items below a scaled hand follow its committed scale and position", "[body_pose_commit]")
{
    // Hand entry at native scale 1.03 (the copy's root), committed at scale 1.0 (the sender's): the weapon below it and the
    // axe below the weapon are composed from the committed parent, scale included, as the engine would compose them.
    const glm::vec3 pivot{11000.f, -71000.f, 5550.f};
    std::array<Transform, 1> native{}, target{};
    native[0] = Posed(glm::angleAxis(.4f, glm::vec3{0.f, 1.f, 0.f}), pivot + glm::vec3{20.f, 5.f, 90.f}, 1.03f);
    const float ratio = 1.f / 1.03f;
    target[0] = Posed(glm::angleAxis(1.1f, glm::vec3{0.f, 0.f, 1.f}), pivot + ratio * glm::vec3{20.f, 5.f, 90.f}, native[0][12] * ratio);
    std::array<RigidItem, 2> items{};
    items[0].ParentEntry = 0;
    items[1].ParentRigid = 0;
    const Transform weaponLocal = Posed(glm::angleAxis(.3f, glm::vec3{1.f, 0.f, 0.f}), {2.f, 1.f, -8.f});
    const Transform axeLocal = Posed(glm::angleAxis(1.2f, glm::vec3{0.f, 0.f, 1.f}), {0.f, 0.f, 30.f});
    std::array<RigidRead, 2> reads{};
    reads[0].Readable = reads[0].Matches = true;
    reads[0].Local = weaponLocal;
    reads[0].World = ComposeWorld(native[0], weaponLocal);
    reads[1].Readable = reads[1].Matches = true;
    reads[1].Local = axeLocal;
    reads[1].World = ComposeWorld(reads[0].World, axeLocal);
    std::array<RigidResult, 2> out{};
    const std::array<bool, 1> written{true}, refused{false};
    CHECK(SolveRigid(items, items.size(), reads, written, refused, target, native, 1, out, 512.f) == 2);
    REQUIRE(out[0].Valid);
    REQUIRE(out[1].Valid);
    CHECK(SameBits(out[0].Target, ComposeWorld(target[0], weaponLocal)));
    CHECK(SameBits(out[1].Target, ComposeWorld(out[0].Target, axeLocal)));
    CHECK(out[0].Target[12] == target[0][12]);
    CHECK(out[1].Target[12] == target[0][12]);
    // The axe head is 30 units out along the weapon at native scale 1.03, i.e. 30.9; at the committed scale 1.0 it is 30.
    CHECK(glm::distance(At(out[1].Target), At(out[0].Target)) == Approx(30.f).margin(1e-3));
    CHECK(glm::distance(At(reads[1].World), At(reads[0].World)) == Approx(30.9f).margin(1e-3));
}
TEST_CASE("finger chains are found by structure on the SE layout, thumb first", "[body_pose_commit]")
{
    std::array<int, BodyPosePresentation::kFingerBonesPerHand> right{}, left{};
    REQUIRE(FindFingerChains(kVanillaFlatParents, kVanillaFlatParents.size(), 36, right));
    REQUIRE(FindFingerChains(kVanillaFlatParents, kVanillaFlatParents.size(), 63, left));
    for (size_t k = 0; k < right.size(); ++k)
    {
        CHECK(right[k] == static_cast<int>(37 + k)); // RF00 RF01 RF02 RF10 .. RF42: entries 37..51
        CHECK(left[k] == static_cast<int>(64 + k));  // LF00 .. LF42: entries 64..78
    }
    // Toes: the only flat child of each foot (LToe 5, RToe 9); a hand has several children, the head none.
    CHECK(UniqueChild(kVanillaFlatParents, kVanillaFlatParents.size(), 4) == 5);
    CHECK(UniqueChild(kVanillaFlatParents, kVanillaFlatParents.size(), 8) == 9);
    CHECK(UniqueChild(kVanillaFlatParents, kVanillaFlatParents.size(), 36) == -1);
    CHECK(UniqueChild(kVanillaFlatParents, kVanillaFlatParents.size(), 31) == -1);
    CHECK(UniqueChild(kVanillaFlatParents, kVanillaFlatParents.size(), -1) == -1);
    // Not a hand (the forearm: one three-bone-deep branch, the hand, which is not a simple chain), a bad index.
    std::array<int, BodyPosePresentation::kFingerBonesPerHand> none{};
    CHECK_FALSE(FindFingerChains(kVanillaFlatParents, kVanillaFlatParents.size(), 35, none));
    for (const int v : none)
        CHECK(v == -1);
    CHECK_FALSE(FindFingerChains(kVanillaFlatParents, kVanillaFlatParents.size(), -1, none));
    CHECK_FALSE(FindFingerChains(kVanillaFlatParents, kVanillaFlatParents.size(), 94, none));
    // Shapes that are not the expected five chains of three: none at all, never a guess.
    auto sixth = kVanillaFlatParents; // reroute the magic node (52) and its two neighbours into a sixth chain under the hand
    sixth[53] = 52;
    sixth[54] = 53;
    CHECK_FALSE(FindFingerChains(sixth, sixth.size(), 36, none));
    auto shortChain = kVanillaFlatParents; // the pinky's last bone (51) moved to the hand: a two-bone finger
    shortChain[51] = 36;
    CHECK_FALSE(FindFingerChains(shortChain, shortChain.size(), 36, none));
    auto branched = kVanillaFlatParents; // the magic node hung below the thumb's first bone: a branch inside a finger
    branched[52] = 37;
    CHECK_FALSE(FindFingerChains(branched, branched.size(), 36, none));
    auto longer = kVanillaFlatParents; // WEAPON (53) hung below the index fingertip (42): a four-bone finger
    longer[53] = 42;
    CHECK_FALSE(FindFingerChains(longer, longer.size(), 36, none));
    for (const int v : none)
        CHECK(v == -1);
}
TEST_CASE("finger locals are trusted only when the native world is their compose", "[body_pose_commit]")
{
    const Transform hand = Posed(glm::angleAxis(.7f, glm::normalize(glm::vec3{1.f, 2.f, 0.f})), {100.f, 200.f, 90.f}, 1.03f);
    const Transform local = Posed(glm::angleAxis(.2f, glm::vec3{1.f, 0.f, 0.f}), {2.86f, -.92f, 3.167f});
    bool exact{};
    CHECK(ComposesFrom(ComposeWorld(hand, local), hand, local, exact));
    CHECK(exact);
    Transform noisy = ComposeWorld(hand, local);
    noisy[10] += .001f;
    CHECK(ComposesFrom(noisy, hand, local, exact));
    CHECK_FALSE(exact);
    Transform other = ComposeWorld(hand, local);
    other[9] += .5f; // another local, or a world something else wrote
    CHECK_FALSE(ComposesFrom(other, hand, local, exact));
    Transform turned = ComposeWorld(hand, Posed(glm::angleAxis(.5f, glm::vec3{1.f, 0.f, 0.f}), {2.86f, -.92f, 3.167f}));
    CHECK_FALSE(ComposesFrom(turned, hand, local, exact));
}
TEST_CASE("Havok gate: a biped body in a world or unreadable refuses, a dynamic body outside a world does not", "[body_pose_commit]")
{
    CHECK(kHavokBodyOffset == 0x20);
    CHECK(kHavokObjectOffset == 0x10);
    CHECK(kHavokWorldOffset == 0x10);
    CHECK(kHavokMotionOffset == 0x160);
    REQUIRE(kHavokStateNames.size() == static_cast<size_t>(HavokState::Count));
    HavokRead r{};
    CHECK(ClassifyHavok(r) == HavokState::NoBody); // no collision object
    r.Collision = 0x1000;
    CHECK(ClassifyHavok(r) == HavokState::Unreadable);
    r.BodyRead = true;
    CHECK(ClassifyHavok(r) == HavokState::NoBody); // collision object without a body
    r.Body = 0x2000;
    CHECK(ClassifyHavok(r) == HavokState::Unreadable);
    r.ObjectRead = true;
    CHECK(ClassifyHavok(r) == HavokState::NoBody);
    r.Object = 0x3000;
    CHECK(ClassifyHavok(r) == HavokState::Unreadable);
    r.StateRead = true;
    r.Motion = 4; // keyframed, outside any world: every M1-run (detached)
    CHECK(ClassifyHavok(r) == HavokState::Detached);
    r.Motion = 5;
    CHECK(ClassifyHavok(r) == HavokState::Detached);
    for (const uint8_t motion : {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{6}, uint8_t{7}})
    {
        r.Motion = motion; // after a ragdoll removal (k5c: 3/2 with world 0)
        CHECK(ClassifyHavok(r) == HavokState::DynamicOutside);
        CHECK_FALSE(HavokRefuses(ClassifyHavok(r)));
    }
    r.World = 0x70000000; // added to a Havok world (k5-2 ForceAddRagdollToWorld, k5c PushActorAway)
    for (const uint8_t motion : {uint8_t{2}, uint8_t{3}, uint8_t{4}})
    {
        r.Motion = motion;
        CHECK(ClassifyHavok(r) == HavokState::InWorld);
        CHECK(HavokRefuses(ClassifyHavok(r)));
    }
    CHECK(HavokRefuses(HavokState::Unreadable));
    CHECK_FALSE(HavokRefuses(HavokState::NoBody));
    CHECK_FALSE(HavokRefuses(HavokState::Detached));
}
TEST_CASE("selected re-selection follows the same remote player only, never two copies at once", "[body_pose_commit]")
{
    const uint64_t session = 3;
    const auto selected = RemotePlayerOf(session, (uint64_t{0x1234} << 32) | 0xFF0008E1u);
    CHECK(selected.Known());
    CHECK(selected.Server == 0x1234u);
    const auto respawned = RemotePlayerOf(session, (uint64_t{0x1234} << 32) | 0xFF000946u); // new FormID, same server id
    const auto other = RemotePlayerOf(session, (uint64_t{0x1235} << 32) | 0xFF000946u);
    const auto nextSession = RemotePlayerOf(session + 1, (uint64_t{0x1234} << 32) | 0xFF000946u);
    const auto unbound = RemotePlayerOf(0, (uint64_t{0x1234} << 32) | 0xFF000946u);
    CHECK(SameRemotePlayer(selected, respawned));
    CHECK_FALSE(SameRemotePlayer(selected, other));
    CHECK_FALSE(SameRemotePlayer(selected, nextSession)); // a new pose session (reconnect / reset) is not the same
    CHECK_FALSE(SameRemotePlayer(selected, unbound));
    CHECK_FALSE(SameRemotePlayer(RemotePlayerKey{}, RemotePlayerKey{}));
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF000946u, selected, respawned, false) == SelectedRebind::Rebind);
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF000946u, selected, respawned, true) == SelectedRebind::CurrentLive);
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF000946u, selected, other, false) == SelectedRebind::OtherPlayer);
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF000946u, RemotePlayerKey{}, respawned, false) == SelectedRebind::Unknown);
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF000946u, selected, unbound, false) == SelectedRebind::Unknown);
    CHECK(DecideSelectedRebind(0xFF0008E1u, 0xFF0008E1u, selected, selected, false) == SelectedRebind::Keep);
    CHECK(DecideSelectedRebind(0, 0xFF000946u, selected, respawned, false) == SelectedRebind::Keep); // nothing selected yet: not a rebind
    REQUIRE(kSelectedRebindNames.size() == static_cast<size_t>(SelectedRebind::Count));
    CHECK(std::string(kSelectedRebindNames[static_cast<size_t>(SelectedRebind::Rebind)]) == "rebind");
}

TEST_CASE("A reconnected or new remote player takes the selected only after the selected copy is gone for the hold", "[body_pose_commit]")
{
    using namespace BodyPoseCommit;
    // Play-test 4: the Quest's main-menu copy (FF000D3A, server id 0x10001E) was selected; after it loaded its save it
    // reconnected as FF000D3D with server id 0x20001C, which the same-player rule calls another player.
    const auto first = RemotePlayerOf(1, (uint64_t{0x10001E} << 32) | 0xFF000D3Au);
    const auto reconnected = RemotePlayerOf(1, (uint64_t{0x20001C} << 32) | 0xFF000D3Du);
    CHECK(DecideSelectedRebind(0xFF000D3Au, 0xFF000D3Du, first, reconnected, false) == SelectedRebind::OtherPlayer);
    const double t = 1000.0;
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, true, 0.0, t) == SelectedReselect::CurrentLive); // never two at once
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, true, t - 5000.0, t) == SelectedReselect::CurrentLive);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, 0.0, t) == SelectedReselect::Waiting); // gone, not seen yet
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, t) == SelectedReselect::Waiting);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, t + kSelectedReselectHoldMs - 1.0) == SelectedReselect::Waiting);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, t + kSelectedReselectHoldMs) == SelectedReselect::Reselect);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, t + 60000.0) == SelectedReselect::Reselect);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Au, false, t, t + 60000.0) == SelectedReselect::Keep); // the selected itself
    CHECK(DecideSelectedReselect(0, 0xFF000D3Du, false, t, t + 60000.0) == SelectedReselect::Keep);           // nothing selected: the first-selection latch decides
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0, false, t, t + 60000.0) == SelectedReselect::Keep);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, std::numeric_limits<double>::quiet_NaN()) == SelectedReselect::Waiting); // fail closed
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, std::numeric_limits<double>::quiet_NaN(), t) == SelectedReselect::Waiting);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t + 10.0, t) == SelectedReselect::Waiting); // clock went back
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, t, std::numeric_limits<double>::infinity()) == SelectedReselect::Waiting);
    CHECK(DecideSelectedReselect(0xFF000D3Au, 0xFF000D3Du, false, -1.0, t) == SelectedReselect::Waiting);
}

TEST_CASE("Presentation grace is fresh then held for exactly 200 ms from the original deadline", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    GraceWindow w;
    CHECK(w.Observe(key, 999., true) == GraceAdmission::None); // no prior successful commit
    REQUIRE(w.CommittedFresh(key, 100, 110, 1000., 990., true));
    CHECK(w.Observe(key, 1000., true) == GraceAdmission::Fresh);
    CHECK(w.Observe(key, std::nextafter(1000., 2000.), true) == GraceAdmission::Hold);
    for (double now = 1001.; now < 1200.; now += 1.)
    {
        CHECK(w.Observe(key, now, true) == GraceAdmission::Hold);
        CHECK(w.SourceDeadlineMs == 1000.);
        CHECK(w.HoldDeadlineMs() == 1200.);
        CHECK(w.Before == 100);
        CHECK(w.After == 110);
    }
    CHECK(w.Observe(key, 1200., true) == GraceAdmission::Hold);
    CHECK(w.Observe(key, std::nextafter(1200., 2000.), true) == GraceAdmission::None);
    CHECK_FALSE(w.Armed);
    CHECK(w.Observe(key, 1100., true) == GraceAdmission::None); // even a backwards poll cannot revive expiry
    CHECK_FALSE(w.CommittedFresh(key, 100, 110, 1000., 999., true));
}
TEST_CASE("Repeated fresh publication and successful held passes cannot renew grace", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    GraceWindow w;
    REQUIRE(w.CommittedFresh(key, 100, 110, 1000., 900., true));
    REQUIRE(w.CommittedFresh(key, 100, 110, 1000., 990., true));
    CHECK(w.HoldDeadlineMs() == 1200.);
    CHECK(w.Observe(key, 1050., true) == GraceAdmission::Hold);
    CHECK(w.Observe(key, 1150., true) == GraceAdmission::Hold);
    CHECK(w.HoldDeadlineMs() == 1200.);
    // A held commit is never a CommittedFresh; neither a stale 'success' nor a renewed deadline can arm.
    CHECK_FALSE(w.CommittedFresh(key, 100, 110, 1000., 1150., true));
    CHECK_FALSE(w.CommittedFresh(key, 100, 110, 2000., 1150., true));
    CHECK(w.Observe(key, 1150., true) == GraceAdmission::None);
    // A valid NEWER source resumes normal presentation and gets only its own original deadline.
    REQUIRE(w.CommittedFresh(key, 111, 112, 2100., 1200., true));
    CHECK(w.Observe(key, 2100., true) == GraceAdmission::Fresh);
    CHECK(w.Observe(key, 2300., true) == GraceAdmission::Hold);
    CHECK(w.Observe(key, std::nextafter(2300., 3000.), true) == GraceAdmission::None);
}
TEST_CASE("Presentation grace rejects every changed trusted identity and root tuple", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    std::array<GraceKey, 7> replacements{};
    replacements.fill(key);
    ++replacements[0].Session;
    ++replacements[1].Owner;
    ++replacements[2].Entity;
    ++replacements[3].Generation;
    ++replacements[4].Root;
    ++replacements[5].Tree;
    ++replacements[6].Actor;
    for (const auto& replacement : replacements)
    {
        GraceWindow w;
        REQUIRE(w.CommittedFresh(key, 1, 2, 1000., 900., true));
        CHECK(w.Observe(replacement, 1050., true) == GraceAdmission::None);
        CHECK(w.Observe(key, 1060., true) == GraceAdmission::None); // equal-looking tuple after invalidation cannot revive
        CHECK_FALSE(w.CommittedFresh(key, 1, 2, 1000., 950., true));
    }
    GraceWindow unknown;
    CHECK_FALSE(unknown.CommittedFresh(GraceKey{}, 1, 2, 1000., 900., true));
}
TEST_CASE("Every safety failure and lifecycle invalidation revokes presentation grace", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    GraceWindow havok;
    REQUIRE(havok.CommittedFresh(key, 1, 2, 1000., 900., true));
    CHECK(havok.Observe(key, 1050., !HavokRefuses(HavokState::InWorld)) == GraceAdmission::None);
    CHECK(havok.Observe(key, 1060., !HavokRefuses(HavokState::Detached)) == GraceAdmission::None);
    // The native integration calls Invalidate on every failed commit/lifecycle/safety gate;
    // the engine-free test below checks that revocation is sticky without claiming to execute native gates.
    GraceWindow w;
    CHECK_FALSE(w.CommittedFresh(key, 1, 2, 1000., 900., false)); // even a claimed success cannot arm unsafe inputs
    REQUIRE(w.CommittedFresh(key, 2, 3, 1000., 900., true));
    w.Invalidate(); // Forget/Clear/rebind (production also drops the immutable source shared_ptr)
    CHECK(w.Observe(key, 1050., true) == GraceAdmission::None);
    CHECK_FALSE(w.CommittedFresh(key, 2, 3, 1000., 950., true));
    auto newGeneration = key;
    ++newGeneration.Generation;
    REQUIRE(w.CommittedFresh(newGeneration, 1, 1, 2000., 1100., true));
}
TEST_CASE("Grace clocks are finite monotonic and cannot be reset by sampling", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf, 899.})
    {
        GraceWindow w;
        REQUIRE(w.CommittedFresh(key, 1, 2, 1000., 900., true));
        CHECK(w.Observe(key, bad, true) == GraceAdmission::None);
        CHECK(w.Observe(key, 1050., true) == GraceAdmission::None);
    }
    for (const double bad : {nan, inf, -inf, -1.})
    {
        GraceWindow deadline, now;
        CHECK_FALSE(deadline.CommittedFresh(key, 1, 2, bad, 900., true));
        CHECK_FALSE(now.CommittedFresh(key, 1, 2, 1000., bad, true));
    }
    GraceWindow reversed;
    CHECK_FALSE(reversed.CommittedFresh(key, 3, 2, 1000., 900., true));
    GraceWindow renew;
    REQUIRE(renew.CommittedFresh(key, 1, 2, 1000., 900., true));
    CHECK_FALSE(renew.CommittedFresh(key, 1, 2, 1100., 950., true));
    CHECK(renew.SourceDeadlineMs == 1000.);
    GraceWindow backwardsCommit;
    REQUIRE(backwardsCommit.CommittedFresh(key, 1, 2, 1000., 900., true));
    CHECK_FALSE(backwardsCommit.CommittedFresh(key, 1, 2, 1000., 899., true));
    CHECK_FALSE(backwardsCommit.CommittedFresh(key, 2, 3, 2000., 899., true)); // a newer source cannot reset a backwards clock
}
TEST_CASE("Held pose inputs recompose against the current root and baseline instead of frozen world outputs", "[body_pose_commit][grace]")
{
    using namespace BodyPosePresentation;
    Input committed;
    committed.Count = 2;
    committed.HasData = true;
    committed.Parent[1] = 0;
    committed.Bone[0] = 0;
    committed.Rotations[0] = glm::angleAxis(.3f, glm::vec3{0.f, 0.f, 1.f});
    committed.Baseline[1].Translation = {2.f, 0.f, 0.f};
    const auto frozen = FlipPoseFrom(committed, 900., 31, 1, 2);
    REQUIRE(ValidGracePose(frozen));
    Output first, held;
    REQUIRE(Solve(committed, first));
    Input current = committed;
    current.RootTranslation = {100.f, 20.f, 0.f};
    current.RootRotation = glm::angleAxis(.7f, glm::vec3{0.f, 0.f, 1.f});
    current.Baseline[0].Translation = current.RootTranslation;
    current.Baseline[1].Translation = current.RootTranslation + glm::vec3{4.f, 0.f, 0.f}; // current native length
    current.Rotations[0] = glm::angleAxis(2.f, glm::vec3{0.f, 0.f, 1.f});                 // later re-sample is not the held input
    ApplyFlipPose(frozen, current);
    REQUIRE(Solve(current, held));
    CHECK(AngleDeg(current.Rotations[0], committed.Rotations[0]) < .01f);
    CHECK(AngleDeg(held.world[0].Rotation, current.RootRotation * frozen.Rotation[0]) < .01f);
    CHECK(glm::distance(held.world[0].Translation, current.RootTranslation) < .001f);
    CHECK(glm::distance(held.world[1].Translation, held.world[0].Translation) == Approx(4.f));
    CHECK(glm::distance(first.world[1].Translation, first.world[0].Translation) == Approx(2.f));
    CHECK(frozen.BeforeTick == 1);
    CHECK(frozen.AfterTick == 2);
    CHECK(frozen.TimeMs == 900.);
    auto invalid = frozen;
    invalid.Rotation[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(ValidGracePose(invalid));
    invalid = frozen;
    invalid.Rotation[0] = glm::quat{0.f, 0.f, 0.f, 0.f};
    CHECK_FALSE(ValidGracePose(invalid));
    invalid = frozen;
    invalid.Hip.x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(ValidGracePose(invalid));
    invalid = frozen;
    invalid.Valid = 0;
    CHECK_FALSE(ValidGracePose(invalid));
}

TEST_CASE("The receive adapter endpoint collapse retains only the original committed grace source", "[body_pose_commit][grace]")
{
    struct Point
    {
        uint64_t Tick{};
        VRPose VRPoseData{};
        RemotePose::Provenance PoseSource{};
    };
    std::vector<Point> points(2);
    for (size_t i = 0; i < points.size(); ++i)
    {
        points[i].Tick = 100 + i * 50;
        points[i].VRPoseData.HasData = true;
        for (auto& q : points[i].VRPoseData.Bones)
            q = glm::quat{1.f, 0.f, 0.f, 0.f};
        points[i].PoseSource = {31, points[i].Tick, i * 50., true};
    }
    const auto original = RemotePoseInterpolation::SampleWithProvenance(points, 125, 31, 990., RemotePose::Policy::LiveVRPlayer);
    REQUIRE(original.Pose.HasData);
    REQUIRE(original.Before.SourceTick == 100);
    REQUIRE(original.After.SourceTick == 150);
    GraceWindow window;
    const auto key = GraceIdentity();
    REQUIRE(window.CommittedFresh(key, 100, 150, 1000., 990., true));
    const auto collapsed = RemotePoseInterpolation::SampleWithProvenance(points, 125, 31, 1001., RemotePose::Policy::LiveVRPlayer);
    REQUIRE(collapsed.Pose.HasData); // older endpoint expired, newer endpoint still fresh
    CHECK(collapsed.Before.SourceTick == 150);
    CHECK(collapsed.After.SourceTick == 150);
    CHECK(UnrefreshedGraceSample(original, collapsed));
    CHECK(window.Observe(key, 1001., true) == GraceAdmission::Hold);
    CHECK(window.SourceDeadlineMs == 1000.); // do not replace it with the newer endpoint's 1050 ms deadline
    const auto empty = RemotePoseInterpolation::SampleWithProvenance(points, 125, 31, 1051., RemotePose::Policy::LiveVRPlayer);
    CHECK_FALSE(empty.Pose.HasData);
    CHECK(UnrefreshedGraceSample(original, empty));
    CHECK(window.Observe(key, 1200., true) == GraceAdmission::Hold);
    CHECK(window.Observe(key, std::nextafter(1200., 2000.), true) == GraceAdmission::None);
    auto different = collapsed;
    ++different.After.SourceTick;
    CHECK_FALSE(UnrefreshedGraceSample(original, different)); // genuinely newer input returns to normal publication
    different = collapsed;
    ++different.Generation;
    CHECK_FALSE(UnrefreshedGraceSample(original, different));
    different = collapsed;
    different.Before.ReceivedAtMs += 1.;
    CHECK_FALSE(UnrefreshedGraceSample(original, different));
    different = collapsed;
    different.After.Generation += 1;
    CHECK_FALSE(UnrefreshedGraceSample(original, different));
    different = collapsed;
    different.Pose.NoBones = true;
    CHECK_FALSE(UnrefreshedGraceSample(original, different));
    different = empty;
    different.After.Valid = true;
    CHECK_FALSE(UnrefreshedGraceSample(original, different)); // malformed missing sample is a veto
    CHECK(original.Before.ReceivedAtMs == 0.);
    CHECK(original.After.ReceivedAtMs == 50.);
    CHECK(original.Before.SourceTick == 100);
    CHECK(original.After.SourceTick == 150);
}

TEST_CASE("Cached guard evidence removes one-shot clean events and preserves the original held timeout", "[body_pose_commit][grace]")
{
    FlipGuard<4> guard;
    FlipPose use;
    guard.Step(Synthetic(0., 1, 1), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    const auto held = guard.Step(Synthetic(10., 2, 3, 170.f), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    REQUIRE(held.Checked);
    REQUIRE(held.Hold);
    REQUIRE(held.EpisodeStart);
    const auto heldOrigin = use.TimeMs;
    FlipDecision replay;
    REQUIRE(ReuseFlipDecision(held, 10., 2000., heldOrigin, replay));
    CHECK(replay.Checked);
    CHECK(replay.Hold);
    CHECK_FALSE(replay.Release);
    CHECK(replay.Reason == FlipReason::NeckHead);
    CHECK(replay.NeckHeadDeg == held.NeckHeadDeg);
    CHECK(replay.HeldTick == held.HeldTick);
    CHECK(replay.HeldAgeMs == 2000.);
    CHECK(replay.EpisodeMs == 1990.);
    CHECK_FALSE(replay.Reset);
    CHECK_FALSE(replay.EpisodeStart);
    CHECK_FALSE(replay.EpisodeEnd);
    CHECK_FALSE(replay.TimeoutStart);
    CHECK(replay.Purged == 0);
    CHECK_FALSE(ReuseFlipDecision(held, 10., std::nextafter(2000., 3000.), heldOrigin, replay));
    CHECK_FALSE(ReuseFlipDecision(held, 10., 9., heldOrigin, replay));
    const auto clean = guard.Step(Synthetic(20., 4, 5), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    REQUIRE(clean.EpisodeEnd);
    REQUIRE(clean.EpisodeMs == 10.);
    REQUIRE(ReuseFlipDecision(clean, 20., 30., use.TimeMs, replay));
    CHECK(replay.Checked);
    CHECK_FALSE(replay.Hold);
    CHECK_FALSE(replay.Release);
    CHECK(replay.Reason == FlipReason::None);
    CHECK(replay.NeckHeadDeg >= 0.f);
    CHECK(replay.EpisodeMs == 0.);
    CHECK(replay.HeldAgeMs == 0.);
    CHECK(replay.HeldTick == 0);
    CHECK_FALSE(replay.EpisodeEnd);
    CHECK(replay.Purged == 0);
    const auto reset = guard.Step(Synthetic(40., 1, 1, 0.f, 0.f, true, 2), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    REQUIRE(reset.Reset);
    REQUIRE(ReuseFlipDecision(reset, 40., 50., use.TimeMs, replay));
    CHECK_FALSE(replay.Reset);
    CHECK_FALSE(ReuseFlipDecision(FlipDecision{}, 40., 50., use.TimeMs, replay));
    CHECK_FALSE(ReuseFlipDecision(clean, 20., std::numeric_limits<double>::quiet_NaN(), use.TimeMs, replay));
}
TEST_CASE("Freshness-only deadline crossing selects one existing committed grace rebuild", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    GraceWindow committed, empty;
    REQUIRE(committed.CommittedFresh(key, 1, 2, 1000., 990., true));
    const double after = std::nextafter(1000., 2000.);
    CHECK_FALSE(CanRetryWithGrace(true, false, 1000., 1000.));
    CHECK(CanRetryWithGrace(true, false, after, 1000.)); // admitted fresh, stale at commit entry OR final gate
    CHECK(committed.Observe(key, after, true) == GraceAdmission::Hold);
    CHECK(empty.Observe(key, after, true) == GraceAdmission::None); // never invent an initial successful source
    CHECK_FALSE(CanRetryWithGrace(false, false, after, 1000.));     // safety refusal does not retry
    CHECK_FALSE(CanRetryWithGrace(true, true, after, 1000.));       // the cached rebuild is final
    CHECK(CanRetryWithGrace(true, false, 1200., 1000.));
    CHECK_FALSE(CanRetryWithGrace(true, false, std::nextafter(1200., 2000.), 1000.));
    CHECK_FALSE(CanRetryWithGrace(true, false, std::numeric_limits<double>::infinity(), 1000.));
    committed.Invalidate();
    CHECK(committed.Observe(key, 1100., true) == GraceAdmission::None);
    // The first attempt evaluates guard history on a scratch copy; selecting the old committed input
    // discards that attempt's admissions and one-shot events instead of mutating the real history twice.
    FlipGuard<4> actual;
    FlipPose use;
    actual.Step(Synthetic(990., 1, 2), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    const auto frozen = *actual.Newest();
    auto tentative = actual;
    tentative.Step(Synthetic(after, 1, 2, 20.f), use, kFlipImplausibleDeg, kFlipHoldTimeoutMs);
    CHECK(tentative.Newest()->TimeMs == after);
    CHECK(actual.Newest()->TimeMs == frozen.TimeMs);
    CHECK(AngleDeg(actual.Newest()->Rotation[kFlipNeck], frozen.Rotation[kFlipNeck]) < .01f);
}
TEST_CASE("A fresh final gate can arm a store completing after D without renewing D plus 200", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    const double before = std::nextafter(1000., 0.), after = std::nextafter(1000., 2000.);
    GraceWindow w;
    REQUIRE(w.CommittedFreshCompletion(key, 1, 2, 1000., before, after, true));
    CHECK(w.SourceDeadlineMs == 1000.);
    CHECK(w.HoldDeadlineMs() == 1200.);
    CHECK(w.Observe(key, after, true) == GraceAdmission::Hold);
    CHECK(w.Observe(key, 1200., true) == GraceAdmission::Hold);
    CHECK(w.Observe(key, std::nextafter(1200., 2000.), true) == GraceAdmission::None);
    GraceWindow staleGate, tooLate, reversed, unsafe;
    CHECK_FALSE(staleGate.CommittedFreshCompletion(key, 1, 2, 1000., after, after, true));
    CHECK_FALSE(tooLate.CommittedFreshCompletion(key, 1, 2, 1000., before, std::nextafter(1200., 2000.), true));
    CHECK_FALSE(reversed.CommittedFreshCompletion(key, 1, 2, 1000., before, 999., true));
    CHECK_FALSE(unsafe.CommittedFreshCompletion(key, 1, 2, 1000., before, after, false));
}

TEST_CASE("A fresh topology rebuild revokes old grace before an expiry retry can hide the mismatch", "[body_pose_commit][grace]")
{
    const auto key = GraceIdentity();
    GraceWindow committed;
    REQUIRE(committed.CommittedFresh(key, 1, 2, 1000., 990., true));
    // A fresh first attempt observes changed entries/topology under the same numeric root/tree.
    // Production revokes this exact state before RebuildCommitTopology can make flatMatches true.
    committed.Invalidate();
    const double after = std::nextafter(1000., 2000.);
    REQUIRE(CanRetryWithGrace(true, false, after, 1000.));              // timing alone would otherwise allow a retry
    CHECK(committed.Observe(key, after, true) == GraceAdmission::None); // rebuilt matching topology cannot revive eligibility
    CHECK_FALSE(committed.CommittedFreshCompletion(key, 1, 2, 1000., 999., after, true));
    REQUIRE(committed.CommittedFresh(key, 2, 3, 2000., 1100., true)); // valid NEWER source can own the rebuilt topology
    CHECK(committed.Observe(key, 2050., true) == GraceAdmission::Hold);
    // Record start may predate a newer successful cache after preemption. Assessment time, not the
    // old invocation start, establishes forward age; malformed/backwards reuse remains rejected.
    FlipDecision completed;
    completed.Checked = true;
    completed.NeckHeadDeg = 20.f;
    completed.Spine1PelvisDeg = 10.f;
    FlipDecision reused;
    CHECK_FALSE(ReuseFlipDecision(completed, 1050., 1040., 1050., reused));
    CHECK(ReuseFlipDecision(completed, 1050., 1060., 1050., reused));
    CHECK(reused.Checked);
    CHECK_FALSE(reused.Hold);
}

// V2 exercises the actual production dwell/grace/provenance/solver helpers together. No engine clock,
// source tick or synthetic engine frame substitutes for the explicitly observed presentation counter.
namespace
{
struct DwellPresenter
{
    PresentationDwell Dwell;
    GraceWindow Grace;
    GraceKey Key = GraceIdentity();
    RemotePose::Sample Source;
    FlipPose CommittedInput{};
    uint64_t PresentFrame{}, LastCommitPresented{}, MaxGap{}, Refusals{}, QuarantinedFresh{}, Stores{};
    bool HaveCommit{};
    DwellPresenter()
    {
        Source.Generation = Key.Generation;
        Source.Pose.HasData = true;
        for (auto& q : Source.Pose.Bones)
            q = glm::quat{1.f, 0.f, 0.f, 0.f};
        Source.Pose.Bones[0] = glm::angleAxis(.3f, glm::vec3{0.f, 0.f, 1.f});
        Source.Before = Source.After = {Key.Generation, 2, 0., true};
    }
    bool Step(double aNow, bool aReceiveNew, bool aSafetyEligible = true)
    {
        if (aReceiveNew)
        {
            ++Source.After.SourceTick;
            Source.After.ReceivedAtMs = aNow;
            Source.Before = Source.After;
        }
        const bool rawFresh = RemotePose::IsFresh(Source.Before, Source.Generation, aNow, RemotePose::Policy::LiveVRPlayer) &&
                              RemotePose::IsFresh(Source.After, Source.Generation, aNow, RemotePose::Policy::LiveVRPlayer);
        const bool held = !rawFresh && Grace.Observe(Key, aNow, aSafetyEligible) == GraceAdmission::Hold;
        const bool admitted = aSafetyEligible && Dwell.Allows(aNow, PresentFrame, rawFresh, held);
        size_t written = 0;
        if (admitted)
        {
            BodyPosePresentation::Input input;
            input.Count = 2;
            input.HasData = true;
            input.Bone[0] = 0;
            input.Parent[1] = 0;
            input.RootTranslation = {static_cast<float>(aNow * .01), 0.f, 0.f};
            input.Baseline[0].Translation = input.RootTranslation;
            input.Baseline[1].Translation = input.RootTranslation + glm::vec3{2.f, 0.f, 0.f};
            for (size_t wire = 0; wire < VRPose::kBoneCount; ++wire)
                input.Rotations[wire] = static_cast<glm::quat>(Source.Pose.Bones[wire]);
            if (held)
                ApplyFlipPose(CommittedInput, input);
            BodyPosePresentation::Output output;
            REQUIRE(BodyPosePresentation::Solve(input, output));
            std::array<uintptr_t, 2> nodes{};
            std::array<bool, 2> selected{true, true};
            std::array<Write, 4> writes{};
            REQUIRE(SelectWrites(0x10000, nodes, input.Count, selected, writes, written));
            REQUIRE(written == 2);
            REQUIRE(Dwell.Commit(aNow, PresentFrame, rawFresh, held, Dwell.Transition));
            if (rawFresh)
            {
                CommittedInput = FlipPoseFrom(input, aNow, Source.Generation, Source.Before.SourceTick, Source.After.SourceTick);
                REQUIRE(ValidGracePose(CommittedInput));
                REQUIRE(Grace.CommittedFresh(Key, Source.Before.SourceTick, Source.After.SourceTick, Source.Before.ReceivedAtMs + RemotePose::kLiveTimeoutMs, aNow, true));
            }
            if (HaveCommit)
                MaxGap = std::max(MaxGap, PresentFrame - LastCommitPresented);
            LastCommitPresented = PresentFrame + 1;
            HaveCommit = true;
            ++Stores;
        }
        else
        {
            ++Refusals;
            QuarantinedFresh += rawFresh && Dwell.Waiting;
            Dwell.Withdraw(aNow, PresentFrame);
            Grace.Invalidate();
        }
        CHECK((written != 0) == admitted); // native refusal plan has ZERO selected writes
        Dwell.Presented(aNow, ++PresentFrame);
        return admitted;
    }
};
} // namespace
TEST_CASE("stalls at 150 250 450 700 and 1500 ms resume without a 1 through 29 frame withdrawal gap", "[body_pose_commit][dwell]")
{
    for (const double fps : {30., 60., 240.})
        for (const double pastDeadline : {150., 250., 450., 700., 1500.})
        {
            CAPTURE(fps, pastDeadline);
            DwellPresenter presenter;
            const double resumedAt = 1000. + pastDeadline;
            const double end = resumedAt + std::max(kWithdrawalDwellMs, 1000. * kWithdrawalDwellPresentFrames / fps) + 1000.;
            for (uint64_t frame = 0; frame * 1000. / fps <= end; ++frame)
            {
                const double now = frame * 1000. / fps;
                presenter.Step(now, now >= resumedAt); // continuously fresh receiver samples AFTER resumption
            }
            REQUIRE(presenter.HaveCommit);
            if (pastDeadline == 150.)
            {
                CHECK(presenter.Dwell.Transition == 0);
                CHECK(presenter.Refusals == 0);
                CHECK(presenter.MaxGap == 0);
            }
            else
            {
                CHECK(presenter.Dwell.Transition == 1);
                CHECK(presenter.Refusals >= kWithdrawalDwellPresentFrames);
                CHECK(presenter.MaxGap >= kWithdrawalDwellPresentFrames);
                if (pastDeadline < 1500. || fps == 30.)
                    CHECK(presenter.QuarantinedFresh > 0); // late resumption can arrive after dwell is already ready
            }
            CHECK_FALSE((presenter.MaxGap >= 1 && presenter.MaxGap <= 29));
            CHECK_FALSE(presenter.Dwell.Waiting);
        }
}
TEST_CASE("dwell needs both exact 750 ms and 45 subsequent actual presentation frames plus fresh eligibility", "[body_pose_commit][dwell]")
{
    PresentationDwell d;
    REQUIRE(d.Commit(0., 10, true, false, d.Transition));
    d.Presented(1., 11);
    REQUIRE(d.Withdraw(10., 11));
    const double justBefore = std::nextafter(760., 0.);
    CHECK_FALSE(d.Allows(justBefore, 56, true, false));
    CHECK_FALSE(d.Allows(760., 55, true, false)); // backwards counter is also refused, not counted as progress
    CHECK(d.Allows(760., 56, true, false));
    CHECK_FALSE(d.Allows(760., 56, false, true)); // even elapsed dwell cannot re-enter using old committed grace
    CHECK_FALSE(d.Allows(760., 56, false, false));
    REQUIRE(d.Commit(760., 56, true, false, d.Transition));
    CHECK_FALSE(d.Waiting);
    CHECK(d.Live);
    CHECK_FALSE(d.Withdraw(761., 56)); // this re-entry was never presented: no new actual withdrawal interval
    CHECK(d.Transition == 1);
    PresentationDwell slow;
    REQUIRE(slow.Commit(0., 0, true, false, 0));
    slow.Presented(0., 1);
    REQUIRE(slow.Withdraw(1., 1));
    CHECK_FALSE(slow.Allows(5000., 1, true, false)); // no presentation callbacks, regardless of engine/source activity
    for (uint64_t frame = 2; frame <= 45; ++frame)
        slow.Presented(5000. + frame, frame);
    CHECK_FALSE(slow.Allows(5050., 45, true, false)); // 44 subsequent presents
    slow.Presented(5051., 46);
    CHECK(slow.Allows(5051., 46, true, false));
    PresentationDwell fast;
    REQUIRE(fast.Commit(0., 0, true, false, 0));
    fast.Presented(0., 1);
    REQUIRE(fast.Withdraw(1., 1));
    for (uint64_t frame = 2; frame <= 101; ++frame)
        fast.Presented(static_cast<double>(frame), frame);
    CHECK_FALSE(fast.Allows(750., 101, true, false)); // 749 ms even though 100 frames have passed
    CHECK(fast.Allows(751., 101, true, false));
}
TEST_CASE("lifecycle source changes and repeated native refusals cannot erase or restart dwell", "[body_pose_commit][dwell]")
{
    PresentationDwell d;
    REQUIRE(d.Commit(0., 0, true, false, 0));
    d.Presented(1., 1);
    REQUIRE(d.Withdraw(10., 1));
    const auto transition = d.Transition;
    GraceWindow source;
    auto identity = GraceIdentity();
    for (uint64_t frame = 2; frame <= 30; ++frame)
    {
        const double now = 10. + frame * 10.;
        source.Invalidate(); // Forget/Clear/topology and every native safety veto discard source eligibility
        ++identity.Generation;
        ++identity.Owner;
        ++identity.Root;
        ++identity.Tree;
        CHECK_FALSE(d.Withdraw(now, frame)); // repeated refusal/reset cannot move the original anchor
        CHECK(d.Transition == transition);
        CHECK(d.StartedMs == 10.);
        CHECK(d.StartedPresentFrame == 1);
        CHECK_FALSE(d.Allows(now, frame, frame % 2 != 0, false));
    }
    CHECK(d.Allows(760., 46, true, false));
    CHECK_FALSE(d.Commit(760., 46, true, false, transition - 1)); // stale gate token cannot cancel a newer withdrawal
    REQUIRE(d.Commit(760., 46, true, false, transition));
    d.Presented(761., 47);
    REQUIRE(d.Withdraw(762., 47));
    CHECK(d.Transition == transition + 1);
    CHECK(d.StartedMs == 762.);
    CHECK(d.StartedPresentFrame == 47);
}
TEST_CASE("cold presentation and backwards nonfinite clock or presentation counter never grant early admission", "[body_pose_commit][dwell]")
{
    PresentationDwell cold;
    CHECK_FALSE(cold.Withdraw(0., 0));
    CHECK_FALSE(cold.Waiting);
    CHECK_FALSE(cold.Allows(0., 0, false, true)); // no first live source invented from a hold
    REQUIRE(cold.Commit(0., 0, true, false, 0));
    CHECK_FALSE(cold.Withdraw(1., 0));
    CHECK_FALSE(cold.Waiting); // committed but never actually presented
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.})
    {
        PresentationDwell d;
        REQUIRE(d.Commit(0., 0, true, false, 0));
        d.Presented(1., 1);
        REQUIRE(d.Withdraw(invalid, 2));
        CHECK(d.Waiting);
        CHECK_FALSE(d.StartValid);
        CHECK_FALSE(d.Allows(2., 2, true, false)); // first valid observation anchors LATER, never at an invalid clock
        CHECK(d.StartedMs == 2.);
        CHECK(d.StartedPresentFrame == 2);
        CHECK_FALSE(d.Allows(751., 47, true, false));
        CHECK(d.Allows(752., 47, true, false));
    }
    PresentationDwell backwards;
    REQUIRE(backwards.Commit(0., 100, true, false, 0));
    backwards.Presented(1., 101);
    REQUIRE(backwards.Withdraw(10., 101));
    CHECK_FALSE(backwards.Allows(9., 200, true, false));
    CHECK_FALSE(backwards.Allows(760., 100, true, false));
    CHECK_FALSE(backwards.Allows(760., 146, true, false)); // frame 200 was observed even alongside the bad clock
    CHECK(backwards.Allows(760., 200, true, false));
    PresentationDwell mixed;
    REQUIRE(mixed.Commit(0., 0, true, false, 0));
    mixed.Presented(1., 1);
    REQUIRE(mixed.Withdraw(std::numeric_limits<double>::quiet_NaN(), 5));
    CHECK_FALSE(mixed.Allows(2., 2, true, false)); // bad clock did not erase the independently observed frame high-water mark
    CHECK_FALSE(mixed.Allows(2., 5, true, false));
    CHECK(mixed.StartedPresentFrame == 5);
    CHECK_FALSE(mixed.Allows(752., 49, true, false));
    CHECK(mixed.Allows(752., 50, true, false));
    backwards.Presented(761., UINT64_MAX);
    CHECK_FALSE(backwards.Allows(2000., 0, true, false)); // wrap/backwards observation never subtracts into an enormous elapsed count
}

TEST_CASE("Child hashes change when a child array changes", "[body_pose_commit]")
{
    std::array<uintptr_t, 3> children{1, 2, 3};
    const auto original = ChildHash(children.data(), children.size());
    children[1] = 4;
    CHECK(ChildHash(children.data(), children.size()) != original);
    CHECK(ChildHash(children.data(), 2) != original);
}
