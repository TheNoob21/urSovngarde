#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

//! Which party a joining player is put in, and whom a leader may kick.
namespace PartyRules
{
struct Party
{
    uint32_t Id{};
    size_t Members{};
};

//! The largest party, the oldest on a tie (ids count up); none while there is no party.
//! Joining only while exactly one party existed left a player in none once somebody had made a second one.
inline std::optional<uint32_t> PartyToJoin(const std::vector<Party>& acParties) noexcept
{
    std::optional<uint32_t> best;
    size_t bestMembers = 0;
    for (const Party& cParty : acParties)
    {
        if (!best || cParty.Members > bestMembers || (cParty.Members == bestMembers && cParty.Id < *best))
        {
            best = cParty.Id;
            bestMembers = cParty.Members;
        }
    }
    return best;
}

//! A leader kicks only a member of its own party.
inline bool MayKick(const std::optional<uint32_t> aKickerParty, const bool aKickerLeads, const std::optional<uint32_t> aTargetParty) noexcept
{
    return aKickerParty && aKickerLeads && aTargetParty == aKickerParty;
}
} // namespace PartyRules
