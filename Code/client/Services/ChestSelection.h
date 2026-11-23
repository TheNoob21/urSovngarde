#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

// Which containers and doors a game announces to the server (AssignObjectsRequest) as its player moves.
// Outdoors it announces those of the cells around its player too, so a change to a chest in the next cell over reaches the others.
// Only those not announced since they came into those cells: crossing a cell sends the new strip, not every cell again.
// One that left them is announced again when it comes back, as the server forgets an object once no player has it in range.
// The game's own entity for an object goes once the object leaves those cells (ObjectService), as the server forgets it then.
// Plain values in and out, so the unit tests check it without the game.
namespace ChestSelection
{
// How many cells out from the player's own the objects are announced outdoors: the 3x3 around it.
// The game has the 5x5 loaded; the nearer cells keep the first announcement in a town small until it is measured.
constexpr int32_t cCellsOut = 1;

// At most this many objects in one AssignObjectsRequest, which counts its objects in 8 bits.
constexpr size_t cMaxPerRequest = 64;

struct Cell
{
    int32_t X{};
    int32_t Y{};
};

// The grid cells around the player's, its own included, aCellsOut out on each side.
inline std::vector<Cell> CellsAround(const int32_t aX, const int32_t aY, const int32_t aCellsOut) noexcept
{
    std::vector<Cell> cells;
    for (int32_t x = aX - aCellsOut; x <= aX + aCellsOut; ++x)
    {
        for (int32_t y = aY - aCellsOut; y <= aY + aCellsOut; ++y)
            cells.push_back({x, y});
    }
    return cells;
}

// The first and one past the last object of each request, aMax at most in each.
inline std::vector<std::pair<size_t, size_t>> Batches(const size_t aCount, const size_t aMax) noexcept
{
    const size_t cMax = std::max<size_t>(aMax, 1);
    std::vector<std::pair<size_t, size_t>> batches;
    for (size_t first = 0; first < aCount; first += cMax)
        batches.emplace_back(first, std::min(first + cMax, aCount));
    return batches;
}

// The objects announced, by form id, while they stay in the announced cells.
class Announced
{
public:
    // Takes the objects of the announced cells now, by form id, and gives the places in that list of those to announce.
    // An object the earlier cells had and these do not is forgotten, so it is announced again when it comes back.
    std::vector<size_t> Update(const std::vector<uint32_t>& acInCells) noexcept
    {
        std::unordered_set<uint32_t> inCells;
        std::vector<size_t> toAnnounce;
        for (size_t i = 0; i < acInCells.size(); ++i)
        {
            if (inCells.insert(acInCells[i]).second && !m_announced.contains(acInCells[i]))
                toAnnounce.push_back(i);
        }

        m_announced = std::move(inCells);
        return toAnnounce;
    }

    // Nothing announced: on a new connection, or when nothing could be announced from where the player is.
    void Clear() noexcept { m_announced.clear(); }

    size_t Size() const noexcept { return m_announced.size(); }

    // Whether the object, by form id, is in the announced cells.
    bool Contains(const uint32_t aFormId) const noexcept { return m_announced.contains(aFormId); }

private:
    std::unordered_set<uint32_t> m_announced;
};
} // namespace ChestSelection
