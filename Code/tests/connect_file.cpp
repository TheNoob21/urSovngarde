#include <catch2/catch.hpp>

#include <Services/ConnectFile.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using ConnectFile::Contents;

namespace
{
bool NamesServer(const std::string& acText)
{
    std::istringstream text(acText);
    Contents contents;
    return ConnectFile::Parse(text, contents);
}

Contents Parsed(const std::string& acText)
{
    std::istringstream text(acText);
    Contents contents;
    REQUIRE(ConnectFile::Parse(text, contents));
    return contents;
}

// Two connect files in a folder of their own, gone at the end of the test.
struct TwoFiles
{
    TwoFiles()
    {
        std::error_code error;
        std::filesystem::remove_all(Folder, error);
        std::filesystem::create_directories(Folder, error);
    }

    ~TwoFiles()
    {
        std::error_code error;
        std::filesystem::remove_all(Folder, error);
    }

    std::filesystem::path Folder = std::filesystem::temp_directory_path() / "connect_file_test";
    std::string Player = (Folder / "player.txt").string();
    std::string Mod = (Folder / "mod.txt").string();
};

void Write(const std::string& acPath, const std::string& acText)
{
    std::ofstream file(acPath, std::ios::binary);
    file << acText;
}
} // namespace

TEST_CASE("the first line is the server", "[connectfile]")
{
    CHECK(Parsed("203.0.113.10:12000\r\n").Address == "203.0.113.10:12000");
    CHECK(Parsed("play.example.org:9600").Address == "play.example.org:9600");
}

TEST_CASE("no port means the server's", "[connectfile]")
{
    CHECK(Parsed("play.example.org\r\n").Address == "play.example.org:10578");
    CHECK(Parsed("203.0.113.10").Address == "203.0.113.10:10578");
}

TEST_CASE("Notepad's byte order mark and stray spaces stay out of the address", "[connectfile]")
{
    CHECK(Parsed("\xEF\xBB\xBF  203.0.113.10:10578  \r\n").Address == "203.0.113.10:10578");
}

TEST_CASE("the second line is the password", "[connectfile]")
{
    const Contents contents = Parsed("203.0.113.10:10578\r\nsecret\r\n");
    CHECK(contents.Password == "secret");
    CHECK_FALSE(contents.AutoConnect);
    CHECK_FALSE(contents.AutoConnectSet);
}

TEST_CASE("a setting is never taken for a password", "[connectfile]")
{
    const Contents onSecondLine = Parsed("203.0.113.10:10578\r\nautoconnect=on\r\n");
    CHECK(onSecondLine.Password.empty());
    CHECK(onSecondLine.AutoConnect);
    CHECK(onSecondLine.AutoConnectSet);

    const Contents afterEmptyLine = Parsed("203.0.113.10:10578\r\n\r\nautoconnect=on\r\n");
    CHECK(afterEmptyLine.Password.empty());
    CHECK(afterEmptyLine.AutoConnect);

    const Contents afterPassword = Parsed("203.0.113.10:10578\r\nsecret\r\nautoconnect=on\r\n");
    CHECK(afterPassword.Password == "secret");
    CHECK(afterPassword.AutoConnect);

    CHECK_FALSE(Parsed("203.0.113.10:10578\r\nautoconnect=off\r\n").AutoConnect);
}

TEST_CASE("explicit standalone connection settings preserve case and unset distinctions", "[connectfile]")
{
    const auto absent = Parsed("server.example\nSecretPassword\n");
    CHECK_FALSE(absent.AutoConnectSet);
    CHECK(absent.Password == "SecretPassword");
    const auto enabled = Parsed("server.example\n\xEF\xBB\xBF AuToCoNnEcT=ON \r\n");
    CHECK(enabled.AutoConnectSet);
    CHECK(enabled.AutoConnect);
    CHECK(enabled.Password.empty());
    for (const auto* value : {"off", "OFF", "invalid"})
    {
        const auto disabled = Parsed(std::string("server.example\nAUTOCONNECT=") + value + "\nSecretPassword\n");
        CHECK(disabled.AutoConnectSet);
        CHECK_FALSE(disabled.AutoConnect);
        CHECK(disabled.Password == "SecretPassword");
    }
}

