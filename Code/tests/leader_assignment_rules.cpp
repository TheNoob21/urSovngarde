#include <catch2/catch.hpp>

#include <LeaderAssignmentRules.h>

TEST_CASE("Only the party leader's game is asked about taking a living actor it registers", "[leader_assignment]")
{
    using LeaderAssignmentRules::WorthAsking;

    CHECK(WorthAsking(true, true, false));
    // Player 3 of rc7.1, in the party but not its leader: never asked, never logged.
    CHECK_FALSE(WorthAsking(true, false, false));
    CHECK_FALSE(WorthAsking(false, false, false));
    // A dead body changes hands only when it is grabbed.
    CHECK_FALSE(WorthAsking(true, true, true));
}
