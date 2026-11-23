#pragma once

#include <cstdint>
#include <string_view>

//! A VR player's shouts, as the voice actions a flat player's game performs for the same.
//!
//! A VR player shouts with the shout button too, and the game casts the shout, but the VR body performs no voice action that
//! the other games could replay: the copies of that player showed the shout's effects only, with no shout pose. From the
//! shout's cast, the VR game sends the two actions a flat player's would (start, then release), and every copy shouts.
namespace VRShout
{
//! Skyrim.esm's voice actions (BGSAction), the same form ids in every game.
inline constexpr uint32_t kActionVoice = 0x13008;
inline constexpr uint32_t kActionVoiceRelease = 0x13457;
inline constexpr uint32_t kActionVoiceReady = 0x13458;
inline constexpr uint32_t kActionVoiceInterrupt = 0x13459;

//! The graph events of a flat character's shout (the master behaviour's names).
inline constexpr std::string_view kStartEvent = "shoutStart";
inline constexpr std::string_view kReleaseEvent = "shoutRelease";

//! How long a shout's cast waits for a voice action the game performs for it itself (ms). The copies play about a tenth of a
//! second behind, so the actions still arrive before their tick plays there.
inline constexpr uint64_t kOwnActionWait = 60;
//! A voice action of the game's own this long before a shout's cast, or this long after it, was performed for that shout (ms).
//! A shout is held up to about 1.5 s before it is released.
inline constexpr uint64_t kOwnActionBefore = 2000;
inline constexpr uint64_t kOwnActionAfter = 250;
//! Two shouts are never this close: the second cast is the same shout, seen again (ms).
inline constexpr uint64_t kSameShout = 300;

//! SpellItem types: only a dragon shout (or a beast's howl, the same kind) shows as a shout. A power or a lesser power is no
//! shout, and VR control menus are lesser powers.
inline constexpr int32_t kVoicePower = 0xB;

inline bool IsVoiceAction(const uint32_t aActionId) noexcept
{
    return aActionId == kActionVoice || aActionId == kActionVoiceRelease || aActionId == kActionVoiceReady || aActionId == kActionVoiceInterrupt;
}

//! Whether a cast of this spell type is a shout to show.
inline bool IsShout(const int32_t aSpellType) noexcept
{
    return aSpellType == kVoicePower;
}

//! What to do at aNow with a shout cast at aCastTick, when the game's newest voice action for this player was at aOwnTick
//! (0: none yet): wait a moment longer, send its actions, or drop it because the game showed it already.
enum class Decision : uint8_t
{
    Wait,
    Send,
    Shown,
};

inline Decision Decide(const uint64_t aCastTick, const uint64_t aOwnTick, const uint64_t aNow) noexcept
{
    if (aOwnTick != 0 && aOwnTick + kOwnActionBefore >= aCastTick && aOwnTick <= aCastTick + kOwnActionAfter)
        return Decision::Shown;
    if (aNow < aCastTick + kOwnActionWait)
        return Decision::Wait;
    return Decision::Send;
}

//! Whether a cast at aCastTick is a new shout, the last one shown at aLastTick (0: none).
inline bool IsNewShout(const uint64_t aCastTick, const uint64_t aLastTick) noexcept
{
    return aLastTick == 0 || aCastTick >= aLastTick + kSameShout || aCastTick < aLastTick;
}
} // namespace VRShout
