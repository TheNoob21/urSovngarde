#pragma once

struct TESActionData;

struct ActorMediator
{
    virtual ~ActorMediator(){};

    static ActorMediator* Get() noexcept;

    bool PerformAction(TESActionData* apAction) noexcept;
    bool ForceAction(TESActionData* apAction) noexcept;
    //! Performs an action of this game's own actor as the game would, so other players' copies replay it.
    bool PerformOwnAction(TESActionData* apAction) noexcept;

    // Reverse engineering stuff, do not use
    bool RePerformAction(TESActionData* apAction) noexcept;
    bool RePerformIdleAction(TESActionData* apData) noexcept;
    bool RePerformComplexAction(TESActionData* apData) noexcept;
};
