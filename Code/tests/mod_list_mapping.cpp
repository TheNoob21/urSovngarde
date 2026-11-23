#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Systems/ModIdMapping.h>
#include <Systems/UnmatchedIds.h>
#include <Structs/ModIdTable.h>
#include <Messages/NotifyModListsDiffer.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/Message.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace TiltedPhoques;

namespace
{
// A VR plugins.txt (no VRESL), then an SE co-op-lite list.
const char* const kQuestPluginsTxt = R"(# This file is used by Skyrim to keep track of your downloaded content.
# Please do not modify this file.
*BoundDaggerCollisionFix.esp
*Insignificant Object Remover.esp
*ForgivingFriends.esp
*vrik.esp
*higgs_vr.esp
*SpellWheelVR.esp
*WeaponThrowVR.esp
*SkyUI_SE.esp
)";

const char* const kSePluginsTxt = R"(*BoundDaggerCollisionFix.esp
*Insignificant Object Remover.esp
*ForgivingFriends.esp
*SkyUI_SE.esp
)";

struct Plugin
{
    std::string Name;
    bool Lite;
};

// Game masters first, as both games load them before plugins.txt.
std::vector<Plugin> LoadOrder(const char* acpPluginsTxt, bool aVr, bool aForgivingFriendsLite)
{
    std::vector<Plugin> order{{"Skyrim.esm", false}, {"Update.esm", false}, {"Dawnguard.esm", false}, {"HearthFires.esm", false}, {"Dragonborn.esm", false}};
    if (aVr)
        order.push_back({"SkyrimVR.esm", false});
    std::istringstream in(acpPluginsTxt);
    for (std::string line; std::getline(in, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == '*')
            line.erase(0, 1);
        order.push_back({line, aForgivingFriendsLite && line == "ForgivingFriends.esp"});
    }
    return order;
}

// The server's own id rule (ModsComponent hands out ids with ModIdTable).
struct Server
{
    ModIdTable::TModList standard, lite;
    uint32_t Seed = 0;

    uint32_t Add(const Plugin& acMod)
    {
        const String name = acMod.Name.c_str();
        return acMod.Lite ? ModIdTable::Add(lite, standard, name, Seed) : ModIdTable::Add(standard, lite, name, Seed);
    }
};

// One game: its local slots, and the mapping the connect response builds.
struct Game
{
    std::map<std::string, uint32_t> FormPrefix; // local form id high part of each plugin
    ModIdMapping Mapping;
    int Rejected = 0;

    Game(Server& aServer, const std::vector<Plugin>& acOrder)
    {
        uint16_t standardSlot = 0, liteSlot = 0;
        for (const auto& mod : acOrder)
        {
            const uint16_t slot = mod.Lite ? liteSlot++ : standardSlot++;
            FormPrefix[mod.Name] = mod.Lite ? (0xFE000000u | (uint32_t(slot) << 12)) : (uint32_t(slot) << 24);
            if (!Mapping.Add(slot, mod.Lite, aServer.Add(mod), mod.Lite))
                ++Rejected;
        }
    }

    uint32_t Form(const std::string& acName, uint32_t aBase) const { return FormPrefix.at(acName) | (aBase & (FormPrefix.at(acName) >> 24 == 0xFE ? 0xFFFu : 0xFFFFFFu)); }
};

// Form id of object `aBase` of plugin `aName` as game `aFrom` sees it, in game `aTo`.
bool Cross(const Game& acFrom, const Game& acTo, const std::string& acName, uint32_t aBase, uint32_t& aResult)
{
    uint32_t mod = 0, base = 0;
    if (!acFrom.Mapping.ToServer(acFrom.Form(acName, aBase), mod, base))
        return false;
    aResult = acTo.Mapping.ToGame(mod, base);
    return true;
}

const std::vector<std::string> kShared{
    "Skyrim.esm",           "Update.esm",  "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", "BoundDaggerCollisionFix.esp", "Insignificant Object Remover.esp",
    "ForgivingFriends.esp", "SkyUI_SE.esp"};
} // namespace

