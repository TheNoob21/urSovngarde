#include <catch2/catch.hpp>
#include <Games/Skyrim/WeaponContactRules.h>
#include <cstddef>

namespace
{
struct Vec
{
    float X{}, Y{}, Z{};
};
struct Body
{
    Vec Velocity;
};
struct Event
{
    void* Bodies[2];
    const float* Point;
};
struct Sample
{
    const void* pBodies[2]{};
    Vec Point{};
    Vec Velocities[2]{};
};
} // namespace

TEST_CASE("Weapon contact callback retains stock ordered samples", "[weapon_contact]")
{
    Body a{{1, 0, 0}}, b{{0, 0, 0}};
    float firstPoint[]{1, 2, 3}, secondPoint[]{4, 5, 6};
    std::array<Sample, 3> samples{};
    size_t count = 0;
    auto callback = [&](const Event* event)
    {
        WeaponContactRules::OnContactPoint<Sample>(
            reinterpret_cast<const unsigned char*>(event), offsetof(Event, Bodies), offsetof(Event, Point), [](Sample& sample, size_t i)
            { sample.Velocities[i] = static_cast<const Body*>(sample.pBodies[i])->Velocity; }, [&](const Sample& sample) { WeaponContactRules::Record(samples, count, sample); });
    };
    callback(nullptr);
    Event missing{{&a, nullptr}, firstPoint};
    callback(&missing);
    CHECK(count == 0);
    Event event{{&a, &b}, firstPoint};
    callback(&event);
    a.Velocity.X = 8;
    event.Point = secondPoint;
    callback(&event);
    REQUIRE(count == 1);
    CHECK(samples[0].Point.X == 4);
    CHECK(samples[0].Velocities[0].X == 1);
    Event reverse{{&b, &a}, nullptr};
    b.Velocity.X = 0;
    a.Velocity.X = 0;
    callback(&reverse);
    REQUIRE(count == 2);
    CHECK(samples[1].pBodies[0] == &b);
    CHECK(samples[1].Point.X == 0);
    CHECK(samples[1].Velocities[0].X == 0);
    event.Point = nullptr;
    callback(&event);
    CHECK(samples[0].Point.X == 0);
    CHECK(samples[0].Velocities[0].X == 1);
}
