#include <catch2/catch.hpp>
#include <Systems/BodyPoseWorldInput.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <limits>

using namespace BodyPoseWorldInput;
namespace
{
Transform Native(const glm::quat& acQ, float aScale)
{
    const auto m = glm::mat3_cast(acQ);
    Transform t{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[r * 3 + c] = m[c][r];
    t[12] = aScale;
    return t;
}
glm::quat Axis(float aAngle, glm::vec3 aAxis)
{
    return glm::angleAxis(aAngle, glm::normalize(aAxis));
}
void SameBits(const Transform& acA, const Transform& acB)
{
    CHECK(std::memcmp(acA.data(), acB.data(), sizeof(float) * acA.size()) == 0);
}
} // namespace

TEST_CASE("World pose input composes parent and local into root-relative target", "[body_pose_world_input]")
{
    const auto parent = Native(Axis(0.83f, {1, 2, -3}) * Axis(-0.4f, {0, 1, 0}), 1.03f);
    const auto root = Native(Axis(-1.1f, {2, -1, 1}), 2.5f);
    auto original = Native(Axis(0.2f, {1, 1, 0}), 1.03f);
    original[9] = -0.0f;
    original[10] = -27.25f;
    original[11] = 10000.f;
    const auto source = Axis(0.7f, {-1, 3, 2}) * Axis(-0.3f, {0, 0, 1});
    Transform out{};
    REQUIRE(Solve(parent, root, original, source, out));
    CHECK(std::memcmp(out.data() + 9, original.data() + 9, sizeof(float) * 4) == 0);
    // Independently multiply the raw row-major matrices and compare each basis
    // vector with the desired root-world rotation applied to the source rotation.
    const auto desired = glm::mat3_cast(source);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            float actual = 0.f, expected = 0.f;
            for (int k = 0; k < 3; ++k)
            {
                actual += parent[r * 3 + k] * out[k * 3 + c];
                expected += root[r * 3 + k] * desired[c][k];
            }
            CHECK(actual == Approx(expected).margin(0.0001));
        }
}

TEST_CASE("World pose input rejects malformed inputs without changing output", "[body_pose_world_input]")
{
    const auto good = Native(Axis(0.5f, {1, 2, 3}), 1.f);
    const auto source = Axis(0.4f, {2, 1, 3});
    Transform sentinel = good;
    sentinel[9] = -0.0f;
    sentinel[10] = 42.f;
    const auto reject = [&](const Transform& parent, const Transform& root, const Transform& local, const glm::quat& q)
    {
        auto out = sentinel;
        CHECK_FALSE(Solve(parent, root, local, q, out));
        SameBits(out, sentinel);
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (int which = 0; which < 3; ++which)
    {
        const auto check = [&](const Transform& bad)
        {
            reject(which == 0 ? bad : good, which == 1 ? bad : good, which == 2 ? bad : good, source);
        };
        auto bad = good;
        bad[0] = nan;
        check(bad);
        bad = good;
        bad[10] = inf;
        check(bad);
        bad = good;
        bad[12] = 0.f;
        check(bad);
        bad = good;
        bad[12] = -1.f;
        check(bad);
        bad = good;
        for (int i = 0; i < 9; ++i)
            bad[i] = 0.f;
        check(bad);
        bad = good;
        bad[0] *= -1;
        bad[3] *= -1;
        bad[6] *= -1;
        check(bad);
        bad = good;
        bad[1] += 0.5f;
        check(bad);
        bad = good;
        bad[0] = 4.f;
        check(bad);
    }
    auto q = source;
    q.x = nan;
    reject(good, good, good, q);
    q = source;
    q.w = inf;
    reject(good, good, good, q);
    q = {0, 0, 0, 0};
    reject(good, good, good, q);
    q = source * 2.f;
    reject(good, good, good, q);
}

TEST_CASE("Physical world pose reconstructs a private local transform", "[body_pose_world_input]")
{
    auto parent = Native(Axis(0.83f, {1, 2, -3}), 2.5f);
    auto local = Native(Axis(-1.1f, {2, -1, 1}), 1.125f);
    parent[9] = 100000.f;
    parent[10] = -70000.f;
    parent[11] = 50000.f;
    local[9] = 15.f;
    local[10] = -32.f;
    local[11] = 9.f;
    // Construct the physical transform independently, using double intermediates.
    Transform body{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            double value = 0;
            for (int k = 0; k < 3; ++k)
                value += double(parent[r * 3 + k]) * local[k * 3 + c];
            body[r * 3 + c] = float(value);
        }
        double value = parent[9 + r];
        for (int k = 0; k < 3; ++k)
            value += double(parent[12]) * parent[r * 3 + k] * local[9 + k];
        body[9 + r] = float(value);
    }
    body[12] = parent[12] * local[12];
    auto original = Native(Axis(0.3f, {0, 1, 0}), local[12]);
    Transform out{};
    REQUIRE(ReconstructLocal(parent, body, original, out));
    CHECK(std::memcmp(&out[12], &original[12], sizeof(float)) == 0);
    for (int i = 0; i < 9; ++i)
        CHECK(out[i] == Approx(local[i]).margin(0.00001));
    for (int i = 9; i < 12; ++i)
        CHECK(out[i] == Approx(local[i]).margin(0.005));
    // Check the reconstructed local against the physical world without relying on ComposeReference.
    for (int r = 0; r < 3; ++r)
    {
        double position = parent[9 + r];
        for (int k = 0; k < 3; ++k)
            position += double(parent[12]) * parent[r * 3 + k] * out[9 + k];
        CHECK(position == Approx(body[9 + r]).epsilon(0).margin(0.002));
        for (int c = 0; c < 3; ++c)
        {
            double rotation = 0;
            for (int k = 0; k < 3; ++k)
                rotation += double(parent[r * 3 + k]) * out[k * 3 + c];
            CHECK(rotation == Approx(body[r * 3 + c]).margin(0.00001));
        }
    }
}