TEST_CASE("Every plugin the Quest and the SE co-op list share maps both ways", "[mod_list_mapping]")
{
    // ForgivingFriends' ESL flag is cleared on SE (owner choice), so it is a normal plugin on both sides.
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, false));
    Game se(server, LoadOrder(kSePluginsTxt, false, false));
    CHECK(vr.Rejected == 0);
    CHECK(se.Rejected == 0);

    // The same plugin sits in a different slot in each game.
    CHECK(vr.FormPrefix.at("BoundDaggerCollisionFix.esp") != se.FormPrefix.at("BoundDaggerCollisionFix.esp"));
    CHECK(vr.FormPrefix.at("SkyUI_SE.esp") != se.FormPrefix.at("SkyUI_SE.esp"));

    // Literal ids: SkyUI is slot 13 in VR and slot 8 on SE.
    uint32_t literal = 0;
    REQUIRE(Cross(vr, se, "SkyUI_SE.esp", 0x800, literal));
    CHECK(literal == 0x08000800u);
    REQUIRE(Cross(se, vr, "SkyUI_SE.esp", 0x800, literal));
    CHECK(literal == 0x0D000800u);

    for (const auto& name : kShared)
        for (const uint32_t base : {0x000800u, 0x000D62u, 0x012345u})
        {
            uint32_t seForm = 0, vrForm = 0;
            INFO(name << " base " << std::hex << base);
            REQUIRE(Cross(vr, se, name, base, seForm));
            CHECK(seForm == se.Form(name, base));
            REQUIRE(Cross(se, vr, name, base, vrForm));
            CHECK(vrForm == vr.Form(name, base));
        }
}

TEST_CASE("Plugins only one game has stay unmapped on the other", "[mod_list_mapping]")
{
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, false));
    Game se(server, LoadOrder(kSePluginsTxt, false, false));

    for (const char* name : {"SkyrimVR.esm", "vrik.esp", "higgs_vr.esp", "SpellWheelVR.esp", "WeaponThrowVR.esp"})
    {
        uint32_t mod = 0, base = 0, seForm = 99;
        REQUIRE(vr.Mapping.ToServer(vr.Form(name, 0x800), mod, base));
        seForm = se.Mapping.ToGame(mod, base);
        INFO(name);
        CHECK(seForm == 0); // the server id exists, but SE never sent it: no wrong object
    }
}

TEST_CASE("A light plugin on SE that is a normal plugin on VR maps both ways", "[mod_list_mapping]")
{
    // ForgivingFriends is light on SE and normal on VR: the server gives it one id, each game keeps its own kind of slot.
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, false));
    Game se(server, LoadOrder(kSePluginsTxt, false, true));
    CHECK(vr.Rejected == 0);
    CHECK(se.Rejected == 0);
    CHECK(se.FormPrefix.at("ForgivingFriends.esp") == 0xFE000000u);
    REQUIRE(server.lite.count("ForgivingFriends.esp") == 1);
    REQUIRE(server.standard.count("ForgivingFriends.esp") == 1);
    CHECK(server.lite.at("ForgivingFriends.esp").id == server.standard.at("ForgivingFriends.esp").id);

    uint32_t result = 0;
    REQUIRE(Cross(se, vr, "ForgivingFriends.esp", 0x800, result));
    CHECK(result == vr.Form("ForgivingFriends.esp", 0x800));
    CHECK((result >> 24) != 0xFE);
    REQUIRE(Cross(vr, se, "ForgivingFriends.esp", 0xABC, result));
    CHECK(result == 0xFE000ABCu);
    // Everything else is untouched by it.
    REQUIRE(Cross(se, vr, "SkyUI_SE.esp", 0x800, result));
    CHECK(result == vr.Form("SkyUI_SE.esp", 0x800));
}

TEST_CASE("A form past a light plugin's range is not turned into another form", "[mod_list_mapping]")
{
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, false));
    Game se(server, LoadOrder(kSePluginsTxt, false, true));
    uint32_t result = 99;
    REQUIRE(Cross(vr, se, "ForgivingFriends.esp", 0x012345, result));
    CHECK(result == 0); // no form on SE, not 0xFE000345
    REQUIRE(Cross(vr, se, "ForgivingFriends.esp", 0xFFF, result));
    CHECK(result == 0xFE000FFFu);
}

