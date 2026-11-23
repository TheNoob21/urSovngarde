#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// What the in-game co-op menu (the "Call to Sovngarde" lesser power) says and does, with no game in it.
// The Papyrus natives in Games/CoopNatives.cpp use these numbers.
namespace CoopMenu
{
// The numbers Papyrus gets from urSovngardeCoop.GetStatus(). They are a promise to the scripts and the esp.
enum class Status : int32_t
{
    kOffline = 0,
    kConnecting = 1,
    kOnline = 2,
    kRetrying = 3, // the line dropped or the server was unreachable, and a new attempt is scheduled
};

// Why a join did not start; the HUD says it in words (DescribeJoin).
enum class JoinResult : int32_t
{
    kStarted = 0,
    kAlreadyOnline = 1, // online or connecting already
    kNoSave = 2,        // no save is loaded, so there is no cell to read
    kNoServer = 3,      // neither the player's connect.txt nor a mod's names a server
    kNotReady = 4,      // the game is not running the client yet
};

inline Status FromInt(int32_t aValue)
{
    return aValue >= 0 && aValue <= 3 ? static_cast<Status>(aValue) : Status::kOffline;
}

// The one line "Show status" prints. The password is never in it.
inline std::string DescribeStatus(Status aStatus, const std::string& acAddress, bool aHasSave)
{
    const std::string where = acAddress.empty() ? std::string("the server") : acAddress;
    switch (aStatus)
    {
    case Status::kOnline: return "Connected to " + where;
    case Status::kConnecting: return "Connecting to " + where + "...";
    case Status::kRetrying: return "Not reachable, trying again: " + where;
    case Status::kOffline: break;
    }
    if (!aHasSave)
        return "Offline. Load a save first.";
    if (acAddress.empty())
        return "Offline. Add a server in the in-game co-op menu.";
    return "Offline. Saved server: " + acAddress;
}

struct OnlineStatus
{
    std::string Name;
    std::size_t Players{};
    int Ping{-1};
};

inline std::string DescribeOnline(const OnlineStatus& acStatus)
{
    const std::string server = acStatus.Name.empty() ? "Connected" : "Connected to " + acStatus.Name;
    const std::string players = acStatus.Players ? std::to_string(acStatus.Players) : "unavailable";
    const std::string ping = acStatus.Ping < 0 ? "unavailable" : std::to_string(acStatus.Ping) + " ms";
    return server + "\nPlayers: " + players + "\nPing: " + ping;
}

inline const char* DescribeJoin(JoinResult aResult)
{
    switch (aResult)
    {
    case JoinResult::kStarted: return "Connecting...";
    case JoinResult::kAlreadyOnline: return "Already connected.";
    case JoinResult::kNoSave: return "Load a save before joining.";
    case JoinResult::kNoServer: return "No server set. Add a server in the in-game co-op menu.";
    case JoinResult::kNotReady: return "The co-op client is not ready yet.";
    }
    return "Could not connect.";
}

// Whether the game's own connect-after-load (autoconnect=on in connect.txt) runs again: once per save loaded since it last ran.
// A disconnect by hand (F6, the menu) or a refused join waits for the next save, so a VR player with no keyboard rejoins by
// loading one. aDone: it ran this session; the counts: saves loaded so far, and as of that run.
inline bool RearmAutoConnect(bool aDone, uint32_t aSaveLoads, uint32_t aSaveLoadsAtRun)
{
    return aDone && aSaveLoads != aSaveLoadsAtRun;
}

} // namespace CoopMenu
