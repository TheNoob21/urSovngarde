#include <catch2/catch.hpp>
#include <Systems/BodyPoseValidation.h>

#include <array>
#include <limits>

namespace
{
using Matrix = std::array<std::array<float, 3>, 3>;
Matrix Identity()
{
    return {{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}};
}
} // namespace

TEST_CASE("Body pose rotations reject malformed source matrices", "[body_pose_validation]")
{
    REQUIRE(BodyPoseValidation::IsRotation(Identity()));
    Matrix mild = Identity();
    mild[0][0] += 0.001f;
    mild[1][0] += 0.001f;
    REQUIRE(BodyPoseValidation::IsRotation(mild));
    REQUIRE_FALSE(BodyPoseValidation::IsRotation(Matrix{})); // zero lower-body matrix
    Matrix head = Identity();
    head[1][2] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(BodyPoseValidation::IsRotation(head));
    Matrix reflection = Identity();
    reflection[0][0] = -1.f;
    REQUIRE_FALSE(BodyPoseValidation::IsRotation(reflection));
    Matrix shear = Identity();
    shear[1][0] = 0.2f;
    REQUIRE_FALSE(BodyPoseValidation::IsRotation(shear));
}
