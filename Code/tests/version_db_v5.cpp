#include <catch2/catch.hpp>

#include "../client/VersionDbV5.h"
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <filesystem>
#include <TiltedCore/Stl.hpp>
#include <spdlog/spdlog.h>
#include <VersionDb.h>
#include <fstream>
#endif

namespace
{
template <class T> void Put(std::string& aBytes, const T& acValue)
{
    aBytes.append(reinterpret_cast<const char*>(&acValue), sizeof(acValue));
}
std::string Fixture()
{
    std::string bytes;
    for (std::uint32_t value : {5u, 1u, 7u, 104u, 0u})
        Put(bytes, value);
    char name[64] = "SkyrimSE.exe";
    bytes.append(name, sizeof(name));
    for (std::uint32_t value : {8u, 0u, 5u, 0u, 0x1234u, 0u, 0x9876u, 0u})
        Put(bytes, value);
    return bytes;
}
bool Parse(const std::string& acBytes, VersionDbV5::AddressMap& aMap, VersionDbV5::AddressMap& aReverse, std::string& aModule, std::array<int, 4> aVersion = {1, 7, 104, 0})
{
    std::istringstream stream(acBytes, std::ios::binary);
    return VersionDbV5::Load(stream, aVersion, aModule, aMap, aReverse);
}
void Set32(std::string& aBytes, std::size_t aPos, std::uint32_t aValue)
{
    std::memcpy(&aBytes[aPos], &aValue, sizeof(aValue));
}
} // namespace
TEST_CASE("Address Library format 5 reads sparse IDs and rejects invalid data", "[version_db_v5]")
{
    VersionDbV5::AddressMap map, reverse;
    std::string module;
    const auto good = Fixture();
    REQUIRE(good.size() == 0x60 + 5 * 4);
    REQUIRE(Parse(good, map, reverse, module));
    REQUIRE(module == "SkyrimSE.exe");
    REQUIRE(map.size() == 2);
    REQUIRE(map.at(1) == 0x1234);
    REQUIRE(map.at(3) == 0x9876);
    REQUIRE(reverse.at(0x1234) == 1);
    REQUIRE(reverse.at(0x9876) == 3);
    const auto oldMap = map, oldReverse = reverse;
    const auto oldModule = module;
    for (std::size_t size = 0; size < good.size(); ++size)
    {
        INFO("truncated size: " << size);
        REQUIRE_FALSE(Parse(good.substr(0, size), map, reverse, module));
        REQUIRE(map == oldMap);
        REQUIRE(reverse == oldReverse);
        REQUIRE(module == oldModule);
    }
    for (const auto mutation : {std::pair<std::size_t, std::uint32_t>{0, 2}, {0x54, 4}, {0x58, 1}, {0x5c, 0}, {0x5c, 0xffffffff}})
    {
        INFO("header offset: " << mutation.first << ", value: " << mutation.second);
        auto bad = good;
        Set32(bad, mutation.first, mutation.second);
        REQUIRE_FALSE(Parse(bad, map, reverse, module));
    }
    REQUIRE_FALSE(Parse(good, map, reverse, module, {1, 7, 99, 0}));
    auto badName = good;
    std::memset(&badName[0x14], 'x', 64);
    REQUIRE_FALSE(Parse(badName, map, reverse, module));
    badName[0x14] = '\0';
    REQUIRE_FALSE(Parse(badName, map, reverse, module));
}

#ifdef _WIN32
TEST_CASE("VersionDb checks the file header before selecting a decoder", "[version_db_v5]")
{
    struct FixtureFile
    {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("ursovngarde-v5-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::path file = root / "Data/SKSE/Plugins/versionlib-1-7-104-0.bin";
        FixtureFile()
        {
            REQUIRE_FALSE(std::filesystem::exists(root));
            std::filesystem::create_directories(file.parent_path());
        }
        ~FixtureFile()
        {
            std::error_code error;
            std::filesystem::remove(file, error);
            for (auto path = file.parent_path(); path != root.parent_path(); path = path.parent_path())
                std::filesystem::remove(path, error);
        }
        void Write(const std::string& bytes)
        {
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            REQUIRE(output.write(bytes.data(), bytes.size()));
        }
    } fixture;
    VersionDb database;
    const auto good = Fixture();
    fixture.Write(good);
    REQUIRE(database.Load(fixture.root, 1, 7, 104, 0));
    REQUIRE(database.GetOffsetMap().at(1) == 0x1234);
    REQUIRE(database.GetLoadedVersionString() == "1.7.104.0");
    for (std::size_t size = 0; size < good.size(); ++size)
    {
        INFO("entry-point truncated size: " << size);
        fixture.Write(good.substr(0, size));
        REQUIRE_FALSE(database.Load(fixture.root, 1, 7, 104, 0));
        REQUIRE(database.GetOffsetMap().empty());
        REQUIRE(database.GetModuleName().empty());
        REQUIRE(database.FindAddressById(1) == nullptr);
    }
}
#endif
