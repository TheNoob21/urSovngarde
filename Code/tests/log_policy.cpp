#include <catch2/catch.hpp>

#include <LogPolicy.h>

TEST_CASE("The client log keeps more history at debug", "[log_policy]")
{
    using LogPolicy::RotationFor;

    CHECK(RotationFor(nullptr).FileSize == 5u * 1048576);
    CHECK(RotationFor(nullptr).FileCount == 3);
    CHECK(RotationFor("info").FileCount == 3);
    CHECK(RotationFor("").FileSize == 5u * 1048576);
    CHECK(RotationFor("debug").FileSize == 20u * 1048576);
    CHECK(RotationFor("debug").FileCount == 8);
    CHECK(RotationFor("DEBUG").FileCount == 8);
    CHECK(RotationFor("info,tp=debug").FileCount == 8);
    CHECK(RotationFor("trace").FileCount == 8);
}

TEST_CASE("A repeating line is due once per interval", "[log_policy]")
{
    using LogPolicy::ReportDue;

    int64_t next = 0;
    CHECK(ReportDue(1000, next, 30000));
    CHECK(next == 31000);
    CHECK_FALSE(ReportDue(30999, next, 30000));
    CHECK(ReportDue(31000, next, 30000));
    CHECK(next == 61000);
}

TEST_CASE("A crash while the loader shuts the process down is marked", "[log_policy]")
{
    using LogPolicy::CrashWhen;

    CHECK(std::string(CrashWhen(false)).empty());
    CHECK(std::string(CrashWhen(true)).find("at process exit") != std::string::npos);
}
