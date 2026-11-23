#pragma once

// The words of power a party was taught, so each goes to its members once: plain values, so the rule is unit-tested.
//
// A word wall, or a quest, teaches the word in the game of the player there. A quest runs in every member's game, so each
// of them may send the same word; the party is told it the first time only.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PartyWords
{
public:
    // aWord: the word's server id, its mod id in the upper half.
    [[nodiscard]] static constexpr uint64_t Key(uint32_t aModId, uint32_t aBaseId) noexcept { return (static_cast<uint64_t>(aModId) << 32) | aBaseId; }

    // More than the game has words, with room for mods. A party past it is told no more.
    // The same as what a NotifyWordLearned holds.
    static constexpr size_t kMaxPerParty = 512;

    // No form has a base id of 0: that is a message that carries no word.
    [[nodiscard]] static constexpr bool IsValid(uint64_t aWord) noexcept { return static_cast<uint32_t>(aWord) != 0; }

    // Whether the party has not been told this word yet; it has been from now on. Party ids are never used twice.
    // Whether it is a word of power is for each game to check: the server has no game data.
    bool IsNew(uint32_t aPartyId, uint64_t aWord)
    {
        if (!IsValid(aWord))
            return false;
        auto& words = m_words[aPartyId];
        return words.size() < kMaxPerParty && words.insert(aWord).second;
    }

    // The party is gone: what it was told goes with it.
    void Forget(uint32_t aPartyId) { m_words.erase(aPartyId); }

    // The words the party was told, for a member who joins it later.
    [[nodiscard]] std::vector<uint64_t> Of(uint32_t aPartyId) const
    {
        const auto cIt = m_words.find(aPartyId);
        return cIt == m_words.end() ? std::vector<uint64_t>{} : std::vector<uint64_t>(cIt->second.begin(), cIt->second.end());
    }

private:
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>> m_words;
};

// The words a party member was taught, waiting for this game to teach them: none is taught while a save loads here.
// Each belongs to the connection and party it came in: once this game connects, disconnects, joins or leaves a party, the
// words that came before are dropped, so none is taught in a later game alone or with another party.
class WordGrants
{
public:
    // A connection, a disconnection, a party joined or left.
    void NewTerm() noexcept
    {
        ++m_term;
        m_waiting.clear();
    }

    // The save system's persistent player ID, captured when this connection begins.
    // Names, the player reference (always 0x14), and a settling timer cannot identify a save's player.
    void BindPlayer(uint64_t aPlayerId) noexcept
    {
        NewTerm();
        m_player = aPlayerId;
    }

    // A word waits once, and no more wait than a party can be told.
    void Add(uint32_t aWordId)
    {
        if (m_waiting.size() >= PartyWords::kMaxPerParty)
            return;
        const auto cIt = std::find_if(m_waiting.begin(), m_waiting.end(), [aWordId](const Grant& acGrant) { return acGrant.WordId == aWordId; });
        if (cIt == m_waiting.end())
            m_waiting.push_back({aWordId, m_term, m_player});
    }

    // The words to teach now, none while the game cannot take them. A word stays until it is taught.
    [[nodiscard]] std::vector<uint32_t> Due(bool aReadable, uint64_t aPlayerId, bool aLoading = false) const
    {
        std::vector<uint32_t> due;
        if (!aReadable || aLoading || !m_player || aPlayerId != m_player)
            return due;
        for (const Grant& cGrant : m_waiting)
        {
            if (cGrant.Term == m_term && cGrant.Player == aPlayerId)
                due.push_back(cGrant.WordId);
        }
        return due;
    }

    void Taught(uint32_t aWordId)
    {
        std::erase_if(m_waiting, [aWordId](const Grant& acGrant) { return acGrant.WordId == aWordId; });
    }

    [[nodiscard]] size_t Size() const noexcept { return m_waiting.size(); }

private:
    struct Grant
    {
        uint32_t WordId;
        uint32_t Term;
        uint64_t Player;
    };

    std::vector<Grant> m_waiting;
    uint32_t m_term{};
    uint64_t m_player{};
};
