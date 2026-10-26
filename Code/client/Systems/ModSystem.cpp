#include <TiltedOnlinePCH.h>

#include <Systems/ModSystem.h>

#include <Events/DisconnectedEvent.h>
#include <Structs/Mods.h>
#include <Structs/GameId.h>

#include <Games/TES.h>

ModSystem::ModSystem(entt::dispatcher& aDispatcher) noexcept
{
    m_modsConnection = aDispatcher.sink<Mods>().connect<&ModSystem::HandleMods>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ModSystem::HandleDisconnected>(this);
}

bool ModSystem::GetServerModId(const uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept
{
    return m_mapping.ToServer(aGameId, aModId, aBaseId);
}

bool ModSystem::GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept
{
    return GetServerModId(aGameId, aServerId.ModId, aServerId.BaseId);
}

uint32_t ModSystem::GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept
{
    return m_mapping.ToGame(aServerId, aFormId);
}

uint32_t ModSystem::GetGameId(const GameId& acGameId) const noexcept
{
    return GetGameId(acGameId.ModId, acGameId.BaseId);
}

void ModSystem::HandleMods(const Mods& acMods) noexcept
{
    m_mapping.Reset();
    const auto pModManager = ModManager::Get();
    for (const auto& mod : acMods.ModList)
    {
        if (Mod* pMod = pModManager->GetByName(mod.Filename.c_str()))
        {
            if (!m_mapping.Add(pMod->GetId(), pMod->IsLite(), mod.Id, mod.IsLite))
                spdlog::error("Invalid or conflicting mapping for mod {}, server lite? {}, local lite? {}, id: {:X}", mod.Filename.c_str(), mod.IsLite, pMod->IsLite(), mod.Id);
        }
        else
        {
            spdlog::error("Failed to find mod {}, is lite? {}, id: {:X}", mod.Filename.c_str(), mod.IsLite, mod.Id);
        }
    }
}

void ModSystem::HandleDisconnected(const DisconnectedEvent&) noexcept
{
    m_mapping.Reset();
}
