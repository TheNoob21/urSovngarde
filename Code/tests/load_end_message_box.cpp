#include <catch2/catch.hpp>

#include <Games/Skyrim/Interface/LoadEndMessageBox.h>
#include <VRAddressOverrides.h>

using LoadEndMessageBox::kShowReturnOffset;
using LoadEndMessageBox::PendingOrUnknown;
using LoadEndMessageBox::ShouldDrop;

TEST_CASE("the load-end show is dropped only with no box pending", "[menus][vr_message_box]")
{
    // The empty show as the loading screen goes down or the main menu comes up: nothing to show.
    CHECK(ShouldDrop(true, false, kShowReturnOffset, true, 0));
    // A box queued during the load (such as "continue loading this save?") waits for this show.
    CHECK_FALSE(ShouldDrop(true, false, kShowReturnOffset, true, 1));
    CHECK_FALSE(ShouldDrop(true, false, kShowReturnOffset, true, 3));
    // A list whose address is not known counts as a box pending.
    const uint32_t cNone = 0;
    CHECK(ShouldDrop(true, false, kShowReturnOffset, true, PendingOrUnknown(&cNone)));
    CHECK_FALSE(ShouldDrop(true, false, kShowReturnOffset, true, PendingOrUnknown(nullptr)));
}

TEST_CASE("the load-end drop keeps its old limits", "[menus][vr_message_box]")
{
    // In play the same show once carried a real box (35 s after a level-up), and dropping it crashed the game.
    CHECK_FALSE(ShouldDrop(true, false, kShowReturnOffset, false, 0));
    // Another caller, such as MessageBoxData::QueueMessage (+0x8d8683), always comes with a box.
    CHECK_FALSE(ShouldDrop(true, false, 0x8d8683, true, 0));
    // Only a show, and only one with no data.
    CHECK_FALSE(ShouldDrop(false, false, kShowReturnOffset, true, 0));
    CHECK_FALSE(ShouldDrop(true, true, kShowReturnOffset, true, 0));
}

TEST_CASE("VR pins the pending message box list", "[menus][vr_message_box]")
{
    // SE 519819: VR Address Library 0.298 has the same address, and MessageBoxMenu's show reads its count at +0x10 (0x143013AD8).
    uint32_t offset = 0;
    uint32_t times = 0;
    for (const auto& cEntry : kVRAddressOverrides)
    {
        if (cEntry.id == 519819u)
        {
            offset = cEntry.offset;
            ++times;
        }
    }
    CHECK(times == 1);
    CHECK(offset == 0x3013ac8u);
}