TEST_CASE("reusing connection contents preserves player-file priority", "[connectfile]")
{
    TwoFiles files;
    Write(files.Mod, "mod.example\nautoconnect=on\n");
    Contents contents;
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    REQUIRE(contents.FromMod);
    Write(files.Player, "player.example\nautoconnect=off\n");
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    CHECK_FALSE(contents.FromMod);
    CHECK(contents.AutoConnectSet);
    CHECK_FALSE(contents.AutoConnect);
    CHECK(contents.Address == "player.example:10578");
}

TEST_CASE("a file with nothing on its first line names no server", "[connectfile]")
{
    CHECK_FALSE(NamesServer(""));
    CHECK_FALSE(NamesServer("\r\n"));
    CHECK_FALSE(NamesServer("   \r\nsecret\r\n"));
    CHECK_FALSE(NamesServer("\xEF\xBB\xBF\r\n"));
}

TEST_CASE("the player's file wins over a mod's", "[connectfile]")
{
    TwoFiles files;
    Write(files.Player, "127.0.0.1:50000\r\n");
    Write(files.Mod, "play.example.org:10578\r\n");

    Contents contents;
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    CHECK(contents.Address == "127.0.0.1:50000");
    CHECK_FALSE(contents.FromMod);
}

TEST_CASE("the mod's file serves when the player has none", "[connectfile]")
{
    TwoFiles files;
    Write(files.Mod, "play.example.org:10578\r\nautoconnect=on\r\n");

    Contents contents;
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    CHECK(contents.Address == "play.example.org:10578");
    CHECK(contents.AutoConnect);
    CHECK(contents.FromMod);
}

TEST_CASE("the mod's file serves when the player's names no server", "[connectfile]")
{
    TwoFiles files;
    Write(files.Player, "\r\nsecret\r\nautoconnect=off\r\n");
    Write(files.Mod, "play.example.org:10578\r\nautoconnect=on\r\n");

    Contents contents;
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    CHECK(contents.Address == "play.example.org:10578");
    CHECK(contents.Password.empty());
    CHECK(contents.AutoConnect);
    CHECK(contents.FromMod);
}

TEST_CASE("the two files are never mixed", "[connectfile]")
{
    TwoFiles files;
    Write(files.Player, "127.0.0.1:50000\r\n");
    Write(files.Mod, "play.example.org:10578\r\nsecret\r\nautoconnect=on\r\n");

    Contents contents;
    REQUIRE(ConnectFile::Load(files.Player, files.Mod, contents));
    CHECK(contents.Password.empty());
    CHECK_FALSE(contents.AutoConnect);
}

TEST_CASE("no file names no server", "[connectfile]")
{
    TwoFiles files;
    Contents contents;
    CHECK_FALSE(ConnectFile::Load(files.Player, files.Mod, contents));
}

TEST_CASE("an unreadable file is kept aside as .bad, replacing an older one, so that a new one can start", "[connectfile]")
{
    TwoFiles files;
    const std::filesystem::path cServers = files.Folder / "servers.txt";
    const std::filesystem::path cBad = files.Folder / "servers.txt.bad";
    Write(cBad.string(), "an older bad file");
    // Cut short by a power loss: not a saved server list any more.
    Write(cServers.string(), "servers-v1\n4\n");

    std::error_code error;
    REQUIRE(ConnectFile::SetAside(cServers, error));
    CHECK_FALSE(error);
    CHECK_FALSE(std::filesystem::exists(cServers));
    REQUIRE(std::filesystem::exists(cBad));
    std::ifstream kept(cBad, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
    CHECK(text == "servers-v1\n4\n");

    // Nothing to set aside is an error, never a crash.
    CHECK_FALSE(ConnectFile::SetAside(files.Folder / "missing.txt", error));
    CHECK(error);
}
