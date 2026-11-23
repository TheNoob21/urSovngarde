#include <TiltedOnlinePCH.h>

#include <CoopActions.h>

#include <Services/ConnectFile.h>
#include <Services/SavedServers.h>
#include <Services/TransportService.h>
#include <Services/VRConnectService.h>

#include <PlayerCharacter.h>
#include <Utils.h>
#include <World.h>
#include <OverlayApp.hpp>

#include <atomic>
#include <chrono>
#include <mutex>

namespace
{
std::atomic<int32_t> s_status{static_cast<int32_t>(CoopMenu::Status::kOffline)};
std::atomic<bool> s_hasSave{false};
std::mutex s_statusLock;
CoopMenu::OnlineStatus s_online;
std::chrono::steady_clock::time_point s_nextStatusRefresh{};

struct SavedJoin
{
    SavedServers::Id Id;
    std::string Address;
};

std::optional<SavedJoin> s_savedJoin;

std::mutex s_errorLock;
std::string s_lastError;

// Empty when the folder cannot be named, which makes a save fail instead of writing to the root of a drive.
std::string PlayerFile()
{
    const char* cpLocalAppData = std::getenv("LOCALAPPDATA");
    if (!cpLocalAppData || !*cpLocalAppData)
        return {};
    return (ConnectFile::PlayerFolder() / "connect.txt").string();
}

// The working directory is the game's, so under Mod Organizer this path shows the mods.
constexpr const char* kModFile = "Data\\SKSE\\Plugins\\urSovngarde\\connect.txt";

bool Ready() noexcept
{
    return entt::locator<World>::has_value();
}

// Connecting reads the player's cell, which only exists once a save is loaded.
bool HasSave() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && pPlayer->parentCell && pPlayer->GetNiNode();
}

void SetError(std::string aText)
{
    std::scoped_lock lock(s_errorLock);
    s_lastError = std::move(aText);
}

bool SavedError(std::string aText)
{
    SetError(std::move(aText));
    return false;
}

std::filesystem::path ServersFile()
{
    if (PlayerFile().empty())
        return {};
    // Keep the existing shared list, including the flat client's servers saved before the per-game connect folders.
    return ConnectFile::PlayerFolder().parent_path() / "SkyrimTogetherVR" / "servers.txt";
}

enum class ServersRead
{
    kRead,       // read, or no file yet: an empty list
    kUnreadable, // opened, but not a saved server list (cut short by a power loss, or edited by hand)
    kNotOpened,  // there, but could not be opened
};

ServersRead ReadServers(const std::filesystem::path& acPath, SavedServers::List& aList)
{
    if (acPath.empty())
        return ServersRead::kNotOpened;
    std::ifstream file(acPath, std::ios::binary);
    if (file)
        return SavedServers::Parse(file, aList) ? ServersRead::kRead : ServersRead::kUnreadable;
    std::error_code error;
    return !std::filesystem::exists(acPath, error) && !error ? ServersRead::kRead : ServersRead::kNotOpened;
}

bool LoadServers(const std::filesystem::path& acPath, SavedServers::List& aList)
{
    return ReadServers(acPath, aList) == ServersRead::kRead;
}

// SE and VR on the same PC can edit the same file. Hold the mutex across read, edit and replacement.
struct ServerFileMutex
{
    HANDLE Handle = CreateMutexW(nullptr, FALSE, L"Local\\urSovngarde.ConnectionFiles");
    bool Owned{};

    bool Lock() noexcept
    {
        if (!Handle)
            return false;
        const DWORD result = WaitForSingleObject(Handle, 500);
        Owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        return Owned;
    }

    ~ServerFileMutex()
    {
        if (Owned)
            ReleaseMutex(Handle);
        if (Handle)
            CloseHandle(Handle);
    }
};

template <class T> bool ChangeServers(T aChange)
{
    const auto path = ServersFile();
    if (path.empty())
        return SavedError("the LOCALAPPDATA folder is not set");
    ServerFileMutex mutex;
    if (!mutex.Lock())
        return SavedError("the saved server list is busy");
    SavedServers::List list;
    const ServersRead cRead = ReadServers(path, list);
    if (cRead == ServersRead::kNotOpened)
        return SavedError("could not read servers.txt");
    // Refusing to write over an unreadable list kept it stuck for good: a new list starts with this change, and the old file
    // is kept aside once the new one is ready to be written.
    const bool cStartAfresh = cRead == ServersRead::kUnreadable;
    if (cStartAfresh)
        list = {};
    if (const auto* error = aChange(list))
        return SavedError(error);
    const auto text = SavedServers::Serialize(list);
    if (!text)
        return SavedError("the saved server list is full");
    if (cStartAfresh)
    {
        std::error_code setAside;
        if (!ConnectFile::SetAside(path, setAside))
            return SavedError("could not read servers.txt");
        spdlog::warn("CoopActions: servers.txt could not be read; kept as servers.txt.bad, a new saved server list is started");
        if (Ready())
            World::Get().GetRunner().Queue([] { Utils::ShowModHudMessage("the saved server list could not be read; it was kept as servers.txt.bad and a new one started"); });
    }
    std::error_code error;
    ConnectFile::WriteAtomic(path, *text, error);
    if (error)
        return SavedError("could not write servers.txt");
    SetError("");
    return true;
}