TEST_CASE("A plugin's name is the same in any case", "[mod_list_mapping]")
{
    // The same plugin installed as "skyui_se.esp" in one game and "SkyUI_SE.esp" in the other still gets one id.
    Server server;
    const uint32_t first = server.Add({"SkyUI_SE.esp", false});
    CHECK(server.Add({"skyui_se.ESP", false}) == first);
    CHECK(server.Add({"SKYUI_SE.esp", true}) == first);
    CHECK(server.standard.size() == 1);
    CHECK(server.lite.size() == 1);
    CHECK(server.Add({"Other.esp", false}) != first);
    CHECK(server.standard.at("SkyUI_SE.esp").refCount == 2);
}

TEST_CASE("Plugin lists are compared by name in any case", "[mod_list_mapping]")
{
    const TiltedPhoques::Vector<String> vr{"Skyrim.esm", "SkyrimVR.esm", "vrik.esp", "SkyUI_SE.esp"};
    const TiltedPhoques::Vector<String> se{"Skyrim.esm", "skyui_se.esp", "ccBGSSSE001-Fish.esm"};
    const auto onlyVr = ModIdTable::OnlyIn(vr, se);
    const auto onlySe = ModIdTable::OnlyIn(se, vr);
    REQUIRE(onlyVr.size() == 2);
    CHECK(onlyVr[0] == "SkyrimVR.esm");
    CHECK(onlyVr[1] == "vrik.esp");
    REQUIRE(onlySe.size() == 1);
    CHECK(onlySe[0] == "ccBGSSSE001-Fish.esm");
    CHECK(ModIdTable::OnlyIn(vr, vr).empty());
}

TEST_CASE("A modded worldspace maps by plugin name and local id", "[mod_list_mapping]")
{
    const std::string cWorld = "*Modded Lands.esp\n";
    Server server;
    // On SE the same plugin sits after a different set of plugins than on VR.
    Game vr(server, LoadOrder((std::string(kQuestPluginsTxt) + cWorld).c_str(), true, false));
    Game se(server, LoadOrder(("*Another Mod.esp\n" + cWorld + kSePluginsTxt).c_str(), false, false));
    uint32_t seForm = 0;
    REQUIRE(Cross(vr, se, "Modded Lands.esp", 0x00A001, seForm));
    CHECK(seForm == se.Form("Modded Lands.esp", 0x00A001));
    CHECK((seForm >> 24) != (vr.Form("Modded Lands.esp", 0) >> 24));
}

TEST_CASE("Forms of a plugin only one game loads stay unmatched in the other", "[mod_list_mapping]")
{
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, false));
    Game se(server, LoadOrder(kSePluginsTxt, false, false));
    uint32_t seForm = 99;
    REQUIRE(Cross(vr, se, "higgs_vr.esp", 0x800, seForm));
    CHECK(seForm == 0);
    // The other way: a plugin the Quest list lacks.
    Game seOnly(server, LoadOrder((std::string(kSePluginsTxt) + "*ccBGSSSE001-Fish.esm\n").c_str(), false, false));
    uint32_t vrForm = 99;
    REQUIRE(Cross(seOnly, vr, "ccBGSSSE001-Fish.esm", 0x800, vrForm));
    CHECK(vrForm == 0);
}

TEST_CASE("NotifyModListsDiffer reads back what it wrote", "[mod_list_mapping]")
{
    NotifyModListsDiffer sendMessage;
    sendMessage.Username = "player 2";
    sendMessage.OnlyHere = {"vrik.esp", "SkyrimVR.esm"};
    sendMessage.OnlyThere = {"ccBGSSSE001-Fish.esm"};

    uint8_t storage[1 << 12];
    TiltedPhoques::Buffer buffer(storage, sizeof(storage));
    TiltedPhoques::Buffer::Writer writer(&buffer);
    sendMessage.Serialize(writer);

    TiltedPhoques::Buffer::Reader reader(&buffer);
    const ServerMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(*CastUnique<NotifyModListsDiffer>(std::move(pMessage)) == sendMessage);
}

