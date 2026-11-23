#include <catch2/catch.hpp>

#include <DialogueFacing.h>

#include <cmath>

TEST_CASE("a talking player faces the speaker the way the game counts headings", "[dialogue_facing]")
{
    const glm::vec3 cFrom{100.f, 100.f, 0.f};

    REQUIRE(DialogueFacing::HeadingTo(cFrom, {100.f, 300.f, 0.f}).value() == Approx(0.f).margin(1e-5));
    REQUIRE(DialogueFacing::HeadingTo(cFrom, {300.f, 100.f, 0.f}).value() == Approx(glm::half_pi<float>()));
    REQUIRE(DialogueFacing::HeadingTo(cFrom, {100.f, -100.f, 0.f}).value() == Approx(glm::pi<float>()));
    // West is three quarters of a turn, never a negative heading.
    REQUIRE(DialogueFacing::HeadingTo(cFrom, {-100.f, 100.f, 50.f}).value() == Approx(3.f * glm::half_pi<float>()));
}

TEST_CASE("a speaker too near or too far leaves the body's own heading", "[dialogue_facing]")
{
    const glm::vec3 cFrom{0.f, 0.f, 0.f};

    REQUIRE_FALSE(DialogueFacing::HeadingTo(cFrom, {0.f, 0.f, 500.f}).has_value());
    REQUIRE_FALSE(DialogueFacing::HeadingTo(cFrom, {DialogueFacing::kMaxDistance + 1.f, 0.f, 0.f}).has_value());
    REQUIRE(DialogueFacing::HeadingTo(cFrom, {DialogueFacing::kMaxDistance, 0.f, 0.f}).has_value());
}

TEST_CASE("a broken position leaves the body's own heading", "[dialogue_facing]")
{
    REQUIRE_FALSE(DialogueFacing::HeadingTo({0.f, 0.f, 0.f}, {std::nanf(""), 0.f, 0.f}).has_value());
}
