#include <catch2/catch.hpp>

#include "../client/VRSupportedVersion.h"

TEST_CASE("VR address data accepts only its executable version", "[vr.version]")
{
    CHECK(VRSupportedVersion::IsSupported("1.4.15.0"));
    for (const auto* version : {"1.4.15.1", "1.4.14.0", "1.4.16.0", "1.3.15.0",
                                "1.5.97.0", "1.6.1170.0", "1.7.104.0", "1.0.0.0",
                                "", "1.4.15", "1.4.15.0 extra", "1.4.15.00", " 1.4.15.0"})
    {
        INFO(version);
        CHECK_FALSE(VRSupportedVersion::IsSupported(version));
    }
}
