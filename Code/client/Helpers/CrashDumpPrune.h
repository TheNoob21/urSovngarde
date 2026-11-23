#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/**
 * Which crash dumps of earlier launches are removed when the game starts.
 * Each launch writes into its own `logs\crash-run-<ticks>-<pid>-<n>` folder, and the in-process history (one full,
 * three small) does not reach across launches, so ~1 GB full dumps piled up one per crashed session.
 * Kept: the full dumps of the newest run that has one, the small dumps of the newest three runs that have any.
 * Pure, so it is tested without the file system (tests/crash_dump_prune.cpp).
 */
namespace CrashDumpPrune
{
// How many runs keep their small dumps (each run keeps at most three itself).
inline constexpr size_t kSmallRunsKept = 3;

struct RunKey
{
    uint64_t Ticks = 0;
    uint64_t Pid = 0;
    uint64_t Sequence = 0;

    [[nodiscard]] friend bool operator<(const RunKey& acLeft, const RunKey& acRight) noexcept
    {
        return std::tie(acLeft.Ticks, acLeft.Pid, acLeft.Sequence) < std::tie(acRight.Ticks, acRight.Pid, acRight.Sequence);
    }
};

namespace Detail
{
[[nodiscard]] inline bool ReadNumber(std::wstring_view& aText, uint64_t& aOut) noexcept
{
    size_t digits = 0;
    uint64_t value = 0;
    while (digits < aText.size() && aText[digits] >= L'0' && aText[digits] <= L'9')
    {
        if (digits == 19) // over uint64 range: not one of ours
            return false;
        value = value * 10 + static_cast<uint64_t>(aText[digits] - L'0');
        ++digits;
    }
    if (digits == 0)
        return false;
    aOut = value;
    aText.remove_prefix(digits);
    return true;
}
} // namespace Detail

/**
 * @return The run's key when the name is exactly `crash-run-<digits>-<digits>-<digits>`, else nothing (left alone).
 */
[[nodiscard]] inline std::optional<RunKey> ParseRunName(std::wstring_view aName) noexcept
{
    constexpr std::wstring_view cPrefix = L"crash-run-";
    if (aName.substr(0, cPrefix.size()) != cPrefix)
        return std::nullopt;
    aName.remove_prefix(cPrefix.size());
    RunKey key;
    if (!Detail::ReadNumber(aName, key.Ticks) || aName.empty() || aName.front() != L'-')
        return std::nullopt;
    aName.remove_prefix(1);
    if (!Detail::ReadNumber(aName, key.Pid) || aName.empty() || aName.front() != L'-')
        return std::nullopt;
    aName.remove_prefix(1);
    if (!Detail::ReadNumber(aName, key.Sequence) || !aName.empty())
        return std::nullopt;
    return key;
}

enum class DumpKind
{
    Other, // never removed
    Full,
    Small,
};

[[nodiscard]] inline bool EndsWithNoCase(const std::wstring_view aText, const std::wstring_view aEnd) noexcept
{
    if (aText.size() < aEnd.size())
        return false;
    const auto cTail = aText.substr(aText.size() - aEnd.size());
    return std::equal(
        cTail.begin(), cTail.end(), aEnd.begin(),
        [](wchar_t a, wchar_t b)
        {
            const auto lower = [](wchar_t c)
            {
                return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            };
            return lower(a) == lower(b);
        });
}

/**
 * @return Full for `crash_*.dmp`, Small for any `*.small.dmp` (crash and freeze), Other for everything else.
 */
[[nodiscard]] inline DumpKind KindOf(const std::wstring_view aFile) noexcept
{
    if (EndsWithNoCase(aFile, L".small.dmp"))
        return DumpKind::Small;
    if (aFile.substr(0, 6) == L"crash_" && EndsWithNoCase(aFile, L".dmp"))
        return DumpKind::Full;
    return DumpKind::Other;
}

struct Run
{
    RunKey Key;
    bool HasFull = false;
    bool HasSmall = false;
};

struct Verdict
{
    bool RemoveFull = false;
    bool RemoveSmall = false;
};

/**
 * @param acRuns The earlier runs found (the current launch's folder is not among them), in any order.
 * @return One verdict per run, in the order given.
 */
[[nodiscard]] inline std::vector<Verdict> Decide(const std::vector<Run>& acRuns)
{
    std::vector<size_t> order(acRuns.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&acRuns](size_t a, size_t b) { return acRuns[b].Key < acRuns[a].Key; }); // newest first

    std::vector<Verdict> verdicts(acRuns.size());
    bool fullKept = false;
    size_t smallKept = 0;
    for (const size_t cIndex : order)
    {
        const Run& cRun = acRuns[cIndex];
        if (cRun.HasFull)
        {
            verdicts[cIndex].RemoveFull = fullKept;
            fullKept = true;
        }
        if (cRun.HasSmall)
        {
            verdicts[cIndex].RemoveSmall = smallKept >= kSmallRunsKept;
            ++smallKept;
        }
    }
    return verdicts;
}
} // namespace CrashDumpPrune
