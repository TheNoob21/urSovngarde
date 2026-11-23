#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

//! What a player was last sent of each thing in its range: characters, dropped items, moved objects.
//! Every shift of a player's loaded cells sent it all of them again, so a party travelling together got a burst at each crossing.
//! Now a thing goes again only when it is new to the player's range, has changed, has not yet reached the player near enough
//! for its game to take it, or was last sent long ago.
namespace ViewerResends
{
using Clock = std::chrono::steady_clock;

//! Sent again anyway once this old: a game may have lost what it was sent, and only a resend gives it back.
constexpr auto kRefreshAfter = std::chrono::seconds(30);
//! Sooner for a character: its spawn also puts right a copy whose updates stopped coming (its place, values, factions, inventory).
constexpr auto kCharacterRefreshAfter = std::chrono::seconds(10);
//! A character's place goes into its stamp this coarsely, so one that has gone this far since its last send is sent again.
constexpr float kPlaceCell = 512.f;
//! Sends this close together are one delivery: a grid shift and the cell entry that follows it come together, while the cells still load.
constexpr auto kSameDelivery = std::chrono::seconds(5);
//! A game takes a dropped item or a moved object only near its player, once the cell's 3D is there; this near, it is.
constexpr float kNearDistance = 4000.f;

struct Record
{
    uint64_t Stamp{};
    //! Sends while near, in a row while it stayed in range unchanged.
    uint8_t NearSends{};
    Clock::time_point SentAt{};
};

//! Folds a value into a stamp of what was sent.
inline uint64_t Mix(const uint64_t aStamp, const uint64_t aValue) noexcept
{
    return aStamp ^ (aValue + 0x9E3779B97F4A7C15ull + (aStamp << 6) + (aStamp >> 2));
}

inline uint64_t MixFloat(const uint64_t aStamp, const float aValue) noexcept
{
    uint32_t bits = 0;
    std::memcpy(&bits, &aValue, sizeof(bits));
    return Mix(aStamp, bits);
}

//! Folds a place into a stamp, by the kPlaceCell cell it is in.
inline uint64_t MixPlace(const uint64_t aStamp, const float aX, const float aY, const float aZ) noexcept
{
    const auto cCell = [](const float aValue) noexcept -> uint64_t
    {
        return std::isfinite(aValue) ? static_cast<uint64_t>(static_cast<int64_t>(std::floor(aValue / kPlaceCell))) : 0;
    };
    return Mix(Mix(Mix(aStamp, cCell(aX)), cCell(aY)), cCell(aZ));
}

//! Whether the thing is sent to the player again.
//! aWasInRange: in range of the player's previous loaded cells too, so it has been sent since it came into range.
//! aNear: near enough that the player's game takes it. aSendsWanted: near sends after which it is not sent again unchanged;
//! more than one where a game can miss the first, its cell's 3D still loading.
//! aRefreshAfter: sent again anyway once the last send is this old.
inline bool IsNeeded(
    const Record* apRecord, const uint64_t aStamp, const bool aWasInRange, const bool aNear, const uint8_t aSendsWanted, const Clock::time_point aNow,
    const Clock::duration aRefreshAfter = kRefreshAfter) noexcept
{
    if (!apRecord || !aWasInRange || apRecord->Stamp != aStamp)
        return true;
    if (aNear && apRecord->NearSends < aSendsWanted)
        return true;
    return aNow - apRecord->SentAt >= aRefreshAfter;
}

//! Whether a shift of a player's loaded cells leaves their centre where it was, in the same worldspace.
//! That is no move but its game announcing its cells again, as it does once it has lost a copy it was sent:
//! everything in range goes again then, whatever the records say.
template <class TWorldSpace, class TCoords>
bool IsReannounce(const TWorldSpace& acPreviousWorldSpace, const TCoords& acPreviousCenter, const TWorldSpace& acWorldSpace, const TCoords& acCenter) noexcept
{
    return static_cast<bool>(acPreviousWorldSpace) && acPreviousWorldSpace == acWorldSpace && acPreviousCenter == acCenter;
}

//! The record after a send.
inline Record AfterSend(const Record* apRecord, const uint64_t aStamp, const bool aWasInRange, const bool aNear, const Clock::time_point aNow) noexcept
{
    const bool cSame = apRecord && aWasInRange && apRecord->Stamp == aStamp;
    if (cSame && aNow - apRecord->SentAt < kSameDelivery)
        return *apRecord;
    const uint32_t cSends = (cSame ? apRecord->NearSends : 0u) + (aNear ? 1u : 0u);
    return Record{aStamp, static_cast<uint8_t>(cSends > 255u ? 255u : cSends), aNow};
}
} // namespace ViewerResends
