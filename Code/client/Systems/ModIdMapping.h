#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>

// Translates form IDs without depending on game objects or wire serialization.
class ModIdMapping
{
public:
    void Reset() noexcept
    {
        m_standard = {};
        m_lite.clear();
        m_server.clear();
    }

    bool Add(uint16_t aLocalId, bool aLocalLite, uint32_t aServerId, bool aServerLite)
    {
        if (aLocalLite != aServerLite || (aLocalLite ? aLocalId > 0xFFF : aLocalId >= 0xFE) ||
            aServerId == std::numeric_limits<uint32_t>::max() || m_server.count(aServerId) ||
            (aLocalLite ? m_lite.count(aLocalId) != 0 : m_standard[aLocalId].present))
            return false;

        m_server.emplace(aServerId, LocalMod{aLocalId, aLocalLite});
        if (aLocalLite)
            m_lite.emplace(aLocalId, aServerId);
        else
            m_standard[aLocalId] = {aServerId, true};
        return true;
    }

    bool ToServer(uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept
    {
        aModId = 0;
        aBaseId = 0;
        if (aGameId == 0)
            return true;
        const uint32_t hi = aGameId >> 24;
        if (hi == 0xFF)
        {
            aModId = std::numeric_limits<uint32_t>::max();
            aBaseId = aGameId & 0xFFFFFF;
            return true;
        }
        if (hi == 0xFE)
        {
            const auto it = m_lite.find(static_cast<uint16_t>((aGameId >> 12) & 0xFFF));
            if (it == m_lite.end())
                return false;
            aModId = it->second;
            aBaseId = aGameId & 0xFFF;
            return true;
        }
        if (!m_standard[hi].present)
            return false;
        aModId = m_standard[hi].serverId;
        aBaseId = aGameId & 0xFFFFFF;
        return true;
    }

    uint32_t ToGame(uint32_t aServerId, uint32_t aBaseId) const noexcept
    {
        if (aServerId == 0 && aBaseId == 0)
            return 0;
        const auto it = m_server.find(aServerId);
        if (it == m_server.end())
            return 0;
        const auto& mod = it->second;
        return mod.lite ? (0xFE000000u | (uint32_t(mod.id) << 12) | (aBaseId & 0xFFFu))
                        : ((uint32_t(mod.id) << 24) | (aBaseId & 0xFFFFFFu));
    }

private:
    struct StandardMod { uint32_t serverId = 0; bool present = false; };
    struct LocalMod { uint16_t id; bool lite; };
    std::array<StandardMod, 0xFE> m_standard{};
    std::unordered_map<uint16_t, uint32_t> m_lite;
    std::unordered_map<uint32_t, LocalMod> m_server;
};