void Say(const std::string& acText)
{
    spdlog::info("CoopActions: {}", acText);
    Utils::ShowModHudMessage(acText.c_str());
}

void ShowSavedMessage(const char* apText)
{
    Utils::ShowModHudMessage(apText);
    if (World::Get().ctx().at<VRConnectService>().GetStatus() == CoopMenu::Status::kOffline)
    {
        if (auto* overlay = World::Get().GetOverlayService().GetOverlayApp())
            overlay->ExecuteAsync("disconnect");
    }
}

CoopMenu::Status ReadStatus() noexcept
{
    return World::Get().ctx().at<VRConnectService>().GetStatus();
}

void JoinLegacy()
{
    const auto result = World::Get().ctx().at<VRConnectService>().Connect();
    if (result != CoopMenu::JoinResult::kStarted)
        Say(CoopMenu::DescribeJoin(result));
    if (result == CoopMenu::JoinResult::kStarted)
        s_savedJoin.reset();
}

bool QueueSavedJoin(int32_t aId, std::optional<CoopActions::JoinCredential> aCredential) noexcept
{
    if (!Ready())
        return false;

    World::Get().GetRunner().Queue(
        [aId, credential = std::move(aCredential)]
        {
            SavedServers::List list;
            if (!LoadServers(ServersFile(), list))
            {
                ShowSavedMessage("could not read the saved server list");
                return;
            }
            const auto* entry = aId == 0 ? SavedServers::LastUsed(list) : SavedServers::Find(list, static_cast<SavedServers::Id>(aId));
            if (!entry)
            {
                if (aId == 0)
                    JoinLegacy();
                else
                    ShowSavedMessage("that server is no longer saved");
                return;
            }

            if (credential && credential->Address != entry->Address)
            {
                ShowSavedMessage("the saved address changed; select it again");
                return;
            }
            auto selected = *entry;
            ConnectFile::AddDefaultPort(selected.Address);
            const auto result = World::Get().ctx().at<VRConnectService>().ConnectSaved(selected.Address, credential ? credential->Password : selected.Password.value_or(""));
            if (result != CoopMenu::JoinResult::kStarted)
            {
                ShowSavedMessage(CoopMenu::DescribeJoin(result));
                return;
            }

            s_savedJoin = SavedJoin{selected.IdValue, std::move(selected.Address)};
        });
    return true;
}
} // namespace

