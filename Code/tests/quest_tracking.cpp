#include <catch2/catch.hpp>

#include <Systems/QuestTracking.h>

using namespace QuestTracking;

namespace
{
using Ids = std::vector<uint32_t>;

const auto cAllRun = [](uint32_t)
{
    return true;
};
} // namespace

TEST_CASE("a member's journal follows the quests its party leader follows", "[quests]")
{
    SECTION("the first time, only the leader's quests stay followed")
    {
        // The leader follows one quest; the member followed two others.
        const Changes cChanges = Follow(Ids{0x3372B}, Ids{}, Ids{0x46EF2, 0x2610C}, true, cAllRun);
        CHECK(cChanges.Track == Ids{0x3372B});
        CHECK(cChanges.Untrack == Ids{0x46EF2, 0x2610C});
        CHECK(cChanges.Followed == Ids{0x3372B});
    }

    SECTION("after that, a quest the member picked itself stays until the leader changes its own")
    {
        const Changes cSame = Follow(Ids{0x3372B}, Ids{0x3372B}, Ids{0x46EF2}, false, cAllRun);
        CHECK(cSame.Track.empty());
        CHECK(cSame.Untrack.empty());
        CHECK(cSame.Followed == Ids{0x3372B});
    }

    SECTION("a quest the leader stops following is no longer followed here, and its new one is")
    {
        const Changes cChanges = Follow(Ids{0x2610C}, Ids{0x3372B}, Ids{}, false, cAllRun);
        CHECK(cChanges.Track == Ids{0x2610C});
        CHECK(cChanges.Untrack == Ids{0x3372B});
        CHECK(cChanges.Followed == Ids{0x2610C});
    }

    SECTION("a quest that does not run here is followed once it does")
    {
        bool runs = false;
        const auto cRunsHere = [&runs](uint32_t aId)
        {
            return aId != 0x46EF2 || runs;
        };

        const Changes cBefore = Follow(Ids{0x46EF2, 0x3372B}, Ids{}, Ids{}, false, cRunsHere);
        CHECK(cBefore.Track == Ids{0x3372B});
        CHECK(cBefore.Followed == Ids{0x3372B});

        runs = true;
        const Changes cAfter = Follow(Ids{0x46EF2, 0x3372B}, cBefore.Followed, Ids{}, false, cRunsHere);
        CHECK(cAfter.Track == Ids{0x46EF2});
        CHECK(cAfter.Untrack.empty());
        CHECK(cAfter.Followed == Ids{0x46EF2, 0x3372B});
    }

    SECTION("a quest the leader sends twice is followed once, and nothing is followed for a leader that follows none")
    {
        const Changes cTwice = Follow(Ids{0x3372B, 0x3372B}, Ids{}, Ids{}, false, cAllRun);
        CHECK(cTwice.Track == Ids{0x3372B});

        const Changes cNone = Follow(Ids{}, Ids{0x3372B}, Ids{0x3372B}, true, cAllRun);
        CHECK(cNone.Track.empty());
        CHECK(cNone.Untrack == Ids{0x3372B});
        CHECK(cNone.Followed.empty());
    }
}