TEST_CASE("Physical world reconstruction inverts accepted native matrix drift", "[body_pose_world_input]")
{
    auto parent = Native(glm::quat{1, 0, 0, 0}, 1.03f);
    parent[0] = 1.01f;
    parent[1] = 0.005f;
    parent[9] = 11013.f;
    parent[10] = -71474.f;
    parent[11] = 5552.f;
    auto body = Native(Axis(0.6f, {1, -2, 3}), 1.f);
    body[9] = 11030.f;
    body[10] = -71460.f;
    body[11] = 5600.f;
    const auto original = Native(glm::quat{1, 0, 0, 0}, 1.125f);
    Transform local{};
    REQUIRE(ReconstructLocal(parent, body, original, local));
    for (int r = 0; r < 3; ++r)
    {
        double position = parent[9 + r];
        for (int k = 0; k < 3; ++k)
            position += double(parent[12]) * parent[r * 3 + k] * local[9 + k];
        CHECK(position == Approx(body[9 + r]).epsilon(0).margin(0.00001));
        for (int c = 0; c < 3; ++c)
        {
            double rotation = 0;
            for (int k = 0; k < 3; ++k)
                rotation += double(parent[r * 3 + k]) * local[k * 3 + c];
            CHECK(rotation == Approx(body[r * 3 + c]).epsilon(0).margin(0.000001));
        }
    }
}

TEST_CASE("Physical world reconstruction rejects invalid and overflowing inputs transactionally", "[body_pose_world_input]")
{
    const auto good = Native(Axis(0.5f, {1, 2, 3}), 1.f);
    auto sentinel = good;
    sentinel[9] = -0.f;
    sentinel[10] = 42.f;
    const auto reject = [&](const Transform& parent, const Transform& body, const Transform& local)
    {
        auto out = sentinel;
        CHECK_FALSE(ReconstructLocal(parent, body, local, out));
        SameBits(out, sentinel);
    };
    for (int which = 0; which < 3; ++which)
    {
        const auto check = [&](const Transform& bad)
        {
            reject(which == 0 ? bad : good, which == 1 ? bad : good, which == 2 ? bad : good);
        };
        auto bad = good;
        bad[0] = std::numeric_limits<float>::quiet_NaN();
        check(bad);
        bad = good;
        bad[10] = std::numeric_limits<float>::infinity();
        check(bad);
        bad = good;
        bad[12] = 0.f;
        check(bad);
        bad = good;
        bad[12] = -1.f;
        check(bad);
        bad = good;
        bad[0] *= -1;
        bad[3] *= -1;
        bad[6] *= -1;
        check(bad);
        bad = good;
        bad[1] += 0.5f;
        check(bad);
    }
    auto parent = good, body = good;
    parent[12] = std::numeric_limits<float>::min();
    body[9] = std::numeric_limits<float>::max();
    reject(parent, body, good);
}

namespace
{
float AngularDistance(const glm::quat& acA, const glm::quat& acB)
{
    const auto relative = glm::conjugate(glm::normalize(acA)) * glm::normalize(acB);
    return 2.f * std::atan2(glm::length(glm::vec3(relative.x, relative.y, relative.z)), std::fabs(relative.w));
}
constexpr float kDegree = 3.14159265358979323846f / 180.f;
} // namespace

