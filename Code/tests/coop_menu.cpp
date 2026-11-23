#include <catch2/catch.hpp>

#include <Services/ConnectFile.h>
#include <Services/CoopMenu.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using ConnectFile::SettingsError;

namespace
{
SettingsError Check(std::string aAddress, std::string aPassword)
{
    return ConnectFile::Normalize(aAddress, aPassword);
}

} // namespace

TEST_CASE("an address and password are trimmed before they are saved", "[coopmenu]")
{
    std::string address = "\xEF\xBB\xBF  play.example.org:10578 \r\n";
    std::string password = "  secret ";
    REQUIRE(ConnectFile::Normalize(address, password) == SettingsError::kNone);
    CHECK(address == "play.example.org:10578");
    CHECK(password == "secret");
}

TEST_CASE("an address without a port is accepted, as the parser adds the default", "[coopmenu]")
{
    CHECK(Check("play.example.org", "") == SettingsError::kNone);
    CHECK(Check("203.0.113.10", "") == SettingsError::kNone);
}

TEST_CASE("addresses the parser or the network could not use are refused", "[coopmenu]")
{
    CHECK(Check("", "") == SettingsError::kEmptyAddress);
    CHECK(Check("   \r\n", "") == SettingsError::kEmptyAddress);
    CHECK(Check("play example.org", "") == SettingsError::kBadAddress);
    CHECK(Check("play.example.org\n:10578", "") == SettingsError::kBadAddress);
    CHECK(Check("play.example.org:", "") == SettingsError::kBadPort);
    CHECK(Check(":10578", "") == SettingsError::kBadPort);
    CHECK(Check("play.example.org:0", "") == SettingsError::kBadPort);
    CHECK(Check("play.example.org:65536", "") == SettingsError::kBadPort);
    CHECK(Check("play.example.org:99999999", "") == SettingsError::kBadPort);
    CHECK(Check("play.example.org:12ab", "") == SettingsError::kBadPort);
    CHECK(Check("play.example.org:65535", "") == SettingsError::kNone);
    CHECK(Check("play.example.org:1", "") == SettingsError::kNone);
}

TEST_CASE("a password the file could not hold is refused", "[coopmenu]")
{
    CHECK(Check("a.example", "two\nlines") == SettingsError::kBadPassword);
    CHECK(Check("a.example", "tab\there") == SettingsError::kBadPassword);
    // The parser would read this line as a setting, not as the password.
    CHECK(Check("a.example", "autoconnect=on") == SettingsError::kBadPassword);
    CHECK(Check("a.example", "AutoConnect=whatever") == SettingsError::kBadPassword);
    CHECK(Check("a.example", "my autoconnect=on") == SettingsError::kNone);
    CHECK(Check("a.example", "pass=word with spaces") == SettingsError::kNone);
}

TEST_CASE("status numbers are the ones the scripts expect", "[coopmenu]")
{
    using CoopMenu::Status;
    CHECK(static_cast<int>(Status::kOffline) == 0);
    CHECK(static_cast<int>(Status::kConnecting) == 1);
    CHECK(static_cast<int>(Status::kOnline) == 2);
    CHECK(static_cast<int>(Status::kRetrying) == 3);
    CHECK(CoopMenu::FromInt(2) == Status::kOnline);
    CHECK(CoopMenu::FromInt(-1) == Status::kOffline);
    CHECK(CoopMenu::FromInt(99) == Status::kOffline);
}

TEST_CASE("the status line names the server and never the password", "[coopmenu]")
{
    using CoopMenu::DescribeStatus;
    using CoopMenu::Status;
    CHECK(DescribeStatus(Status::kOnline, "a.example:1", true) == "Connected to a.example:1");
    CHECK(DescribeStatus(Status::kConnecting, "a.example:1", true) == "Connecting to a.example:1...");
    CHECK(DescribeStatus(Status::kRetrying, "a.example:1", true).find("a.example:1") != std::string::npos);
    CHECK(DescribeStatus(Status::kOffline, "a.example:1", true) == "Offline. Saved server: a.example:1");
    CHECK(DescribeStatus(Status::kOffline, "a.example:1", false) == "Offline. Load a save first.");
    CHECK(DescribeStatus(Status::kOffline, "", true) == "Offline. Add a server in the in-game co-op menu.");
    CHECK(std::string(CoopMenu::DescribeJoin(CoopMenu::JoinResult::kNoServer)) == "No server set. Add a server in the in-game co-op menu.");
    CHECK(DescribeStatus(Status::kOnline, "", true) == "Connected to the server");
}

TEST_CASE("every join result has a message", "[coopmenu]")
{
    using CoopMenu::JoinResult;
    for (const auto result : {JoinResult::kStarted, JoinResult::kAlreadyOnline, JoinResult::kNoSave, JoinResult::kNoServer, JoinResult::kNotReady})
        CHECK(std::string(CoopMenu::DescribeJoin(result)).size() > 5);
}

TEST_CASE("a bare IPv6 address is refused, a bracketed one is not", "[coopmenu]")
{
    CHECK(Check("2001:db8::1", "") == SettingsError::kBadAddress);
    CHECK(Check("a:b:c", "") == SettingsError::kBadAddress);
    CHECK(Check("[2001:db8::1]:10578", "") == SettingsError::kNone);
    CHECK(Check("[::1]:80", "") == SettingsError::kNone);
}

TEST_CASE("A save loaded after a disconnect by hand runs the connect-after-load again", "[coopmenu]")
{
    using CoopMenu::RearmAutoConnect;

    // Not run yet this session: the first load runs it anyway.
    CHECK_FALSE(RearmAutoConnect(false, 1, 0));
    // Ran, or connected, at the 1st save; F6 then disconnected with no save loaded since.
    CHECK_FALSE(RearmAutoConnect(true, 1, 1));
    // A 2nd save loaded since: it runs again.
    CHECK(RearmAutoConnect(true, 2, 1));
}

TEST_CASE("online co-op status shows server name, population and latency", "[coopmenu]")
{
    CHECK(CoopMenu::DescribeOnline({"Riverwood", 2, 37}) == "Connected to Riverwood\nPlayers: 2\nPing: 37 ms");
    CHECK(CoopMenu::DescribeOnline({}) == "Connected\nPlayers: unavailable\nPing: unavailable");
}

TEST_CASE("the stock saved-server writer creates folders and replaces a complete file", "[coopmenu]")
{
    const auto folder = std::filesystem::temp_directory_path() / "coop_menu_stock_writer_test";
    const auto target = folder / "SkyrimTogetherVR" / "servers.txt";
    std::filesystem::remove_all(folder);
    std::error_code io;
    ConnectFile::WriteAtomic(target, "first complete list\n", io);
    REQUIRE_FALSE(io);
    ConnectFile::WriteAtomic(target, "second complete list\n", io);
    REQUIRE_FALSE(io);
    std::ifstream file(target);
    std::stringstream text;
    text << file.rdbuf();
    CHECK(text.str() == "second complete list\n");
    file.close();
    for (const auto& entry : std::filesystem::directory_iterator(target.parent_path()))
        CHECK(entry.path().extension() != ".tmp");
    std::filesystem::remove_all(folder);
}

TEST_CASE("the stock saved-server writer refuses an empty destination", "[coopmenu]")
{
    std::error_code io;
    ConnectFile::WriteAtomic({}, "complete list\n", io);
    CHECK(io == std::make_error_code(std::errc::invalid_argument));
}
