#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// Which entity has a server id, kept as the components that carry one (LocalComponent, RemoteComponent, ObjectComponent) are set,
// changed and removed, so a lookup does not walk every entity. An actor this game runs comes before a copy, and a copy before an
// object, as Utils::FindEntityByServerId has always taken them.
// Plain values in and out, so the unit tests check it without the game.
namespace ServerIdIndex
{
enum class Kind : uint8_t
{
    kLocal,
    kRemote,
    kObject,
    kCount
};

template <class TEntity> class Index
{
public:
    // The entity's id of this kind is now aId; the one it had before is forgotten.
    void Set(const Kind aKind, const TEntity aEntity, const uint32_t aId) noexcept
    {
        auto& ids = m_ids[Slot(aKind)];
        if (const auto it = ids.find(aEntity); it != ids.end())
        {
            if (it->second == aId)
                return;
            Unlist(aKind, aEntity, it->second);
            it->second = aId;
        }
        else
            ids.emplace(aEntity, aId);
        m_entities[aId].push_back({aKind, aEntity});
    }

    // The entity no longer has an id of this kind.
    void Erase(const Kind aKind, const TEntity aEntity) noexcept
    {
        auto& ids = m_ids[Slot(aKind)];
        const auto it = ids.find(aEntity);
        if (it == ids.end())
            return;
        Unlist(aKind, aEntity, it->second);
        ids.erase(it);
    }

    // The entity with the id: one this game runs first, then a copy, then an object.
    [[nodiscard]] std::optional<TEntity> Find(const uint32_t aId) const noexcept
    {
        const auto it = m_entities.find(aId);
        if (it == m_entities.end() || it->second.empty())
            return std::nullopt;
        const auto cFirst = std::min_element(it->second.begin(), it->second.end(), [](const Entry& acLeft, const Entry& acRight) { return acLeft.What < acRight.What; });
        return cFirst->Entity;
    }

    // The entity whose id of this kind is aId.
    [[nodiscard]] std::optional<TEntity> Find(const Kind aKind, const uint32_t aId) const noexcept
    {
        const auto it = m_entities.find(aId);
        if (it == m_entities.end())
            return std::nullopt;
        for (const Entry& cEntry : it->second)
            if (cEntry.What == aKind)
                return cEntry.Entity;
        return std::nullopt;
    }

    void Clear() noexcept
    {
        for (auto& ids : m_ids)
            ids.clear();
        m_entities.clear();
    }

    // How many ids have an entity.
    [[nodiscard]] size_t Size() const noexcept { return m_entities.size(); }

private:
    struct Entry
    {
        Kind What;
        TEntity Entity;
    };

    static size_t Slot(const Kind aKind) noexcept { return static_cast<size_t>(aKind); }

    void Unlist(const Kind aKind, const TEntity aEntity, const uint32_t aId) noexcept
    {
        const auto it = m_entities.find(aId);
        if (it == m_entities.end())
            return;
        auto& entries = it->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [aKind, aEntity](const Entry& acEntry) { return acEntry.What == aKind && acEntry.Entity == aEntity; }), entries.end());
        if (entries.empty())
            m_entities.erase(it);
    }

    std::array<std::unordered_map<TEntity, uint32_t>, static_cast<size_t>(Kind::kCount)> m_ids;
    std::unordered_map<uint32_t, std::vector<Entry>> m_entities;
};
} // namespace ServerIdIndex
