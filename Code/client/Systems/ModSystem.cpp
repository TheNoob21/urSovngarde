#include <Services/GameExtension.h>
#include <TiltedOnlinePCH.h>

#include <Systems/ModSystem.h>

#include <Events/DisconnectedEvent.h>
#include <Structs/Mods.h>
#include <Structs/GameId.h>
#include <Messages/NotifyModListsDiffer.h>
#include <Structs/PluginList.h>

#include <Services/OverlayService.h>
#include <UI/Banner/VRBanner.h>
#include <World.h>

#include <Games/TES.h>

ModSystem::ModSystem(entt::dispatcher& aDispatcher) noexcept
{
    m_modsConnection = aDispatcher.sink<Mods>().connect<&ModSystem::HandleMods>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ModSystem::HandleDisconnected>(this);
    m_modListsDifferConnection = aDispatcher.sink<NotifyModListsDiffer>().connect<&ModSystem::HandleModListsDiffer>(this);
}

bool ModSystem::GetServerModId(const uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept
{
    if (m_mapping.ToServer(aGameId, aModId, aBaseId))
        return true;

    // The form is from a plugin this game did not send to the server, so it cannot be told to the others.
    if (m_unmatchedLocalIds.First(UnmatchedIds::LocalKey(aGameId)))
        spdlog::debug("Form {:X} is from a plugin with no server id, it is not synced", aGameId);
    return false;
}

bool ModSystem::GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept
{
    return GetServerModId(aGameId, aServerId.ModId, aServerId.BaseId);
}

uint32_t ModSystem::GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept
{
    const uint32_t cGameId = m_mapping.ToGame(aServerId, aFormId);
    if (cGameId == 0 && (aServerId != 0 || aFormId != 0) && m_unmatchedServerIds.First(aServerId))
        spdlog::debug(
            "Form {:X} of server mod {} is not matched in this game (a plugin this game lacks, or a form past a light plugin's range), it falls back to no form", aFormId,
            aServerId);
    return cGameId;
}

uint32_t ModSystem::GetGameId(const GameId& acGameId) const noexcept
{
    return GetGameId(acGameId.ModId, acGameId.BaseId);
}

bool ModSystem::GetSharedSpellId(uint32_t aId, GameId& aSpell) const noexcept
{
    if (GetServerModId(aId, aSpell) && !GameExtension::RejectMagic(aSpell))
        return true;
    aSpell = {};
    return false;
}
uint32_t ModSystem::GetLocalSpellId(const GameId& acSpell) const noexcept
{
    return GameExtension::RejectMagic(acSpell) ? 0 : GetGameId(acSpell);
}

void ModSystem::HandleMods(const Mods& acMods) noexcept
{
    GameExtension::Module(0, nullptr);
    m_mapping.Reset();
    m_unmatchedServerIds.Clear();
    m_unmatchedLocalIds.Clear();
    const auto pModManager = ModManager::Get();
    for (const auto& mod : acMods.ModList)
    {
        GameExtension::Module(mod.Id, mod.Filename.c_str());
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
    GameExtension::Module(0, nullptr);
    m_mapping.Reset();
    m_unmatchedServerIds.Clear();
    m_unmatchedLocalIds.Clear();
}

// The server tells both players of a pair once when their plugin lists differ; what only one game loads cannot be matched in the other.
void ModSystem::HandleModListsDiffer(const NotifyModListsDiffer& acMessage) const noexcept
{
    constexpr uint32_t cPlaceColour = 0xD4AF37;
    constexpr size_t cShown = 6;

    const std::string cWho = acMessage.Username.c_str();

    spdlog::debug("Mod lists: {} loads other plugins: {} only there, {} only here", cWho, acMessage.OnlyThere.size(), acMessage.OnlyHere.size());

    std::string message = "You and " + cWho + " don't load the same plugins, so things from a plugin only one of you has won't show for the other.";
    if (!acMessage.OnlyThere.empty())
        message += " Only " + cWho + " has: " + PluginList::Joined(acMessage.OnlyThere, cShown).c_str() + ".";
    if (!acMessage.OnlyHere.empty())
        message += " Only you have: " + std::string(PluginList::Joined(acMessage.OnlyHere, cShown).c_str()) + ".";

    World::Get().GetOverlayService().SendSystemMessage(message);
    VRBanner::Show("Not the same plugins as " + cWho, cPlaceColour, 60, 8.f);
}
