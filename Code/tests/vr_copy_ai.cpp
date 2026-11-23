#include <catch2/catch.hpp>

#include <VRAddressOverrides.h>

#include <cstdint>

namespace
{
uint32_t VROffsetOf(const uint32_t aId) noexcept
{
    for (const auto& cEntry : kVRAddressOverrides)
        if (cEntry.id == aId)
            return cEntry.offset;
    return 0;
}

uint32_t TimesListed(const uint32_t aId) noexcept
{
    uint32_t count = 0;
    for (const auto& cEntry : kVRAddressOverrides)
        count += cEntry.id == aId ? 1 : 0;
    return count;
}
} // namespace

TEST_CASE("VR has the actor update the copy-AI hook names, apart from its default", "[vr_copy_ai]")
{
    // AE 37356, the flat hook, is SE 36365: the VR function 0x5e0e20, matched by its exception-table entry and the sizes of the
    // functions around it. It is the function the VR hook uses.
    CHECK(VROffsetOf(36365) == 0x5e0e20);
    // The default stays on SE 37356, as before.
    CHECK(VROffsetOf(37356) == 0x6226a0);
    CHECK(TimesListed(36365) == 1);
    CHECK(TimesListed(37356) == 1);
}
