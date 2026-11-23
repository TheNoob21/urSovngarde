#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ModIdTable.h>

namespace ESLoader
{
struct PluginData;
}

struct ModsComponent
{
    using Entry = ModIdTable::Entry;

    uint32_t AddStandard(const String& acpFilename) noexcept;
    uint32_t AddLite(const String& acpFilename) noexcept;

    void AddServerMod(const ESLoader::PluginData& acData);

    const auto& GetStandardMods() const noexcept { return m_standardMods; }
    const auto& GetLiteMods() const noexcept { return m_liteMods; }
    const auto& GetServerMods() const noexcept { return m_serverMods; }

    bool IsInstalled(const String& acpFileName) const noexcept;

    using TModList = ModIdTable::TModList;

private:
    uint32_t m_seed = 0;
    // Mappings of ids owned by the server
    TModList m_standardMods;
    TModList m_liteMods;

    // List of mods installed on the server.
    TModList m_serverMods;
};
