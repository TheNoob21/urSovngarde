#include <TiltedCore/Stl.hpp>
#include <catch2/catch.hpp>
#include <PluginRules.h>

using namespace TiltedPhoques;

TEST_CASE("NPC plugin warnings use the admission personal list", "[plugin_rules]")
{
    Vector<String> here{"SkyrimVR.esm", "HIGGS_VR.esp", "City.esp", "Custom.esp"};
    Vector<String> there{"ccBGSSSE001-Fish.esm", "precision.esp", "Other.esp"};
    const auto leftOut = PluginRules::FilterForPair(here, true, there, true, {"CUSTOM.ESP"});
    CHECK(here == Vector<String>{"City.esp"});
    CHECK(there == Vector<String>{"ccBGSSSE001-Fish.esm", "Other.esp"});
    CHECK(leftOut.Here == Vector<String>{"SkyrimVR.esm", "HIGGS_VR.esp", "Custom.esp"});
    CHECK(leftOut.There == Vector<String>{"precision.esp"});
}

TEST_CASE("NPC plugin warnings omit bundled Creations on the SE side of a mixed pair", "[plugin_rules]")
{
    Vector<String> flat{"CCBGSSSE001-FISH.ESM", "_ResourcePack.esl", "ccBGSSSE016-Umbra.esm"};
    Vector<String> vr{"ccBGSSSE025-AdvDSGS.esm"};
    const auto leftOut = PluginRules::FilterForPair(vr, false, flat, true, {});
    CHECK(flat == Vector<String>{"ccBGSSSE016-Umbra.esm"});
    CHECK(vr == Vector<String>{"ccBGSSSE025-AdvDSGS.esm"});
    CHECK(leftOut.Here.empty());
    CHECK(leftOut.There == Vector<String>{"CCBGSSSE001-FISH.ESM", "_ResourcePack.esl"});
}
