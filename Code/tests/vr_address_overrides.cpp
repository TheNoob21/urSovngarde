#include <catch2/catch.hpp>

#include <VRAddressOverrides.h>

TEST_CASE("VR finds the clone call at the address its library gives", "[vr_address_overrides]")
{
    // SE 68835 NiObject::Clone, read in SkyrimVR 1.4.15: its own NiCloningProcess, CreateClone then ProcessClone.
    uint32_t found = 0, offset = 0;
    for (const auto& cEntry : kVRAddressOverrides)
        if (cEntry.id == 68835)
        {
            ++found;
            offset = cEntry.offset;
        }
    CHECK(found == 1);
    CHECK(offset == 0xc97810);
}

TEST_CASE("VR walks a target's active effects with the game's own visitor", "[vr_address_overrides]")
{
    // AE 34540 / SE 33756 MagicTarget::VisitEffects, read in SkyrimVR 1.4.15: it walks the lock-free list
    // (BSLocklessSimpleList<ActiveEffect*>, shared-pointer nodes) that SE's simple list layout would misread.
    uint32_t found = 0, offset = 0;
    for (const auto& cEntry : kVRAddressOverrides)
        if (cEntry.id == 34540)
        {
            ++found;
            offset = cEntry.offset;
        }
    CHECK(found == 1);
    CHECK(offset == 0x558580);
}