TEST_CASE("Delta step respects shortest arc, frame rate, and five degree envelope", "[body_pose_world_input]")
{
    const glm::quat identity{1, 0, 0, 0};
    const auto previous = Axis(3.f * kDegree, {1, 2, -1});
    const auto desired = Axis(40.f * kDegree, {-2, 1, 3});
    glm::quat out{}, negated{};
    REQUIRE(StepDelta(previous, desired, 1.f / 60.f, out));
    CHECK(AngularDistance(previous, out) == Approx(0.25f * kDegree).margin(0.000002));
    CHECK(AngularDistance(identity, out) <= 5.f * kDegree + 0.00001f);
    REQUIRE(StepDelta(-previous, -desired, 1.f / 60.f, negated));
    CHECK(AngularDistance(out, negated) < 0.000002f);
    REQUIRE(StepDelta(previous, desired, 0.001f, out));
    CHECK(AngularDistance(previous, out) == Approx(0.015f * kDegree).margin(0.000002));
    REQUIRE(StepDelta(previous, desired, 0.1f, out));
    CHECK(AngularDistance(previous, out) == Approx(0.25f * kDegree).margin(0.000002));
    const auto nearEdge = Axis(4.9f * kDegree, {1, 0, 0});
    REQUIRE(StepDelta(nearEdge, Axis(40.f * kDegree, {1, 0, 0}), 0.1f, out));
    CHECK(AngularDistance(identity, out) == Approx(5.f * kDegree).margin(0.000002));
    REQUIRE(StepDelta(previous, -previous, 0.01f, out));
    CHECK(AngularDistance(previous, out) < 0.000002f);
}

TEST_CASE("Delta step rejects invalid inputs transactionally", "[body_pose_world_input]")
{
    const glm::quat identity{1, 0, 0, 0}, sentinel = Axis(2.f * kDegree, {1, 2, 3});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto reject = [&](const glm::quat& previous, const glm::quat& desired, float dt)
    {
        auto out = sentinel;
        CHECK_FALSE(StepDelta(previous, desired, dt, out));
        CHECK(std::memcmp(&out, &sentinel, sizeof(out)) == 0);
    };
    for (float dt : {0.f, -0.01f, 0.1001f, nan, inf})
        reject(identity, identity, dt);
    reject(Axis(5.1f * kDegree, {1, 0, 0}), identity, 0.01f);
    reject(glm::quat{0, 0, 0, 0}, identity, 0.01f);
    reject(identity, glm::quat{2, 0, 0, 0}, 0.01f);
    reject(glm::quat{nan, 0, 0, 0}, identity, 0.01f);
    reject(identity, glm::quat{inf, 0, 0, 0}, 0.01f);
}

TEST_CASE("Native reference composition rotates and scales local translation", "[body_pose_world_input]")
{
    auto parent = Native(Axis(90.f * kDegree, {0, 0, 1}), 2.f);
    auto local = Native(Axis(90.f * kDegree, {1, 0, 0}), 3.f);
    parent[9] = 10;
    parent[10] = 20;
    parent[11] = 30;
    local[9] = 1;
    local[10] = 2;
    local[11] = 3;
    Transform out{};
    REQUIRE(ComposeReference(parent, local, out));
    const Transform expected{0, 0, 1, 1, 0, 0, 0, 1, 0, 6, 22, 36, 6};
    for (size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Approx(expected[i]).margin(0.00001));
    const auto reject = [&](const Transform& p, const Transform& l)
    {
        auto result = expected;
        CHECK_FALSE(ComposeReference(p, l, result));
        SameBits(result, expected);
    };
    auto bad = parent;
    bad[0] = std::numeric_limits<float>::quiet_NaN();
    reject(bad, local);
    bad = local;
    bad[12] = 0;
    reject(parent, bad);
    bad = local;
    bad[1] += 0.5f;
    reject(parent, bad);
    bad = parent;
    bad[12] = std::numeric_limits<float>::max();
    reject(bad, local);
    bad = local;
    bad[9] = std::numeric_limits<float>::max();
    reject(parent, bad);
}

TEST_CASE("Delta step treats signed-zero half-turn quaternions identically", "[body_pose_world_input]")
{
    const glm::quat identity{1, 0, 0, 0};
    for (const glm::quat desired : {glm::quat{0.f, 1.f, 0.f, 0.f}, glm::quat{0.f, 0.f, -1.f, 0.f}, glm::quat{0.f, 0.f, 0.f, -1.f}})
    {
        glm::quat positive{}, negative{};
        REQUIRE(StepDelta(identity, desired, 1.f / 60.f, positive));
        REQUIRE(StepDelta(identity, -desired, 1.f / 60.f, negative));
        CHECK(AngularDistance(positive, negative) < 0.000002f);
        CHECK(AngularDistance(identity, positive) == Approx(0.25f * kDegree).margin(0.000002));
    }
}
