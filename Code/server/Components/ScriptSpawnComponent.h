#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

//! A temporary actor a game spawned (a script's PlaceAtMe, a scene's ambush), and which other games stand an actor of their own for it.
//! Every game runs the same scripts, so another game's spawn for the same event is paired with this one (TwinSpawn).
struct ScriptSpawnComponent
{
    GameId CellId{};
    GameId WorldSpaceId{};
    //! Where it was made: the marker a script placed it at, the same in every game.
    glm::vec3 SpawnPosition{};
    std::chrono::steady_clock::time_point SpawnedAt{};
    //! The players whose games were given this character for a spawn of their own.
    TiltedPhoques::Vector<uint32_t> PairedPlayers{};
};
