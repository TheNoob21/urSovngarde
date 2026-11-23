#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <ActionModIds.h>
#include <Systems/ModIdMapping.h>

namespace
{
// ModSystem's two lookups over a plain mapping.
struct Mods
{
    ModIdMapping Mapping;

    bool GetServerModId(const uint32_t aGameId, GameId& aServerId) const noexcept { return Mapping.ToServer(aGameId, aServerId.ModId, aServerId.BaseId); }
    uint32_t GetGameId(const GameId& acGameId) const noexcept { return Mapping.ToGame(acGameId.ModId, acGameId.BaseId); }
};

// Server mods: 0 Skyrim.esm, 1 Update.esm, 7 a plugin both games load (index 05 on flat, 06 in VR), 9 a light plugin.
void LoadFlat(Mods& aMods)
{
    REQUIRE(aMods.Mapping.Add(0, false, 0, false));
    REQUIRE(aMods.Mapping.Add(1, false, 1, false));
    REQUIRE(aMods.Mapping.Add(5, false, 7, false));
    REQUIRE(aMods.Mapping.Add(0x002, true, 9, true));
}

void LoadVr(Mods& aMods)
{
    REQUIRE(aMods.Mapping.Add(0, false, 0, false));
    REQUIRE(aMods.Mapping.Add(1, false, 1, false));
    REQUIRE(aMods.Mapping.Add(6, false, 7, false));
    REQUIRE(aMods.Mapping.Add(0x011, true, 9, true));
}

ActionEvent SentOver(const ActionEvent& acAction)
{
    Buffer buffer(1000);
    Buffer::Writer writer(&buffer);
    acAction.GenerateDifferential(ActionEvent{}, writer);
    Buffer::Reader reader(&buffer);
    ActionEvent received;
    received.ApplyDifferential(reader);
    return received;
}
} // namespace

TEST_CASE("An action's forms resolve to the same forms in a game whose load order differs", "[action_mod_ids]")
{
    Mods flat, vr;
    LoadFlat(flat);
    LoadVr(vr);

    // Flat: an action of Skyrim.esm, a target in its plugin at index 05, an idle in a light plugin.
    ActionEvent action;
    action.ActionId = 0x00013005;
    action.TargetId = 0x05001234;
    action.IdleId = 0xFE002ABC;
    ActionModIds::Fill(flat, action);
    CHECK(action.ModActionId == GameId(0, 0x13005));
    CHECK(action.ModTargetId == GameId(7, 0x1234));
    CHECK(action.ModIdleId == GameId(9, 0xABC));

    ActionEvent received = SentOver(action);
    REQUIRE(received == action);
    ActionModIds::Resolve(vr, received);
    // The same forms in VR, where that plugin is at index 06.
    CHECK(received.ActionId == 0x00013005);
    CHECK(received.TargetId == 0x06001234);
    CHECK(received.IdleId == 0xFE011ABC);

    // Resolved again (a replay chain copied into a later spawn), it stays the same.
    ActionModIds::Resolve(vr, received);
    CHECK(received.TargetId == 0x06001234);
}

TEST_CASE("An action's forms the other game cannot name fall back as before or resolve to none", "[action_mod_ids]")
{
    Mods flat, vr;
    LoadFlat(flat);
    LoadVr(vr);

    // No target, and an idle from a plugin the server does not list: no mod id, so the raw ids go on as before.
    ActionEvent action;
    action.ActionId = 0x00013005;
    action.IdleId = 0x0A000800;
    ActionModIds::Fill(flat, action);
    CHECK_FALSE(action.ModTargetId);
    CHECK_FALSE(action.ModIdleId);
    ActionModIds::Resolve(vr, action);
    CHECK(action.TargetId == 0);
    CHECK(action.IdleId == 0x0A000800);

    // A temporary target is the sender's own reference: none here, never whatever has that id in this game.
    ActionEvent swing;
    swing.ActionId = 0x00013005;
    swing.TargetId = 0xFF000D0D;
    ActionModIds::Fill(flat, swing);
    CHECK(swing.ModTargetId == GameId(0xFFFFFFFF, 0xD0D));
    ActionModIds::Resolve(vr, swing);
    CHECK(swing.TargetId == 0);

    // A plugin the receiver lacks: no form, not another one.
    Mods bare;
    REQUIRE(bare.Mapping.Add(0, false, 0, false));
    ActionEvent fromPlugin;
    fromPlugin.TargetId = 0x05001234;
    ActionModIds::Fill(flat, fromPlugin);
    ActionModIds::Resolve(bare, fromPlugin);
    CHECK(fromPlugin.TargetId == 0);

    // The server swaps an action for its instant counterpart and drops both its ids: it stays without a form.
    ActionEvent instant;
    instant.ActionId = 0x00013005;
    ActionModIds::Fill(flat, instant);
    instant.ActionId = 0;
    instant.ModActionId = {};
    ActionModIds::Resolve(vr, instant);
    CHECK(instant.ActionId == 0);
}