TEST_CASE("A plugin light in both games maps to the same light form", "[mod_list_mapping]")
{
    // VR with ESL support loads ForgivingFriends light too; the two games share one light id.
    Server server;
    Game vr(server, LoadOrder(kQuestPluginsTxt, true, true));
    Game se(server, LoadOrder(kSePluginsTxt, false, true));
    CHECK(vr.Rejected == 0);
    CHECK(se.Rejected == 0);
    CHECK(server.lite.size() == 1); // one light id, shared by both games
    CHECK(server.standard.count("ForgivingFriends.esp") == 0);

    uint32_t result = 0;
    REQUIRE(Cross(vr, se, "ForgivingFriends.esp", 0x801, result));
    CHECK(result == 0xFE000801u);
    REQUIRE(Cross(se, vr, "ForgivingFriends.esp", 0xABC, result));
    CHECK(result == 0xFE000ABCu);
}

TEST_CASE("Two light plugins missing from the server are reported apart", "[mod_list_mapping]")
{
    // Every light plugin sits in slot 0xFE, so the slot alone cannot tell two missing ones apart.
    UnmatchedIds reported;
    const uint32_t cFirstLight = 0xFE001ABCu;
    const uint32_t cSecondLight = 0xFE002ABCu;
    CHECK(reported.First(UnmatchedIds::LocalKey(cFirstLight)));
    CHECK(reported.First(UnmatchedIds::LocalKey(cSecondLight)));
    CHECK_FALSE(reported.First(UnmatchedIds::LocalKey(0xFE001123u))); // the same plugin, another form
    CHECK_FALSE(reported.First(UnmatchedIds::LocalKey(cSecondLight)));
}

TEST_CASE("A light plugin's report is not a normal plugin's", "[mod_list_mapping]")
{
    UnmatchedIds reported;
    CHECK(UnmatchedIds::LocalKey(0x05000800u) == 5);
    CHECK(UnmatchedIds::LocalKey(0xFE005800u) != UnmatchedIds::LocalKey(0x05000800u));
    CHECK(UnmatchedIds::LocalKey(0xFE000800u) != UnmatchedIds::LocalKey(0x00000800u));
    CHECK(reported.First(UnmatchedIds::LocalKey(0x05000800u)));
    CHECK(reported.First(UnmatchedIds::LocalKey(0xFE005800u)));
    CHECK_FALSE(reported.First(UnmatchedIds::LocalKey(0x05000FFFu)));
}

TEST_CASE("A report starts again after the mapping is rebuilt", "[mod_list_mapping]")
{
    UnmatchedIds reported;
    CHECK(reported.First(7));
    CHECK_FALSE(reported.First(7));
    reported.Clear();
    CHECK(reported.First(7));
}

TEST_CASE("Reporting while the mapping is rebuilt loses nothing and breaks nothing", "[mod_list_mapping]")
{
    UnmatchedIds reported;
    std::atomic<bool> running{true};
    std::vector<std::thread> lookups;
    for (int i = 0; i < 4; ++i)
        lookups.emplace_back(
            [&reported, &running]
            {
                while (running)
                    for (uint32_t key = 0; key < 64; ++key)
                        reported.First(UnmatchedIds::LocalKey(0xFE000000u | (key << 12)));
            });
    for (int i = 0; i < 2000; ++i)
        reported.Clear();
    running = false;
    for (auto& lookup : lookups)
        lookup.join();

    // Whatever happened in between, once things are quiet each plugin is reported exactly once more.
    reported.Clear();
    for (uint32_t key = 0; key < 64; ++key)
    {
        CHECK(reported.First(UnmatchedIds::LocalKey(0xFE000000u | (key << 12))));
        CHECK_FALSE(reported.First(UnmatchedIds::LocalKey(0xFE000000u | (key << 12))));
    }
}

TEST_CASE("Looking up a missing plugin leaves the mapping as it was", "[mod_list_mapping]")
{
    ModIdMapping mapping;
    uint32_t mod = 99, base = 99;
    const ModIdMapping& cMapping = mapping;
    CHECK_FALSE(cMapping.ToServer(0xFE001ABCu, mod, base));
    CHECK(cMapping.ToGame(5, 0x800) == 0);
    CHECK_FALSE(cMapping.ToServer(0xFE001ABCu, mod, base));

    // The plugin arrives later and is found at once: the misses were not remembered.
    REQUIRE(mapping.Add(1, true, 5, true));
    REQUIRE(cMapping.ToServer(0xFE001ABCu, mod, base));
    CHECK(mod == 5);
    CHECK(base == 0xABC);
    CHECK(cMapping.ToGame(5, 0x800) == 0xFE001800u);
}
