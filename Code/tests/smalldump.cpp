// The small crash dump (client/Helpers/SmallDump.h), written of this test process: a real minidump, small enough for a
// report, with run-local retention.

#include <catch2/catch.hpp>

#include <Helpers/SmallDump.h>
#include <Helpers/CrashDumpDirectory.h>

#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
struct TempTree
{
    std::filesystem::path Path;
    ~TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(Path, ec);
    }
};
} // namespace

TEST_CASE("A small crash dump is a real minidump and stays small", "[smalldump]")
{
    const auto root = std::filesystem::temp_directory_path() / (L"tp-smalldump-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    REQUIRE(std::filesystem::create_directory(root));
    TempTree cleanup{root};
    std::filesystem::create_directories(root / L"logs");
    CrashDumps::Directory run;
    REQUIRE(run.Initialize(root));
    const auto folder = run.Path();
    auto write = [&](const std::filesystem::path& path, DWORD& error, void (*restart)() noexcept = nullptr, uint64_t target = SmallDump::cTarget, DWORD extra = 0) -> uint64_t
    {
        HANDLE file = run.CreateFileForRun(path.filename());
        REQUIRE(file != INVALID_HANDLE_VALUE);
        const auto result = SmallDump::Write(file, nullptr, nullptr, error, restart, target, extra);
        CloseHandle(file);
        return result;
    };

    DWORD error = 0;
    const auto path = folder / L"crash_UTC_2026-10-07_08-00-00.small.dmp";
    const uint64_t size = write(path, error);
    INFO("error " << error);
    REQUIRE(size > 0);
    // The launcher takes small dumps under 4 MB.
    CHECK(size < 4ull * 1024 * 1024);

    char signature[4]{};
    std::ifstream(path, std::ios::binary).read(signature, 4);
    CHECK(std::string(signature, 4) == "MDMP");

    SECTION("the freeze dump's handle table keeps it a small minidump")
    {
        const auto withHandles = folder / L"freeze.small.dmp";
        const uint64_t handleSize = write(withHandles, error, nullptr, UINT64_MAX, MiniDumpWithHandleData);
        INFO("error " << error);
        REQUIRE(handleSize > 0);
        CHECK(handleSize < 4ull * 1024 * 1024);
        CHECK(handleSize > size);
    }

    SECTION("too big a dump is written again with only the crashing thread's stack")
    {
        // A few threads waiting, each with a stack of its own, as the game has a hundred.
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&stop] {
                volatile char stack[64 * 1024];
                for (auto& c : stack)
                    c = 1;
                while (!stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto full = folder / L"full.small.dmp";
        const uint64_t fullSize = write(full, error);
        static int s_restarts = 0;
        s_restarts = 0;
        const auto smaller = folder / L"smaller.small.dmp";
        // A target of one byte: always the second pass.
        const uint64_t smallerSize = write(smaller, error, []() noexcept { ++s_restarts; }, 1);
        stop = true;
        for (auto& t : threads)
            t.join();

        INFO("no extra threads " << size << ", with eight " << fullSize << ", second pass " << smallerSize);
        CHECK(s_restarts == 2);
        REQUIRE(smallerSize > 0);
        // Eight 64 KB stacks left out (each at least 48 KB of it in the full dump).
        CHECK(smallerSize + 8 * 48 * 1024 < fullSize);
        char head[4]{};
        std::ifstream(smaller, std::ios::binary).read(head, 4);
        CHECK(std::string(head, 4) == "MDMP");
    }
}

TEST_CASE("Crash dump directories isolate runs and retain only their own handles", "[smalldump]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (L"tp-dump-directory-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    REQUIRE(std::filesystem::create_directory(root));
    TempTree cleanup{root};
    const auto privateRoot = root / L"private";
    const auto ownerRoot = root / L"owner";
    fs::create_directories(privateRoot / L"logs");
    fs::create_directories(ownerRoot / L"logs");
    const auto oldName = L"crash_UTC_2026-10-01_08-00-00.small.dmp";
    std::ofstream(privateRoot / L"logs" / oldName) << "private prior";
    std::ofstream(ownerRoot / L"logs" / oldName) << "owner prior";
    auto contents = [](const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };

    CrashDumps::Directory uninitialized;
    CHECK(uninitialized.CreateFileForRun(L"x.dmp") == INVALID_HANDLE_VALUE);
    CHECK_FALSE(uninitialized.Initialize(root / L"missing"));
    CHECK(uninitialized.CreateFileForRun(L"x.dmp") == INVALID_HANDLE_VALUE);

    CrashDumps::Directory run;
    CrashDumps::Directory second;
    const auto firstLaunch = root / L"first-launch";
    REQUIRE(fs::create_directory(firstLaunch));
    CrashDumps::Directory fresh;
    REQUIRE(fresh.Initialize(firstLaunch));
    CHECK(fresh.Path().parent_path() == firstLaunch / L"logs");
    REQUIRE(run.Initialize(privateRoot));
    REQUIRE(second.Initialize(privateRoot));
    CHECK(run.Path() != second.Path());
    CHECK(run.Path().parent_path() == privateRoot / L"logs");
    const auto foreign = run.Path() / L"crash_UTC_2026-10-02_08-00-00.small.dmp";
    std::ofstream(foreign) << "introduced after setup";
    CHECK(run.CreateFileForRun(foreign.filename()) == INVALID_HANDLE_VALUE);
    // A failed write must neither close a caller-owned handle nor mutate an existing file.
    HANDLE readOnly = CreateFileW(foreign.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(readOnly != INVALID_HANDLE_VALUE);
    DWORD error = 0;
    CHECK(SmallDump::Write(readOnly, nullptr, nullptr, error) == 0);
    CHECK(error != 0);
    DWORD flags = 0;
    CHECK(GetHandleInformation(readOnly, &flags));
    CloseHandle(readOnly);
    CHECK(contents(foreign) == "introduced after setup");

    // Developer mode permits an unprivileged directory symlink on supported Windows versions.
    const auto link = root / L"linked-private";
    if (CreateSymbolicLinkW(link.c_str(), privateRoot.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2))
    {
        CrashDumps::Directory linked;
        CHECK_FALSE(linked.Initialize(link));
    }
    for (const auto& invalid :
         {fs::path{}, fs::path(L"."), fs::path(L".."), fs::path(L"..\\escape.dmp"), fs::path(L"sub\\file.dmp"), root / L"absolute.dmp", fs::path(L"file.dmp:stream")})
        CHECK(run.CreateFileForRun(invalid) == INVALID_HANDLE_VALUE);

    {
        CrashDumps::History<3> history;
        for (int i = 0; i != 4; ++i)
        {
            auto name = L"own-" + std::to_wstring(i) + L".dmp";
            HANDLE file = run.CreateFileForRun(name);
            REQUIRE(file != INVALID_HANDLE_VALUE);
            const std::string data = "own " + std::to_string(i);
            DWORD written = 0;
            REQUIRE(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
            REQUIRE(written == data.size());
            history.Keep(file);
        }
        CHECK_FALSE(fs::exists(run.Path() / L"own-0.dmp"));
    }
    for (int i = 1; i != 4; ++i)
        CHECK(contents(run.Path() / (L"own-" + std::to_wstring(i) + L".dmp")) == "own " + std::to_string(i));
    CHECK(contents(foreign) == "introduced after setup");
    CHECK(contents(privateRoot / L"logs" / oldName) == "private prior");
    CHECK(contents(ownerRoot / L"logs" / oldName) == "owner prior");
    CHECK_FALSE(fs::exists(root / L"escape.dmp"));
}
