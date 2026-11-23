#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include <TiltedCore/Stl.hpp>

//! One copy per other player. A party respawn moves its player far within the loaded area, and the server may send that
//! player again as a new character before the old copy went: the old one then stood where the player died (FF000D0D beside
//! the live FF000EF1). When a player's copy is made here, an older one of the same player goes.
namespace PlayerCopyRules
{
//! What becomes of an older copy of the player a new copy was just made for.
enum class Older
{
    kKeep,       //!< none, or the new copy is that same actor
    kRemoveSeen, //!< still the server's: removed as the server's own removal would (its server id is another one)
    kDelete,     //!< no longer the server's character here: deleted as a copy this game made
};

//! aOlderFormId: the copy last made here for this player, 0 for none; aNewFormId: the one just made.
//! aOlderExists: it still is a remote player copy this game made; aOlderTracked: an entity still has it as a server character.
inline Older OnPlayerCopyMade(const uint32_t aOlderFormId, const uint32_t aNewFormId, const bool aOlderExists, const bool aOlderTracked) noexcept
{
    if (aOlderFormId == 0 || aOlderFormId == aNewFormId || !aOlderExists)
        return Older::kKeep;
    return aOlderTracked ? Older::kRemoveSeen : Older::kDelete;
}

//! The copy last made here for a player. The game gives a freed form id to the next actor it makes, another player's copy
//! included, so the copy is known by its handle too. Player ids are not reused while the server runs, and the records go with
//! the connection.
struct Record
{
    uint32_t PlayerId{};
    uint32_t FormId{};
    uint32_t Handle{};
    //! The server id it was made for, which names the ride this game put it on (MountService::ReleaseBeforeDelete).
    uint32_t ServerId{};
};

//! What the game has for a record now.
struct Found
{
    //! The handle of the actor on the record's form id now, 0 for none.
    uint32_t Handle{};
    //! That actor is a remote player copy this game made.
    bool OwnCopy{};
    //! An entity still has it as a server character.
    bool Tracked{};
};

//! What becomes of the copy remembered for aPlayerId when aNewFormId was just made for it: only that same actor goes, never
//! another one that came to have its form id.
inline Older OnRememberedCopy(const Record& acRecord, const uint32_t aPlayerId, const uint32_t aNewFormId, const Found& acFound) noexcept
{
    const bool cSameActor = acRecord.PlayerId == aPlayerId && acRecord.Handle != 0 && acFound.Handle == acRecord.Handle;
    return OnPlayerCopyMade(acRecord.FormId, aNewFormId, cSameActor && acFound.OwnCopy, acFound.Tracked);
}

//! Whether a player's spawn is that player's newest character. The server numbers each player character it makes
//! (CharacterSpawnRequest::SpawnGeneration): one older than a character already seen here for that player is stale, even when it
//! arrives last, and makes no copy. 0 is not numbered (an older server) and counts as newest, as before.
enum class Spawn
{
    kNewest,
    kStale,
};

inline Spawn OnPlayerSpawn(const uint32_t aNewestSeen, const uint32_t aGeneration) noexcept
{
    return aGeneration != 0 && aGeneration < aNewestSeen ? Spawn::kStale : Spawn::kNewest;
}

//! The copy last made here for each other player.
class Remembered
{
public:
    //! The copy just made for its player: the one remembered before for that player, if any, is handed back.
    std::optional<Record> Remember(const Record& acRecord) noexcept
    {
        std::optional<Record> before;
        if (const auto it = m_records.find(acRecord.PlayerId); it != m_records.end())
            before = it->second;
        m_records[acRecord.PlayerId] = acRecord;
        return before;
    }

    //! An actor deleted here: the record of that actor goes. aHandle is 0 when the actor is gone already.
    void Forget(const uint32_t aFormId, const uint32_t aHandle) noexcept
    {
        for (auto it = m_records.begin(); it != m_records.end();)
        {
            const bool cThatActor = it->second.FormId == aFormId && (aHandle == 0 || it->second.Handle == aHandle);
            it = cThatActor ? m_records.erase(it) : std::next(it);
        }
    }

    [[nodiscard]] std::optional<Record> Of(const uint32_t aPlayerId) const noexcept
    {
        const auto it = m_records.find(aPlayerId);
        return it != m_records.end() ? std::optional<Record>(it->second) : std::nullopt;
    }

    //! A player's newest character seen here; kept when its copy goes, as the server may still send an older one.
    void SawGeneration(const uint32_t aPlayerId, const uint32_t aGeneration) noexcept
    {
        uint32_t& newest = m_generations[aPlayerId];
        newest = std::max(newest, aGeneration);
    }

    [[nodiscard]] uint32_t NewestGeneration(const uint32_t aPlayerId) const noexcept
    {
        const auto it = m_generations.find(aPlayerId);
        return it != m_generations.end() ? it->second : 0;
    }

    void Clear() noexcept
    {
        m_records.clear();
        m_generations.clear();
    }

private:
    TiltedPhoques::Map<uint32_t, Record> m_records;
    TiltedPhoques::Map<uint32_t, uint32_t> m_generations;
};
} // namespace PlayerCopyRules