namespace CoopActions
{
void Update() noexcept
{
    if (!Ready())
        return;
    const auto status = ReadStatus();
    const auto previous = CoopMenu::FromInt(s_status.load(std::memory_order_relaxed));
    const auto now = std::chrono::steady_clock::now();
    auto& transport = World::Get().GetTransport();
    if (status != previous || (status == CoopMenu::Status::kOnline && now >= s_nextStatusRefresh))
    {
        std::scoped_lock lock(s_statusLock);
        s_online = {};
        if (status == CoopMenu::Status::kOnline)
        {
            s_online.Name = transport.GetServerName().c_str();
            s_online.Players = transport.GetServerPlayerCount();
            s_online.Ping = transport.GetConnectionStatus().m_nPing;
            s_nextStatusRefresh = now + std::chrono::milliseconds(500);
        }
        s_status.store(static_cast<int32_t>(status), std::memory_order_relaxed);
        if (auto* pApp = World::Get().GetOverlayService().GetOverlayApp())
        {
            auto arguments = CefListValue::Create();
            arguments->SetInt(0, static_cast<int32_t>(status));
            pApp->ExecuteAsync("connectionStatus", arguments);
        }
    }
    s_hasSave.store(HasSave(), std::memory_order_relaxed);

    if (s_savedJoin && status == CoopMenu::Status::kOnline)
    {
        const auto selected = std::move(*s_savedJoin);
        s_savedJoin.reset();
        if (transport.GetServerEndpoint() == selected.Address &&
            !ChangeServers([&](SavedServers::List& aList) -> const char* { return SavedServers::SetLastUsed(aList, selected.Id) ? nullptr : "that server is no longer saved"; }))
            spdlog::debug("CoopActions: could not remember the last used server");
    }
    else if (status == CoopMenu::Status::kOffline)
        s_savedJoin.reset();
}

bool Connect() noexcept
{
    if (!Ready())
        return false;

    World::Get().GetRunner().Queue(JoinLegacy);
    return true;
}

bool ConnectSaved(int32_t aId, std::optional<JoinCredential> aCredential) noexcept
{
    return aId > 0 && QueueSavedJoin(aId, std::move(aCredential));
}

bool ConnectLastUsed() noexcept
{
    return QueueSavedJoin(0, {});
}

bool Disconnect() noexcept
{
    if (!Ready())
        return false;

    World::Get().GetRunner().Queue(
        []
        {
            s_savedJoin.reset();
            if (!World::Get().ctx().at<VRConnectService>().Disconnect())
                Say("not connected");
        });
    return true;
}

CoopMenu::Status GetStatus() noexcept
{
    return CoopMenu::FromInt(s_status.load(std::memory_order_relaxed));
}

std::string GetStatusText()
{
    {
        std::scoped_lock lock(s_statusLock);
        if (GetStatus() == CoopMenu::Status::kOnline)
            return CoopMenu::DescribeOnline(s_online);
        if (GetStatus() == CoopMenu::Status::kConnecting)
            return "Connecting...";
        if (GetStatus() == CoopMenu::Status::kRetrying)
            return "Server not reachable. Trying again...";
    }

    std::string address;
    bool hasPassword = false;
    int autoConnect = -1;
    LoadSettings(address, hasPassword, autoConnect);
    return CoopMenu::DescribeStatus(CoopMenu::Status::kOffline, address, s_hasSave.load(std::memory_order_relaxed));
}

bool GetSavedServers(std::vector<ServerSummary>& aServers)
{
    SavedServers::List list;
    if (!LoadServers(ServersFile(), list))
        return false;
    aServers.clear();
    aServers.reserve(list.Entries.size());
    for (const auto& entry : list.Entries)
        aServers.push_back({static_cast<int32_t>(entry.IdValue), entry.Name, entry.Address, entry.Password.has_value()});
    return true;
}

int32_t GetLastUsedServer()
{
    SavedServers::List list;
    if (!LoadServers(ServersFile(), list))
        return 0;
    return static_cast<int32_t>(list.LastUsedId.value_or(0));
}

bool AddSavedServer(const std::string& acAddress)
{
    return ChangeServers(
        [&](SavedServers::List& aList) -> const char*
        {
            SavedServers::Id id{};
            return SavedServers::Add(aList, {0, acAddress, acAddress, {}}, id) ? nullptr : "check the address or free a server slot";
        });
}

uint32_t SaveSavedServer(int32_t aId, const std::string& acName, const std::string& acAddress, const std::string& acPassword, bool aRememberPassword)
{
    SavedServers::Id savedId{};
    const bool written = ChangeServers(
        [&](SavedServers::List& aList) -> const char*
        {
            SavedServers::Entry entry{0, acName, acAddress, {}};
            if (aRememberPassword)
                entry.Password = acPassword;
            savedId = SavedServers::Save(aList, static_cast<SavedServers::Id>(aId), std::move(entry));
            return savedId ? nullptr : "check the server fields or free a server slot";
        });
    return written ? savedId : 0;
}

bool ImportSavedServer()
{
    ConnectFile::Contents legacy;
    if (!ConnectFile::Load(PlayerFile(), kModFile, legacy))
        return SavedError("connect.txt does not name a server");
    return ChangeServers(
        [&](SavedServers::List& aList) -> const char*
        {
            SavedServers::Id id{};
            std::optional<std::string> password;
            if (!legacy.Password.empty())
                password = legacy.Password;
            return SavedServers::Add(aList, {0, legacy.Address, legacy.Address, std::move(password)}, id) ? nullptr : "check connect.txt or free a server slot";
        });
}

bool RenameSavedServer(int32_t aId, const std::string& acName)
{
    return ChangeServers([&](SavedServers::List& aList) -> const char* { return SavedServers::Rename(aList, aId, acName) ? nullptr : "enter a name for an existing server"; });
}

bool RemoveSavedServer(int32_t aId)
{
    return ChangeServers([&](SavedServers::List& aList) -> const char* { return SavedServers::Remove(aList, aId) ? nullptr : "that server is no longer saved"; });
}

bool LoadSettings(std::string& aAddress, bool& aHasPassword, int& aAutoConnect, std::string* apPassword)
{
    if (apPassword)
        apPassword->clear();

    ConnectFile::Contents contents;
    if (!ConnectFile::Load(PlayerFile(), kModFile, contents))
    {
        aAddress.clear();
        aHasPassword = false;
        aAutoConnect = -1;
        return false;
    }
    aAddress = contents.Address;
    if (apPassword)
        *apPassword = contents.Password;
    aHasPassword = !contents.Password.empty();
    aAutoConnect = contents.AutoConnectSet ? (contents.AutoConnect ? 1 : 0) : -1;
    return true;
}

std::string GetLastErrorText()
{
    std::scoped_lock lock(s_errorLock);
    return s_lastError;
}

} // namespace CoopActions
