#pragma once

#include <cstdint>

struct Actor;
struct NotifyFollowerCommand;

// Another player's follower, asked to wait or to follow her player again (FollowerCommand.h, urSovngardeFollowerEffect.psc).
// Her player's game runs her: this game sends the word there, and carries out the words that reach the followers it runs.
// The script side is the self spell of urSovngardeCoopMenu.esp: on this player it shows the choice, on a follower it carries the word out.
namespace FollowerCommands
{
// This player activated another player's follower: the choice is shown. False without the menu plugin (nothing happens, as before).
bool Offer(Actor* apCopy) noexcept;

// The player's choice (script thread): a word is sent to her player's game; anything else only ends the choice.
void Ask(int32_t aCommand) noexcept;

// The copy the player is choosing for (script thread), for a trade with her here; 0 for none.
uint32_t Chosen() noexcept;

// A word to a follower this game runs, or the answer to this player's word (game thread).
void OnNotify(const NotifyFollowerCommand& acMessage) noexcept;

// The word waiting for the follower the script runs on (script thread); 0 for none.
int32_t Take(uint32_t aFormId) noexcept;

// The script's answer for that word (script thread): told to the player who asked, and to this player.
void Done(uint32_t aFormId, int32_t aCommand, int32_t aReply) noexcept;

// A spell of the menu plugin (the power, the follower spell): cast by this game for itself, never sent to the others.
bool IsMenuSpell(uint32_t aFormId) noexcept;

// A positive native addition with no old container, to the exact open trade target.
// Gives no authority to other remote NPC changes; the hooks still take a current ownership token.
bool MayAddTradeItem(Actor* apActor, int32_t aCount, bool aHasOldContainer) noexcept;

// Words the script never answered are answered for it (game thread, with the update).
void Sweep() noexcept;

// Everything forgotten, at a disconnect.
void Reset() noexcept;
} // namespace FollowerCommands
