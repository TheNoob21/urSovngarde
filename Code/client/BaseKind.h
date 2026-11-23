#pragma once

#include <cstdint>

//! Whether the owner's base for a creature is another kind of creature than this game's own, so that a copy of the owner's stands in for it.
//!
//! A leveled list can hold two creatures that share a name and differ in race (a Frostbite Spider and a large one). Name alone calls them the same kind.
//! So the race decides whenever both can be read: two person races are never foreign, whatever their names (a Nord and a Breton bandit share one graph).
//! The name decides only when a race cannot be read.
//!
namespace BaseKind
{
enum class Decider
{
    Name,           //!< the names agree and the race did not decide
    Race,           //!< both races were read
    RaceUnreadable, //!< the race could not be read and the names differ
};

struct Verdict
{
    bool Foreign = false;
    Decider By = Decider::Name;
};

//! aLocalRace and aRemoteRace are the races' form ids, 0 when not readable; a local race is only trusted when the actor's own race agrees with its base.
inline Verdict Decide(const bool aSameName, const uint32_t aLocalRace, const uint32_t aRemoteRace, const bool aBothPersons) noexcept
{
    if (aLocalRace && aRemoteRace)
        return {aLocalRace != aRemoteRace && !aBothPersons, Decider::Race};
    if (aSameName)
        return {false, Decider::Name};
    return {true, Decider::RaceUnreadable};
}
} // namespace BaseKind
