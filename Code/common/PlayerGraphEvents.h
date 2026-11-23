#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>

// The graph events a player's own game takes with no action behind them: Papyrus Debug.SendAnimationEvent, the console's
// "sae", emote mods. A player's other actions are sent by the game's action hook already; these never were.
// Only the idles below are sent, and played on that player's copies: gestures that need no prop, furniture or target, and the
// events that end them. Each is the event of a Skyrim.esm idle record (IDLE ENAM), the same in the SE and VR Skyrim.esm.
// They go as an action with no action form, its name only, and its Type marked (kTypeBit), so the games and the server tell
// them from the game's own actions.
namespace PlayerGraphEvents
{
inline constexpr std::string_view kAllowed[] = {
    // Gestures.
    "IdleWave",
    "IdleSalute",
    "IdleApplaud2",
    "IdleApplaud3",
    "IdleApplaud4",
    "IdleApplaud5",
    "IdleApplaudSarcastic",
    "IdleLaugh",
    "IdleCiceroDance1",
    "IdleCiceroDance2",
    "IdleCiceroDance3",
    "IdleCiceroHappy",
    "IdleCiceroAgitated",
    "IdleNervous",
    "IdleWipeBrow",
    "IdleGetAttention",
    "IdleSilentBow",
    "IdleSnapToAttention",
    "IdlePray",
    "IdleStudy",
    "IdleExamine",
    "IdlePointClose",
    "IdlePointFar_01",
    "IdlePointFar_02",
    "IdleCivilWarCheer",
    "SpectatorCheer",
    "SpectatorClap",
    // The events that end them.
    "IdleStop",
    "IdleStopInstant",
    "IdleForceDefaultState",
};

inline constexpr std::size_t kAllowedCount = std::size(kAllowed);
static_assert(kAllowedCount < 256);

//! Marks an action as one of these graph events. The game's own Type is its action input (bits 0-1) and bit 2.
inline constexpr uint32_t kTypeBit = 1u << 30;

//! At most this many of a player's graph events a second, sent or played.
inline constexpr std::size_t kMaxPerSecond = 4;
//! At most this many wait to be sent.
inline constexpr std::size_t kMaxQueued = 8;

using Clock = std::chrono::steady_clock;

//! The game's event names are case-insensitive.
constexpr bool SameName(const std::string_view acLeft, const std::string_view acRight) noexcept
{
    if (acLeft.size() != acRight.size())
        return false;
    for (std::size_t i = 0; i < acLeft.size(); ++i)
    {
        char l = acLeft[i];
        char r = acRight[i];
        if (l >= 'A' && l <= 'Z')
            l = static_cast<char>(l - 'A' + 'a');
        if (r >= 'A' && r <= 'Z')
            r = static_cast<char>(r - 'A' + 'a');
        if (l != r)
            return false;
    }
    return true;
}

//! The event's place in kAllowed, none for an event not sent.
constexpr std::optional<uint8_t> IndexOf(const std::string_view acName) noexcept
{
    for (std::size_t i = 0; i < kAllowedCount; ++i)
        if (SameName(kAllowed[i], acName))
            return static_cast<uint8_t>(i);
    return std::nullopt;
}

constexpr bool IsAllowed(const std::string_view acName) noexcept
{
    return IndexOf(acName).has_value();
}

//! Whether an action is one of these graph events rather than an action the game performed.
constexpr bool IsGraphEvent(const uint32_t aType, const uint32_t aActionId) noexcept
{
    return (aType & kTypeBit) != 0 && aActionId == 0;
}

//! The first of the events that end a gesture in kAllowed.
inline constexpr std::size_t kFirstEnd = kAllowedCount - 3;
static_assert(kAllowed[kFirstEnd] == "IdleStop");
//! An event that ends a gesture goes only this long after a gesture: mods and the game send them to reset a player's graph, and on
//! a copy they would stand it up from a seat or a horse.
inline constexpr auto kEndWithin = std::chrono::seconds(10);

//! Lets an event that ends a gesture through only after a gesture, once.
class EndGate
{
public:
    bool Allows(const std::string_view acName, const Clock::time_point aNow) noexcept
    {
        const auto cIndex = IndexOf(acName);
        if (!cIndex)
            return false;
        if (*cIndex < kFirstEnd)
        {
            m_open = true;
            m_gestureAt = aNow;
            return true;
        }
        if (!m_open || aNow - m_gestureAt > kEndWithin)
            return false;
        m_open = false;
        return true;
    }

private:
    bool m_open{false};
    Clock::time_point m_gestureAt{};
};

//! Lets through at most kMaxPerSecond events in any second.
class RateLimit
{
public:
    bool Take(const Clock::time_point aNow) noexcept
    {
        // Once full, m_next is the oldest of the last kMaxPerSecond taken.
        if (m_count == kMaxPerSecond && aNow - m_taken[m_next] < std::chrono::seconds(1))
            return false;
        m_taken[m_next] = aNow;
        m_next = (m_next + 1) % kMaxPerSecond;
        if (m_count < kMaxPerSecond)
            ++m_count;
        return true;
    }

private:
    std::array<Clock::time_point, kMaxPerSecond> m_taken{};
    std::size_t m_next{0};
    std::size_t m_count{0};
};

//! The local player's events waiting to be sent, in order: allowed ones only, within the rate, at most kMaxQueued.
class Outbox
{
public:
    bool Push(const std::string_view acName, const Clock::time_point aNow) noexcept
    {
        const auto cIndex = IndexOf(acName);
        if (!cIndex || m_size == kMaxQueued || !m_gate.Allows(acName, aNow) || !m_rate.Take(aNow))
            return false;
        m_items[(m_head + m_size) % kMaxQueued] = *cIndex;
        ++m_size;
        return true;
    }

    std::optional<std::string_view> Pop() noexcept
    {
        if (m_size == 0)
            return std::nullopt;
        const uint8_t cIndex = m_items[m_head];
        m_head = (m_head + 1) % kMaxQueued;
        --m_size;
        return kAllowed[cIndex];
    }

    [[nodiscard]] std::size_t Size() const noexcept { return m_size; }

private:
    std::array<uint8_t, kMaxQueued> m_items{};
    std::size_t m_head{0};
    std::size_t m_size{0};
    EndGate m_gate;
    RateLimit m_rate;
};
} // namespace PlayerGraphEvents
