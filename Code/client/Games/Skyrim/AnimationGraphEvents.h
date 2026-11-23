#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

//! The graph events the game and mods send to NPCs, as Character's IAnimationGraphManagerHolder slot sees them.
//! A copy plays only its owner's stagger: one started on it here is held back, and its owner's game is told (HitReactionService).
namespace AnimationGraphEvents
{
//! A copy replays its owner's stagger or recoil action (ActorMediator::ForceAction). Any thread.
void NoteReplayedReaction(uint32_t aFormId) noexcept;

//! The next of the local player's own graph events to send, of those PlayerGraphEvents.h lets through, oldest first.
//! The first call once the player exists hooks the player's graph event slot. Main thread.
std::optional<std::string_view> TakePlayerEvent() noexcept;

} // namespace AnimationGraphEvents
