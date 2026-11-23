#include <ModContent.h>

#include <catch2/catch.hpp>

namespace
{
using ModContent::Edition;
using ModContent::Manifest;
using TiltedPhoques::String;
using TiltedPhoques::Vector;

Manifest Load(Edition aEdition, std::initializer_list<Mods::Entry> aMods, std::initializer_list<uint64_t> aSizes, const Vector<String>& acPersonal = {})
{
    Mods mods;
    mods.ModList = aMods;
    return ModContent::BuildManifest(mods, ModContentSizes{aEdition, aSizes}, acPersonal);
}
} // namespace

TEST_CASE("Shared content is checked by case-insensitive name and size", "[mod_content]")
{
    const auto cHost = Load(Edition::kSE, {{"Skyrim.esm", 0, false}, {"Common.esp", 0, true}, {"Missing.esl", 1, true}, {"Changed.esp", 3, false}}, {100, 200, 300, 400});
    const auto cGuest = Load(Edition::kSE, {{"Extra.esp", 3, false}, {"changed.esp", 2, false}, {"SKYRIM.ESM", 0, false}, {"common.esp", 1, false}}, {50, 450, 100, 200});
    const auto cDifference = ModContent::Compare(&cHost, cGuest);
    CHECK(cDifference.Missing == Vector<String>{"Missing.esl"});
    CHECK(cDifference.Extra == Vector<String>{"Extra.esp"});
    CHECK(cDifference.Different == Vector<String>{"changed.esp"});
    CHECK(cDifference.Unreadable.empty());
    CHECK_FALSE(cDifference.Incomplete);
    CHECK(ModContent::Describe(cDifference) == "Missing from your game are Missing.esl. Extra plugins in your game are Extra.esp. Different file sizes for changed.esp.");
}

TEST_CASE("SE and VR match across built-in masters and light slot differences", "[mod_content]")
{
    const auto cHost = Load(Edition::kSE, {{"Skyrim.esm", 0, false}, {"Update.esm", 1, false}, {"City.esp", 9, true}}, {100, 60, 200});
    const auto cGuest = Load(Edition::kVR, {{"Skyrim.esm", 0, false}, {"Update.esm", 1, false}, {"SkyrimVR.esm", 5, false}, {"City.esp", 6, false}}, {101, 61, 300, 200});
    CHECK(ModContent::Compare(&cHost, cGuest).Empty());
    CHECK(ModContent::Compare(&cGuest, cHost).Empty());

    auto vrWithESL = cGuest;
    vrWithESL.Plugins.at("city.esp").Size = 201;
    CHECK(ModContent::Compare(&cHost, vrWithESL).Different == Vector<String>{"City.esp"});

    auto sameEdition = cGuest;
    sameEdition.GameEdition = Edition::kSE;
    CHECK(ModContent::Compare(&cHost, sameEdition).Different == Vector<String>{"Skyrim.esm", "Update.esm"});
    sameEdition.Plugins.erase("update.esm");
    CHECK(ModContent::Compare(&cHost, sameEdition).Missing == Vector<String>{"Update.esm"});
}

TEST_CASE("Personal plugins are exempt from the built-in list and the host's setting", "[mod_content]")
{
    const auto personal = ModContent::ParsePersonalPlugins("  Mine.esp , , OTHER.esl\t ");
    REQUIRE(personal == Vector<String>{"Mine.esp", "OTHER.esl"});
    const auto host = Load(
        Edition::kVR, {{"Skyrim.esm", 0, false}, {"SkyrimVR.esm", 1, false}, {"HIGGS_VR.esp", 2, false}, {"Other.esl", 4, true}, {"Shared.esp", 5, false}},
        {100, 200, 300, 600, 700}, personal);
    const auto guest = Load(
        Edition::kSE, {{"Skyrim.esm", 0, false}, {"Precision.esp", 3, false}, {"Mine.esp", 7, false}, {"urSovngardeCoopMenu.esp", 8, false}, {"Extra.esp", 9, false}},
        {100, 50, 200, 300, 400}, personal);
    CHECK(ModContent::Compare(&host, guest).Missing == Vector<String>{"Shared.esp"});
    CHECK(ModContent::Compare(&host, guest).Extra == Vector<String>{"Extra.esp"});
    CHECK(ModContent::Compare(&guest, host).Missing == Vector<String>{"Extra.esp"});
    CHECK(ModContent::Compare(&guest, host).Extra == Vector<String>{"Shared.esp"});
}

