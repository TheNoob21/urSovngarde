#include <catch2/catch.hpp>
#include <Systems/BodyPoseBounds.h>
#include <cmath>
#include <limits>

using namespace BodyPoseBounds;

TEST_CASE("The world bound sits at the SE NiAVObject offset, just before the flags", "[body_pose_bounds]")
{
    CHECK(kNodeWorldBound == 0xE4);
    CHECK(kNodeWorldBound + kBoundBytes == 0xF4);
    CHECK(sizeof(Sphere) == 16);
}

TEST_CASE("The sphere of the written points holds every point and the padding", "[body_pose_bounds]")
{
    PointBox box{};
    box.Add(74555.f, -129432.f, 22740.f);    // feet
    box.Add(74580.2f, -129427.8f, 22882.8f); // raised hand
    box.Add(74531.f, -129440.f, 22812.f);    // other hand
    Sphere s{};
    REQUIRE(box.Enclosing(kWrittenPaddingUnits, s));
    for (const Sphere point :
         {Sphere{74555.f, -129432.f, 22740.f, kWrittenPaddingUnits}, Sphere{74580.2f, -129427.8f, 22882.8f, kWrittenPaddingUnits},
          Sphere{74531.f, -129440.f, 22812.f, kWrittenPaddingUnits}})
        CHECK(Contains(s, point));
    CHECK(s[3] < 120.f); // half the box diagonal (about 73) plus the padding: not a runaway bound
}

TEST_CASE("No point, a non-finite point or a bad padding gives no sphere", "[body_pose_bounds]")
{
    Sphere s{};
    CHECK_FALSE(PointBox{}.Enclosing(kWrittenPaddingUnits, s));
    PointBox bad{};
    bad.Add(0.f, 0.f, 0.f);
    bad.Add(std::numeric_limits<float>::quiet_NaN(), 1.f, 1.f);
    CHECK_FALSE(bad.Enclosing(kWrittenPaddingUnits, s));
    PointBox one{};
    one.Add(1.f, 2.f, 3.f);
    CHECK_FALSE(one.Enclosing(-1.f, s));
    REQUIRE(one.Enclosing(0.f, s));
    CHECK(s[3] > 0.f); // a single point still has a usable radius
}

TEST_CASE("A union holds both spheres and never shrinks the native bound", "[body_pose_bounds]")
{
    // The diagnosed case: hands bound at hip height, written hand at the raised position.
    const Sphere native{74556.f, -129425.f, 22805.f, 28.f};
    const Sphere posed{74560.f, -129430.f, 22830.f, 95.f};
    Sphere u{};
    REQUIRE(Union(native, posed, u));
    CHECK(Contains(u, native));
    CHECK(Contains(u, posed));
    const Sphere left{0.f, 0.f, 0.f, 1.f}, right{10.f, 0.f, 0.f, 1.f};
    REQUIRE(Union(left, right, u));
    CHECK(Contains(u, left));
    CHECK(Contains(u, right));
    CHECK(u[3] == Approx(6.f).margin(0.01));
    CHECK(u[0] == Approx(5.f));
}

TEST_CASE("A sphere already holding the other is kept bit for bit (no store)", "[body_pose_bounds]")
{
    const Sphere cell{70000.f, -130000.f, 22000.f, 6000.f}, posed{74560.f, -129430.f, 22830.f, 95.f};
    Sphere u{};
    REQUIRE(Union(cell, posed, u));
    CHECK(SameBits(u, cell));
    REQUIRE(Union(posed, cell, u));
    CHECK(SameBits(u, cell));
    REQUIRE(Union(posed, posed, u));
    CHECK(SameBits(u, posed));
}

TEST_CASE("Unusable bounds are refused: non-finite or radius not positive", "[body_pose_bounds]")
{
    const Sphere good{0.f, 0.f, 0.f, 1.f};
    Sphere u{};
    CHECK_FALSE(Union(Sphere{0.f, 0.f, 0.f, 0.f}, good, u));
    CHECK_FALSE(Union(good, Sphere{0.f, 0.f, 0.f, -1.f}, u));
    CHECK_FALSE(Union(Sphere{std::numeric_limits<float>::infinity(), 0.f, 0.f, 1.f}, good, u));
    CHECK_FALSE(Union(good, Sphere{0.f, std::numeric_limits<float>::quiet_NaN(), 0.f, 1.f}, u));
    CHECK_FALSE(Usable(Sphere{0.f, 0.f, 0.f, std::numeric_limits<float>::infinity()}));
}

TEST_CASE("Unions at world coordinates still hold both inputs after float rounding", "[body_pose_bounds]")
{
    for (int i = 0; i < 200; ++i)
    {
        const float k = static_cast<float>(i);
        const Sphere a{100000.f + k * 3.7f, -150000.f + k * 1.3f, 20000.f - k, 5.f + k * 0.11f};
        const Sphere b{a[0] + std::sin(k) * 60.f, a[1] + std::cos(k) * 60.f, a[2] + k * 0.3f, 20.f + k * 0.07f};
        Sphere u{};
        REQUIRE(Union(a, b, u));
        CHECK(Contains(u, a));
        CHECK(Contains(u, b));
        CHECK(u[3] >= std::max(a[3], b[3]));
    }
}
