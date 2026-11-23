
#include <Components.h>
#include <algorithm>
#include <es_loader/ESLoader.h>
#include <Structs/ModIdTable.h>

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    return ModIdTable::Add(m_standardMods, m_liteMods, acpFilename, m_seed);
}

uint32_t ModsComponent::AddLite(const String& acpFilename) noexcept
{
    return ModIdTable::Add(m_liteMods, m_standardMods, acpFilename, m_seed);
}

void ModsComponent::AddServerMod(const ESLoader::PluginData& acData)
{
    // kind of a hack since we want to store both, so we take the two byte value
    m_serverMods.emplace(acData.m_filename, Entry{acData.m_liteId, 1});
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    return std::any_of(m_serverMods.begin(), m_serverMods.end(), [&acpFilename](const TModList::value_type& acEntry) { return ModIdTable::SameName(acEntry.first, acpFilename); });
}