TEST_CASE("The supported personal mechanics mods never block a pair", "[mod_content]")
{
    const auto cVanilla = Load(Edition::kSE, {{"Skyrim.esm", 0, false}}, {100});
    const auto cVR = Load(
        Edition::kVR,
        {{"Skyrim.esm", 0, false},
         {"SkyrimVR.esm", 1, false},
         {"vrik.esp", 2, false},
         {"higgs_vr.esp", 3, false},
         {"ImmersiveWeaponPenetrationVR.esp", 4, false},
         {"PhysicalCollisionVR.esp", 5, false}},
        {101, 200, 300, 400, 500, 600});
    const auto cFlat = Load(Edition::kSE, {{"Skyrim.esm", 0, false}, {"Precision.esp", 1, false}}, {100, 200});
    for (const auto* pHost : {&cVanilla, &cVR, &cFlat})
        for (const auto* pGuest : {&cVanilla, &cVR, &cFlat})
            CHECK(ModContent::Compare(pHost, *pGuest).Empty());
}

TEST_CASE("Strict shared content admits vanilla SE and VR in either host order", "[mod_content]")
{
    const auto cSE = Load(
        Edition::kSE, {{"Skyrim.esm", 0, false}, {"Update.esm", 1, false}, {"Dawnguard.esm", 2, false}, {"HearthFires.esm", 3, false}, {"Dragonborn.esm", 4, false}},
        {100, 200, 300, 400, 500});
    const auto cVR = Load(
        Edition::kVR,
        {{"Skyrim.esm", 0, false}, {"Update.esm", 1, false}, {"Dawnguard.esm", 2, false}, {"HearthFires.esm", 3, false}, {"Dragonborn.esm", 4, false}, {"SkyrimVR.esm", 5, false}},
        {101, 201, 301, 401, 501, 701});
    CHECK(ModContent::Compare(nullptr, cSE).Empty());
    CHECK(ModContent::Compare(nullptr, cVR).Empty());
    CHECK(ModContent::Compare(&cSE, cVR).Empty());
    CHECK(ModContent::Compare(&cVR, cSE).Empty());
    CHECK(cSE.Plugins.size() == 5);
    CHECK(cVR.Plugins.size() == 5);

    const auto cUpdatedSE = Load(
        Edition::kSE,
        {{"Skyrim.esm", 0, false},
         {"Update.esm", 1, false},
         {"Dawnguard.esm", 2, false},
         {"HearthFires.esm", 3, false},
         {"Dragonborn.esm", 4, false},
         {"_RESOURCEPACK.ESL", 0, true},
         {"ccBGSSSE001-Fish.esm", 5, false},
         {"ccBGSSSE025-AdvDSGS.esm", 6, false},
         {"ccBGSSSE037-Curios.esl", 1, true},
         {"ccQDRSSE001-SurvivalMode.esl", 2, true}},
        {100, 200, 300, 400, 500, 600, 800, 900, 1000, 1100});
    CHECK(ModContent::Compare(nullptr, cUpdatedSE).Empty());
    CHECK(ModContent::Compare(&cUpdatedSE, cVR).Empty());
    CHECK(ModContent::Compare(&cVR, cUpdatedSE).Empty());
    CHECK(ModContent::Compare(&cUpdatedSE, cSE).Missing.size() == 5);
    CHECK(ModContent::Compare(&cSE, cUpdatedSE).Extra.size() == 5);

    auto vrWithFish = cVR;
    vrWithFish.Plugins.emplace("ccbgssse001-fish.esm", ModContent::Plugin{"ccBGSSSE001-Fish.esm", 800});
    CHECK(ModContent::Compare(&cSE, vrWithFish).Extra == Vector<String>{"ccBGSSSE001-Fish.esm"});
    CHECK(ModContent::Compare(&vrWithFish, cSE).Missing == Vector<String>{"ccBGSSSE001-Fish.esm"});
}

