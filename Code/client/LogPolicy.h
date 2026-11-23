#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

//! Rules for what the client log keeps and how often it repeats itself.
namespace LogPolicy
{
struct Rotation
{
    size_t FileSize;
    size_t FileCount;
};

//! The client log's rotation for a SPDLOG_LEVEL value (null when unset): a debug or trace log gets 20 MB times 8, so that
//! a session's history is not rotated away within a minute; otherwise 5 MB times 3.
inline Rotation RotationFor(const char* acpLevels) noexcept
{
    if (acpLevels)
    {
        std::string levels(acpLevels);
        for (auto& c : levels)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        // "debug", "trace", or a list such as "info,tp=debug": a logger's own level counts too.
        if (levels.find("debug") != std::string::npos || levels.find("trace") != std::string::npos)
            return {static_cast<size_t>(1048576) * 20, 8};
    }
    return {static_cast<size_t>(1048576) * 5, 3};
}

//! A repeating line at most once every aIntervalMs for one key (an actor): the next report time is kept by the caller.
inline bool ReportDue(const int64_t aNowMs, int64_t& aNextMs, const int64_t aIntervalMs) noexcept
{
    if (aNowMs < aNextMs)
        return false;
    aNextMs = aNowMs + aIntervalMs;
    return true;
}

//! What the crash report's first line adds when the loader is already shutting the process down: a DLL's unload, not play.
inline const char* CrashWhen(const bool aLoaderShuttingDown) noexcept
{
    return aLoaderShuttingDown ? " at process exit (a DLL unloading while the game ends)" : "";
}
} // namespace LogPolicy
