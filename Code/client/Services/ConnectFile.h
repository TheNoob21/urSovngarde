#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// The file that names the server: its address, then the password, then settings as "name=value" lines.
// A player has their own, and a mod can ship the same file, which is how a mod list comes with its server.
namespace ConnectFile
{
// The player's own folder in %LOCALAPPDATA%, for this file and the launcher's files.
// One per game, so a flat and a VR game on one PC never read each other's.
#ifdef SKYRIMVR
constexpr const char* kFolderName = "SkyrimTogetherVR";
#else
constexpr const char* kFolderName = "SkyrimTogetherSE";
#endif

// The server's port when an address leaves it out.
constexpr uint16_t kDefaultPort = 10578;

inline std::filesystem::path PlayerFolder()
{
    const char* cpLocalAppData = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(cpLocalAppData ? cpLocalAppData : "") / kFolderName;
}

struct Contents
{
    std::string Address;
    std::string Password;
    // The launcher's "connect automatically" setting: an "autoconnect=on" line after the address.
    bool AutoConnect = false;
    // Explicit off must also override the keyboard-free standalone default.
    bool AutoConnectSet = false;
    // This came from a mod's file, as the player's own names no server.
    bool FromMod = false;
};

// Spaces and Notepad's UTF-8 byte order mark make the address unresolvable.
inline void Trim(std::string& aText)
{
    if (aText.rfind("\xEF\xBB\xBF", 0) == 0)
        aText.erase(0, 3);

    const auto first = aText.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        aText.clear();
        return;
    }
    aText.erase(aText.find_last_not_of(" \t\r\n") + 1);
    aText.erase(0, first);
}

inline void AddDefaultPort(std::string& aAddress)
{
    if (aAddress.find(':') == std::string::npos)
        aAddress += ":" + std::to_string(kDefaultPort);
}

// False when the text names no server.
inline bool Parse(std::istream& aText, Contents& aContents)
{
    std::string address;
    if (!std::getline(aText, address))
        return false;

    Trim(address);
    if (address.empty())
        return false;

    // A hand-made file may leave the port out, and the network library's own default is not the server's.
    AddDefaultPort(address);

    // After the address: the password, then settings as "name=value" lines.
    // A file without a password may have the setting on the second line, so a setting is never taken for a password.
    std::string password;
    bool autoConnect = false;
    bool autoConnectSet = false;
    bool passwordRead = false;
    for (std::string line; std::getline(aText, line);)
    {
        Trim(line);
        std::string setting = line;
        std::transform(setting.begin(), setting.end(), setting.begin(), [](unsigned char aChar) { return static_cast<char>(std::tolower(aChar)); });
        if (setting.rfind("autoconnect=", 0) == 0)
        {
            autoConnect = setting == "autoconnect=on";
            autoConnectSet = true;
        }
        else if (!passwordRead)
        {
            password = line;
            passwordRead = true;
        }
    }

    aContents.Address = address;
    aContents.Password = password;
    aContents.AutoConnect = autoConnect;
    aContents.AutoConnectSet = autoConnectSet;
    return true;
}

// Validation shared by the saved-server list.
enum class SettingsError
{
    kNone,
    kEmptyAddress,
    kBadAddress,  // spaces or control characters inside
    kBadPort,     // not a number from 1 to 65535, or no host before it
    kBadPassword, // a line break or control character, or text the parser would take for a setting
};

inline bool IsControl(unsigned char aChar)
{
    return aChar < 0x20 || aChar == 0x7F;
}

// Trims both and checks that the connection parser can read the endpoint and password.
inline SettingsError Normalize(std::string& aAddress, std::string& aPassword)
{
    Trim(aAddress);
    Trim(aPassword);

    if (aAddress.empty())
        return SettingsError::kEmptyAddress;
    for (const unsigned char c : aAddress)
        if (c == ' ' || IsControl(c))
            return SettingsError::kBadAddress;

    // The parser takes the first ':' for a port, so a bare IPv6 address would be saved with a garbage port.
    // Brackets name one: [::1]:10578.
    if (std::count(aAddress.begin(), aAddress.end(), ':') > 1 && aAddress.front() != '[')
        return SettingsError::kBadAddress;

    const auto colon = aAddress.rfind(':');
    if (colon != std::string::npos)
    {
        const std::string port = aAddress.substr(colon + 1);
        if (colon == 0 || port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return SettingsError::kBadPort;
        const int number = std::stoi(port);
        if (number < 1 || number > 65535)
            return SettingsError::kBadPort;
    }

    for (const unsigned char c : aPassword)
        if (IsControl(c))
            return SettingsError::kBadPassword;
    std::string lowered = aPassword;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.rfind("autoconnect=", 0) == 0)
        return SettingsError::kBadPassword;

    return SettingsError::kNone;
}

// A file that can no longer be read is kept beside it as "<name>.bad" (an older one is replaced), so that a new one can be started
// in its place. Nothing is deleted: the player can still look at it or put it back.
inline bool SetAside(const std::filesystem::path& acFile, std::error_code& aIoError)
{
    aIoError.clear();
    auto bad = acFile;
    bad += ".bad";
    std::error_code ignored;
    std::filesystem::remove(bad, ignored);
    std::filesystem::rename(acFile, bad, aIoError);
    return !aIoError;
}

// Replace the target only after the complete text has been written.
inline void WriteAtomic(const std::filesystem::path& acTarget, const std::string& acText, std::error_code& aIoError)
{
    aIoError.clear();
    if (acTarget.empty())
    {
        aIoError = std::make_error_code(std::errc::invalid_argument);
        return;
    }
    if (acTarget.has_parent_path())
    {
        std::filesystem::create_directories(acTarget.parent_path(), aIoError);
        if (aIoError)
            return;
    }

    static std::atomic<unsigned> s_saveCounter{0};
#ifdef _WIN32
    const auto processId = _getpid();
#else
    const auto processId = getpid();
#endif
    auto temporary = acTarget;
    temporary += "." + std::to_string(processId) + "." + std::to_string(++s_saveCounter) + ".tmp";
    std::error_code ignored;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << acText;
        file.close();
        if (!file)
        {
            aIoError = std::make_error_code(std::errc::io_error);
            std::filesystem::remove(temporary, ignored);
            return;
        }
    }

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        std::filesystem::rename(temporary, acTarget, aIoError);
        if (!aIoError)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::filesystem::remove(temporary, ignored);
}

// The player's file comes first whenever it names a server.
// Every join from the launcher writes it, a friend's code as a local tunnel, and a mod's file must not send that join elsewhere.
inline bool Load(const std::filesystem::path& acPlayerFile, const std::filesystem::path& acModFile, Contents& aContents)
{
    std::ifstream playerFile(acPlayerFile);
    if (playerFile && Parse(playerFile, aContents))
    {
        aContents.FromMod = false;
        return true;
    }

#ifndef SKYRIMVR
    // Older flat clients shared VR's file. A new flat file takes precedence without changing that original.
    if (acPlayerFile == PlayerFolder() / "connect.txt")
    {
        std::ifstream legacy(PlayerFolder().parent_path() / "SkyrimTogetherVR" / "connect.txt");
        if (legacy && Parse(legacy, aContents))
        {
            aContents.FromMod = false;
            return true;
        }
    }
#endif

    std::ifstream modFile(acModFile);
    if (!modFile || !Parse(modFile, aContents))
        return false;
    aContents.FromMod = true;
    return true;
}

} // namespace ConnectFile
