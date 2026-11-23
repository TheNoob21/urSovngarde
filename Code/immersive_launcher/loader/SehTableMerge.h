#pragma once

// One sorted x64 exception table for the game and the launcher together, for Wine.
//
// ExeLoader copies the game's PE headers over the launcher's. Wine looks up the unwind data of an address inside a
// module through the exception directory of that module's headers in memory, so after the copy it only finds the
// game's functions: an exception raised in the launcher's or the client's code never reaches its handler and the
// process ends. Windows keeps using the table it read at load time and is not affected.
//
// Every launcher function lies above the game's last one, so the game's table followed by the launcher's is already
// sorted. PlanMerge checks that and refuses anything else. No Windows headers here, so the tests build on any platform.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace seh_table
{
// RUNTIME_FUNCTION on x64 (ExeLoader.cpp checks the layout matches).
struct RuntimeFunction
{
    uint32_t BeginAddress;
    uint32_t EndAddress;
    uint32_t UnwindData;
};
static_assert(sizeof(RuntimeFunction) == 12, "RUNTIME_FUNCTION is 12 bytes on x64");

enum class Result
{
    kOk,
    kEmpty,          // a table has no entries
    kUnsorted,       // a table is out of order, overlaps itself or has an entry with an empty range
    kTablesOverlap,  // the launcher's first function starts before the game's last one ends
    kNoRoom,         // the merged table does not fit in the free region
    kOverlapsSource, // the free region holds part of a table being merged
};

// Every entry has BeginAddress < EndAddress and starts at or after the previous entry's end,
// which is what a binary search over the table needs.
inline bool IsSorted(const RuntimeFunction* apTable, size_t aCount) noexcept
{
    for (size_t i = 0; i < aCount; ++i)
    {
        if (apTable[i].BeginAddress >= apTable[i].EndAddress)
            return false;
        if (i > 0 && apTable[i].BeginAddress < apTable[i - 1].EndAddress)
            return false;
    }
    return true;
}

struct Plan
{
    uint32_t TableRva = 0;  // where the merged table goes
    uint32_t Count = 0;     // entries in it
    uint32_t SizeBytes = 0; // for DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size
};

namespace detail
{
inline bool RangesOverlap(uint64_t aBegin, uint64_t aEnd, uint64_t aOtherBegin, uint64_t aOtherEnd) noexcept
{
    return aBegin < aOtherEnd && aOtherBegin < aEnd;
}
} // namespace detail

// Checks both tables and places the merged one at the start of the free region [aFreeBeginRva, aFreeEndRva),
// rounded up to 16 bytes. The table RVAs say where the source tables lie, so the region can be checked against them.
inline Result PlanMerge(
    const RuntimeFunction* apGame, size_t aGameCount, uint32_t aGameTableRva, const RuntimeFunction* apLauncher, size_t aLauncherCount, uint32_t aLauncherTableRva,
    uint32_t aFreeBeginRva, uint32_t aFreeEndRva, Plan& aPlan) noexcept
{
    aPlan = {};
    if (!apGame || !apLauncher || aGameCount == 0 || aLauncherCount == 0)
        return Result::kEmpty;
    if (!IsSorted(apGame, aGameCount) || !IsSorted(apLauncher, aLauncherCount))
        return Result::kUnsorted;
    if (apLauncher[0].BeginAddress < apGame[aGameCount - 1].EndAddress)
        return Result::kTablesOverlap;

    constexpr uint64_t cEntrySize = sizeof(RuntimeFunction);
    const uint64_t cCount = static_cast<uint64_t>(aGameCount) + aLauncherCount;
    const uint64_t cBytes = cCount * cEntrySize;
    const uint64_t cTableRva = (static_cast<uint64_t>(aFreeBeginRva) + 15) & ~uint64_t{15};
    if (cBytes > UINT32_MAX || aFreeEndRva <= aFreeBeginRva || cTableRva + cBytes > aFreeEndRva)
        return Result::kNoRoom;

    const uint64_t cGameBegin = aGameTableRva;
    const uint64_t cLauncherBegin = aLauncherTableRva;
    if (detail::RangesOverlap(cTableRva, cTableRva + cBytes, cGameBegin, cGameBegin + aGameCount * cEntrySize) ||
        detail::RangesOverlap(cTableRva, cTableRva + cBytes, cLauncherBegin, cLauncherBegin + aLauncherCount * cEntrySize))
        return Result::kOverlapsSource;

    aPlan.TableRva = static_cast<uint32_t>(cTableRva);
    aPlan.Count = static_cast<uint32_t>(cCount);
    aPlan.SizeBytes = static_cast<uint32_t>(cBytes);
    return Result::kOk;
}

// The game's entries, then the launcher's, into apOut (room for aGameCount + aLauncherCount entries).
inline void WriteMerged(const RuntimeFunction* apGame, size_t aGameCount, const RuntimeFunction* apLauncher, size_t aLauncherCount, RuntimeFunction* apOut) noexcept
{
    std::memcpy(apOut, apGame, aGameCount * sizeof(RuntimeFunction));
    std::memcpy(apOut + aGameCount, apLauncher, aLauncherCount * sizeof(RuntimeFunction));
}
} // namespace seh_table
