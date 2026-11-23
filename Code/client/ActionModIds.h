#pragma once

#include <Structs/ActionEvent.h>

//! An action names its action, target and idle by this game's form ids, which are another game's only where both load the
//! plugin at the same index. They also go as server mod ids (ModSystem) and are resolved to this game's forms on arrival.
namespace ActionModIds
{
//! TMods: GetServerModId(uint32_t, GameId&) and GetGameId(const GameId&), as ModSystem.
//! A form the server cannot name (a plugin it does not list) gets none, and its raw id is used on arrival, as before.
template <class TMods> void Fill(const TMods& acMods, ActionEvent& aAction) noexcept
{
    const auto cFill = [&acMods](const uint32_t aFormId, GameId& aId)
    {
        aId = GameId{};
        if (aFormId != 0 && !acMods.GetServerModId(aFormId, aId))
            aId = GameId{};
    };
    cFill(aAction.ActionId, aAction.ModActionId);
    cFill(aAction.TargetId, aAction.ModTargetId);
    cFill(aAction.IdleId, aAction.ModIdleId);
}

//! The sender's forms as this game's. One this game lacks, or a temporary reference (another game's own), resolves to none.
template <class TMods> void Resolve(const TMods& acMods, ActionEvent& aAction) noexcept
{
    const auto cResolve = [&acMods](const GameId& acId, uint32_t& aFormId)
    {
        if (acId)
            aFormId = acMods.GetGameId(acId);
    };
    cResolve(aAction.ModActionId, aAction.ActionId);
    cResolve(aAction.ModTargetId, aAction.TargetId);
    cResolve(aAction.ModIdleId, aAction.IdleId);
}
} // namespace ActionModIds
