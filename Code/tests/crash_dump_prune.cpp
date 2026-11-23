#include <catch2/catch.hpp>

#include <Helpers/CrashDumpDirectory.h>
#include <Helpers/CrashDumpPrune.h>

#include <filesystem>
#include <fstream>

using namespace CrashDumpPrune;

TEST_CASE("Only the crash-run folders this client makes are recognised", "[crash]")
{
    const auto cKey = ParseRunName(L"crash-run-133700000000000000-4242-0");
    REQUIRE(cKey);
    CHECK(cKey->Ticks == 133700000000000000ull);
    CHECK(cKey->Pid == 4242);
    CHECK(cKey->Sequence == 0);

    CHECK_FALSE(ParseRunName(L"crash-run-1-2"));
    CHECK_FALSE(ParseRunName(L"crash-run-1-2-3.old"));
    CHECK_FALSE(ParseRunName(L"crash-run-1-2-3-4"));
    CHECK_FALSE(ParseRunName(L"crash-run--2-3"));
    CHECK_FALSE(ParseRunName(L"crash-run-a-2-3"));
    CHECK_FALSE(ParseRunName(L"crash-run-99999999999999999999-2-3")); // over uint64
    CHECK_FALSE(ParseRunName(L"Crash-run-1-2-3"));
    CHECK_FALSE(ParseRunName(L"logs"));
}

TEST_CASE("Dump files are told apart by name; anything else is never removed", "[crash]")
{
    CHECK(KindOf(L"crash_UTC_2026-11-23_01-02-03.dmp") == DumpKind::Full);
    CHECK(KindOf(L"crash_UTC_2026-11-23_01-02-03.small.dmp") == DumpKind::Small);
    CHECK(KindOf(L"freeze.small.dmp") == DumpKind::Small);
    CHECK(KindOf(L"crash_UTC_x.DMP") == DumpKind::Full);
    CHECK(KindOf(L"tp_client.log") == DumpKind::Other);
    CHECK(KindOf(L"notes.dmp") == DumpKind::Other);
}

TEST_CASE("The newest full dump and the small dumps of the newest three runs are kept", "[crash]")
{
    // Given out of order; key ticks say which is newer.
    const std::vector<Run> cRuns{
        {{10, 1, 0}, true, true},   // 0: oldest
        {{50, 1, 0}, true, true},   // 1: newest with a full dump
        {{30, 1, 0}, false, true},  // 2
        {{40, 1, 0}, true, false},  // 3
        {{20, 1, 0}, false, true},  // 4
        {{60, 1, 0}, false, false}, // 5: newest, empty
    };
    const auto cVerdicts = Decide(cRuns);
    REQUIRE(cVerdicts.size() == cRuns.size());

    CHECK_FALSE(cVerdicts[1].RemoveFull);
    CHECK(cVerdicts[3].RemoveFull);
    CHECK(cVerdicts[0].RemoveFull);

    // Small dumps, newest first: run 1 (50), run 2 (30), run 4 (20) kept; run 0 (10) removed.
    CHECK_FALSE(cVerdicts[1].RemoveSmall);
    CHECK_FALSE(cVerdicts[2].RemoveSmall);
    CHECK_FALSE(cVerdicts[4].RemoveSmall);
    CHECK(cVerdicts[0].RemoveSmall);

    CHECK_FALSE(cVerdicts[5].RemoveFull);
    CHECK_FALSE(cVerdicts[5].RemoveSmall);
}

TEST_CASE("Two launches in the same tick are ordered by pid and sequence", "[crash]")
{
    const std::vector<Run> cRuns{{{5, 2, 0}, true, false}, {{5, 1, 7}, true, false}};
    const auto cVerdicts = Decide(cRuns);
    CHECK_FALSE(cVerdicts[0].RemoveFull);
    CHECK(cVerdicts[1].RemoveFull);
    CHECK(Decide({}).empty());
}

TEST_CASE("A launch removes the dumps of earlier launches it does not keep, and their empty folders", "[crash]")
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (L"tp-crashprune-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    REQUIRE(fs::create_directories(root / L"logs"));
    const auto logs = root / L"logs";
    const auto make = [&](const wchar_t* acRun, std::initializer_list<const wchar_t*> aFiles)
    {
        fs::create_directory(logs / acRun);
        for (const auto* pFile : aFiles)
            std::ofstream(logs / acRun / pFile) << "x";
    };
    // Old ticks (1601), so none counts as a game starting right now.
    make(L"crash-run-100-1-0", {L"crash_UTC_a.dmp", L"crash_UTC_a.small.dmp"});
    make(L"crash-run-200-1-0", {L"crash_UTC_b.small.dmp"});
    make(L"crash-run-300-1-0", {L"crash_UTC_c.small.dmp"});
    make(L"crash-run-400-1-0", {L"crash_UTC_d.dmp", L"crash_UTC_d.small.dmp", L"notes.txt"});
    make(L"crash-run-500-1-0", {});
    make(L"crash-run-x-1-0", {L"crash_UTC_e.dmp"}); // not ours: left alone
    std::ofstream(logs / L"tp_client.log") << "log";

    {
        CrashDumps::Directory run;
        REQUIRE(run.Initialize(root));

        CHECK_FALSE(fs::exists(logs / L"crash-run-100-1-0")); // full replaced by 400's, small beyond the newest three
        CHECK(fs::exists(logs / L"crash-run-200-1-0" / L"crash_UTC_b.small.dmp"));
        CHECK(fs::exists(logs / L"crash-run-300-1-0" / L"crash_UTC_c.small.dmp"));
        CHECK(fs::exists(logs / L"crash-run-400-1-0" / L"crash_UTC_d.dmp"));
        CHECK(fs::exists(logs / L"crash-run-400-1-0" / L"notes.txt"));
        CHECK_FALSE(fs::exists(logs / L"crash-run-500-1-0"));
        CHECK(fs::exists(logs / L"crash-run-x-1-0" / L"crash_UTC_e.dmp"));
        CHECK(fs::exists(logs / L"tp_client.log"));
        CHECK(fs::exists(run.Path())); // this launch's own folder
    }
    std::error_code error;
    fs::remove_all(root, error);
}
