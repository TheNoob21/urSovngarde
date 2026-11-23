#pragma once

#include <Services/CoopMenu.h>

#include <optional>
#include <string>
#include <vector>

// What the co-op menu does in the game: join, leave, say how it stands, and keep the player's connect.txt.
// Papyrus (Games/CoopNatives.cpp) and the stock connection UI call these.
// Every function may be called from any thread; the ones that touch the game hand the work to its thread.
namespace CoopActions
{
// Called once a frame on the game's thread: caches what the other threads may read.
void Update() noexcept;

// Queues the join. The HUD says why it did not start, like F6 does. False only when the client is not running yet.
bool Connect() noexcept;
struct JoinCredential
{
    std::string Address;
    std::string Password;
};
bool ConnectSaved(int32_t aId, std::optional<JoinCredential> aCredential = {}) noexcept;
bool ConnectLastUsed() noexcept;
// Queues the leave and stops retries.
bool Disconnect() noexcept;

CoopMenu::Status GetStatus() noexcept;
std::string GetStatusText();

struct ServerSummary
{
    int32_t Id{};
    std::string Name;
    std::string Address;
    bool HasPassword{};
};

bool GetSavedServers(std::vector<ServerSummary>& aServers);
int32_t GetLastUsedServer();
bool AddSavedServer(const std::string& acAddress);
uint32_t SaveSavedServer(int32_t aId, const std::string& acName, const std::string& acAddress, const std::string& acPassword, bool aRememberPassword);
bool ImportSavedServer();
bool RenameSavedServer(int32_t aId, const std::string& acName);
bool RemoveSavedServer(int32_t aId);

// The player's connect.txt first, then the mod's, like a join. False when neither names a server.
// aAutoConnect: -1 when the file has no autoconnect line, else 0 off or 1 on.
bool LoadSettings(std::string& aAddress, bool& aHasPassword, int& aAutoConnect, std::string* apPassword = nullptr);
std::string GetLastErrorText();

} // namespace CoopActions
