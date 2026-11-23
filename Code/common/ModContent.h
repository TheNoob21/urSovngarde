#pragma once

#include <Structs/ModContentDifference.h>
#include <Structs/ModContentSizes.h>
#include <Structs/ModIdTable.h>
#include <Structs/Mods.h>
#include <Structs/PluginList.h>

#include <map>
#include <string_view>

namespace ModContent
{
using Edition = ModContentSizes::Edition;
using TiltedPhoques::Vector;

inline constexpr const char* kPersonalPlugins[]{
#include "PersonalPlugins.inc"
};

inline bool IsPersonal(const String& acName, const Vector<String>& acAdditional) noexcept
{
    return std::any_of(std::begin(kPersonalPlugins), std::end(kPersonalPlugins), [&](const char* apName) { return ModIdTable::SameName(acName, apName); }) ||
           std::any_of(acAdditional.begin(), acAdditional.end(), [&](const String& acPersonal) { return ModIdTable::SameName(acName, acPersonal); });
}

//! The plugins of a plain flat Skyrim SE install with the mod, in load order. A setup with others gets a warning on every load.
inline constexpr const char* kDefaultInstall[]{"Skyrim.esm",     "Update.esm",        "Dawnguard.esm",      "HearthFires.esm",
                                               "Dragonborn.esm", "_ResourcePack.esl", "SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp"};

//! Whether a load order is the plain install. Plugins each player may choose for themselves (IsPersonal: the mod's own co-op
//! menu among them) are left out of the comparison: they are not a difference from the plain install.
inline bool IsDefaultInstall(const Vector<String>& acLoadOrder) noexcept
{
    size_t next = 0;
    for (const String& cName : acLoadOrder)
    {
        if (IsPersonal(cName, {}))
            continue;
        if (next == std::size(kDefaultInstall) || cName != kDefaultInstall[next])
            return false;
        ++next;
    }
    return next == std::size(kDefaultInstall);
}

inline Vector<String> ParsePersonalPlugins(std::string_view aSetting)
{
    Vector<String> plugins;
    while (!aSetting.empty())
    {
        const auto cEnd = aSetting.find(',');
        auto name = aSetting.substr(0, cEnd);
        aSetting.remove_prefix(cEnd == std::string_view::npos ? aSetting.size() : cEnd + 1);

        const auto cFirst = name.find_first_not_of(" \t");
        if (cFirst == std::string_view::npos)
            continue;
        name = name.substr(cFirst, name.find_last_not_of(" \t") - cFirst + 1);
        plugins.emplace_back(name.data(), name.size());
    }
    return plugins;
}

struct Plugin
{
    String Name;
    uint64_t Size{};
};

struct Manifest
{
    Edition GameEdition{};
    std::map<String, Plugin> Plugins;
    bool Incomplete{};
};

inline Manifest BuildManifest(const Mods& acMods, const ModContentSizes& acSizes, const Vector<String>& acPersonal)
{
    Manifest manifest;
    manifest.GameEdition = acSizes.GameEdition;
    manifest.Incomplete = (manifest.GameEdition != Edition::kSE && manifest.GameEdition != Edition::kVR) || acMods.ModList.empty() || acMods.ModList.size() != acSizes.Sizes.size();

    for (size_t i = 0; i < acMods.ModList.size(); ++i)
    {
        const auto& name = acMods.ModList[i].Filename;
        if (name.empty() || name.find_first_of("\\/\r\n") != String::npos || name.find('\0') != String::npos || name.size() > 104)
        {
            manifest.Incomplete = true;
            continue;
        }

        if (IsPersonal(name, acPersonal))
            continue;

        String key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char aChar) { return static_cast<char>(std::tolower(aChar)); });
        const auto cSize = i < acSizes.Sizes.size() ? acSizes.Sizes[i] : 0;
        const auto cInserted = manifest.Plugins.emplace(std::move(key), Plugin{name, cSize}).second;
        manifest.Incomplete |= !cInserted;
    }
    return manifest;
}

// Bethesda ships different copies of these masters with SE and VR. Both games still need the master.
inline bool IsCrossEditionMaster(const String& acKey, Edition aHost, Edition aClient) noexcept
{
    return aHost != Edition::kUnknown && aClient != Edition::kUnknown && aHost != aClient &&
           (acKey == "skyrim.esm" || acKey == "update.esm" || acKey == "dawnguard.esm" || acKey == "hearthfires.esm" || acKey == "dragonborn.esm");
}

// The four free Creations bundled with SE and the SE resource pack have no VR counterpart.
inline bool IsBundledSEOnly(const String& acName, Edition aHere, Edition aThere) noexcept
{
    constexpr const char* cBundled[] = {"_ResourcePack.esl", "ccBGSSSE001-Fish.esm", "ccBGSSSE025-AdvDSGS.esm", "ccBGSSSE037-Curios.esl", "ccQDRSSE001-SurvivalMode.esl"};
    return aHere == Edition::kSE && aThere == Edition::kVR &&
           std::any_of(std::begin(cBundled), std::end(cBundled), [&](const char* apName) { return ModIdTable::SameName(acName, apName); });
}

inline ModContentDifference Compare(const Manifest* apHost, const Manifest& acClient)
{
    ModContentDifference difference;
    difference.Incomplete = acClient.Incomplete;
    for (const auto& [key, plugin] : acClient.Plugins)
    {
        if (plugin.Size == 0)
            difference.Unreadable.push_back(plugin.Name);
        if (!apHost)
            continue;
        const auto cHost = apHost->Plugins.find(key);
        if (cHost == apHost->Plugins.end())
        {
            if (!IsBundledSEOnly(key, acClient.GameEdition, apHost->GameEdition))
                difference.Extra.push_back(plugin.Name);
        }
        else if (plugin.Size != 0 && plugin.Size != cHost->second.Size && !IsCrossEditionMaster(key, apHost->GameEdition, acClient.GameEdition))
            difference.Different.push_back(plugin.Name);
    }

    if (apHost)
        for (const auto& [key, plugin] : apHost->Plugins)
            if (!acClient.Plugins.contains(key) && !IsBundledSEOnly(key, apHost->GameEdition, acClient.GameEdition))
                difference.Missing.push_back(plugin.Name);
    return difference;
}

// Leave room for the existing mod mapping in the same reliable packet.
inline void LimitNotice(ModContentDifference& aDifference)
{
    constexpr size_t cMaxNames = 128;
    for (auto* pList : {&aDifference.Missing, &aDifference.Extra, &aDifference.Different, &aDifference.Unreadable})
    {
        if (pList->size() > cMaxNames)
        {
            pList->resize(cMaxNames);
            aDifference.Truncated = true;
        }
    }
}

inline String Describe(const ModContentDifference& acDifference, size_t aShown = SIZE_MAX)
{
    String text;
    if (acDifference.Incomplete)
        text = "The game sent an incomplete plugin list. ";
    if (!acDifference.Missing.empty())
        text += "Missing from your game are " + PluginList::Joined(acDifference.Missing, aShown) + ". ";
    if (!acDifference.Extra.empty())
        text += "Extra plugins in your game are " + PluginList::Joined(acDifference.Extra, aShown) + ". ";
    if (!acDifference.Different.empty())
        text += "Different file sizes for " + PluginList::Joined(acDifference.Different, aShown) + ". ";
    if (!acDifference.Unreadable.empty())
        text += "Could not read the file size for " + PluginList::Joined(acDifference.Unreadable, aShown) + ". ";
    if (acDifference.Truncated)
        text += "The full list is available at server debug log level. ";
    if (!text.empty())
        text.pop_back();
    return text;
}
} // namespace ModContent
