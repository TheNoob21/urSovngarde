#include <catch2/catch.hpp>

#include <Services/HeldMover.h>

#include <vector>

namespace
{
struct Copy
{
    uint32_t PlayerId{};
};
} // namespace

TEST_CASE("A carried object is drawn from the copy of the player the server named", "[held_mover]")
{
    using HeldMover::Named;

    const std::vector<Copy> cCopies{{3}, {7}, {9}};
    // Two players side by side: the one named, not the nearest.
    CHECK(Named(cCopies, 7) == size_t{1});
    CHECK(Named(cCopies, 9) == size_t{2});
    // Nobody named, or that player's copy not here: the nearest, as before.
    CHECK_FALSE(Named(cCopies, 0).has_value());
    CHECK_FALSE(Named(cCopies, 4).has_value());
    CHECK_FALSE(Named(std::vector<Copy>{}, 7).has_value());
}