TEST_CASE("Shared ESL and Creation Club content needs a match", "[mod_content]")
{
    const auto cHost = Load(
        Edition::kSE, {{"Skyrim.esm", 0, false}, {"ccBGSSSE001-Fish.esm", 1, false}, {"ScopedBows.esp", 2, false}, {"ccBGSSSE016-Umbra.esm", 4, false}, {"NewSpells.esl", 3, true}},
        {100, 10000, 400, 650, 800});
    const auto cGuest = Load(Edition::kVR, {{"Skyrim.esm", 0, false}, {"ccbgssse001-fish.esm", 1, false}}, {120, 230});
    const auto cDifference = ModContent::Compare(&cHost, cGuest);
    CHECK(cDifference.Different == Vector<String>{"ccbgssse001-fish.esm"});
    CHECK(cDifference.Missing == Vector<String>{"ccBGSSSE016-Umbra.esm", "NewSpells.esl", "ScopedBows.esp"});
}

TEST_CASE("Incomplete or unreadable manifests cannot establish a host", "[mod_content]")
{
    const auto cUnknown = Load(Edition::kUnknown, {{"Skyrim.esm", 0, false}}, {});
    CHECK(ModContent::Compare(nullptr, cUnknown).Incomplete);
    const auto cUnmeasurable = Load(Edition::kVR, {{"Skyrim.esm", 0, false}, {"City.esl", 0, true}}, {100, 0});
    CHECK(ModContent::Compare(nullptr, cUnmeasurable).Unreadable == Vector<String>{"City.esl"});
    const auto cDuplicate = Load(Edition::kSE, {{"Skyrim.esm", 0, false}, {"City.esl", 0, true}, {"city.esl", 1, true}}, {100, 300, 350});
    CHECK(ModContent::Compare(nullptr, cDuplicate).Incomplete);
    const auto cMalformed = Load(Edition::kSE, {{"Skyrim.esm", 0, false}, {"bad/path.esp", 1, false}}, {100, 300});
    CHECK(ModContent::Compare(nullptr, cMalformed).Incomplete);
}

TEST_CASE("Very large differences still name plugins in a bounded notice", "[mod_content]")
{
    ModContentDifference difference;
    difference.Extra.resize(4096, "Extra.esp");
    difference.Missing.resize(4096, "Missing.esp");

    ModContent::LimitNotice(difference);

    CHECK(difference.Truncated);
    CHECK(difference.Extra.size() == 128);
    CHECK(difference.Missing.size() == 128);
    CHECK(ModContent::Describe(difference).find("server debug log level") != String::npos);
}

TEST_CASE("Legacy bot content is a mismatch under strict admission", "[mod_content][bot]")
{
    const auto host = Load(Edition::kVR, {{"Skyrim.esm", 0, false}}, {100});
    const auto bot = Load(Edition::kVR, {{"Skyrim.esm", 0, false}}, {0});
    const auto difference = ModContent::Compare(&host, bot);
    CHECK_FALSE(difference.Empty());
    CHECK(difference.Unreadable == Vector<String>{"Skyrim.esm"});
    const auto measured = Load(Edition::kVR, {{"Skyrim.esm", 0, false}}, {100});
    CHECK(ModContent::Compare(&host, measured).Empty());
}

TEST_CASE("the plain install check leaves out plugins each player chooses, the co-op menu among them", "[mod_content]")
{
    const Vector<String> cPlain{"Skyrim.esm",     "Update.esm",        "Dawnguard.esm",      "HearthFires.esm",
                                "Dragonborn.esm", "_ResourcePack.esl", "SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp"};
    CHECK(ModContent::IsDefaultInstall(cPlain));

    // The mod's own co-op menu plugin, which the install guides ask for, is no departure from the plain install.
    Vector<String> withMenu = cPlain;
    withMenu.push_back("urSovngardeCoopMenu.esp");
    CHECK(ModContent::IsDefaultInstall(withMenu));

    // Another mod still is, and so is a missing plugin or another order.
    Vector<String> withMod = cPlain;
    withMod.push_back("SkyUI_SE.esp");
    CHECK_FALSE(ModContent::IsDefaultInstall(withMod));
    Vector<String> missing = cPlain;
    missing.pop_back();
    CHECK_FALSE(ModContent::IsDefaultInstall(missing));
    Vector<String> swapped = cPlain;
    std::swap(swapped[1], swapped[2]);
    CHECK_FALSE(ModContent::IsDefaultInstall(swapped));
}
