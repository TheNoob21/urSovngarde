#pragma once

#include <Systems/ModIdMapping.h>

#include <Systems/UnmatchedIds.h>

struct Mods;
struct NotifyModListsDiffer;
struct DisconnectedEvent;
struct GameId;

/**
 * @brief Makes sure that different clients can have independent load orders and mods.
 */
struct ModSystem
{
    ModSystem(entt::dispatcher& aDispatcher) noexcept;
    ~ModSystem() = default;

    TP_NOCOPYMOVE(ModSystem);

    bool GetServerModId(uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept;
    bool GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept;
    uint32_t GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept;
    uint32_t GetGameId(const GameId& acGameId) const noexcept;

    bool GetSharedSpellId(uint32_t aLocalId, GameId& aSpell) const noexcept;
    uint32_t GetLocalSpellId(const GameId& acSpell) const noexcept;

protected:
    void HandleMods(const Mods& acMods) noexcept;
    void HandleDisconnected(const DisconnectedEvent&) noexcept;
    void HandleModListsDiffer(const NotifyModListsDiffer& acMessage) const noexcept;

private:
    entt::scoped_connection m_modsConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_modListsDifferConnection;
    ModIdMapping m_mapping;
    // Mods and plugins already logged as unmatched; the lookups above change nothing else.
    mutable UnmatchedIds m_unmatchedServerIds;
    mutable UnmatchedIds m_unmatchedLocalIds;
};
