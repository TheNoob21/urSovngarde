#pragma once

// A trap one player sets off fires in the other players' games too: plain values, so the rules are unit-tested.
//
// A pressure plate, a tripwire or a trap linker fires its trap from its script, activating it with itself as the
// activator, not the player. The traps name their trigger as their activate parent (Skyrim.esm: XAPR), so a trigger that
// activates itself has the game fire its traps too. The game whose own actor stepped on the trigger, or used it, tells the
// others, and each game fires its own copy. Damage needs nothing more: each game takes a trap's hits only on its own actors.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace TrapSync
{
// Skyrim.esm's trap triggers, found by the scripts on them (PressurePlate, PressureReleasePlate, Tripwire, TrapTriggerBase,
// TrapTriggerHinge); the DLCs add none. The Dwemer button is left out: a player presses it, and that use is shared already.
inline constexpr std::array<uint32_t, 11> kTriggerBases{
    0x0001D9F9, // TrapStonePressurePlate01
    0x0001A4BB, // TrapStonePressurePlate02
    0x0006F8EC, // TrapPressurePlateMetal01
    0x0006FBEF, // TrapPressurePlateDwe01
    0x00070B9C, // TrapNorFirePlateSafe01
    0x00075D2B, // TrapNorPressurePedestal01, which fires when its weight is taken off
    0x00022F54, // TrapTripwire01
    0x00079986, // TrapLinker
    0x0007617A, // TrapTriggerBox
    0x000F4974, // LinkMarkerEmpty, a linker too
    0x000AE4E0, // TrapTriggerHinge, a door whose opening fires its trap
};

// aBaseFormId: the trigger's base form as this game has it; Skyrim.esm is always loaded first.
[[nodiscard]] constexpr bool IsTriggerBase(uint32_t aBaseFormId) noexcept
{
    return std::find(kTriggerBases.begin(), kTriggerBases.end(), aBaseFormId) != kTriggerBases.end();
}

// A trigger counts as this game's actor's doing this long after the actor stepped on it, left it or used it: a plate's
// script fires its trap at once, and a linker passes it on within a frame or two.
inline constexpr auto kTouchWindow = std::chrono::seconds(1);
// A trap fired here this recently is not fired again for another game: that game's player's copy set it off here too.
inline constexpr auto kRefireWindow = std::chrono::seconds(2);

class Recent
{
public:
    using Clock = std::chrono::steady_clock;

    // This game's actor stepped on the trigger, left it or used it.
    void Touched(uint32_t aTriggerId, Clock::time_point aNow) { m_touched[aTriggerId] = aNow; }
    // Whether the trigger's firing of a trap now is this game's to send.
    [[nodiscard]] bool IsTouchedHere(uint32_t aTriggerId, Clock::time_point aNow) const noexcept { return IsWithin(m_touched, aTriggerId, aNow, kTouchWindow); }

    // The trap fired here, for this game's own reasons or another game's.
    void Fired(uint32_t aTrapId, Clock::time_point aNow) { m_fired[aTrapId] = aNow; }
    [[nodiscard]] bool IsFiredRecently(uint32_t aTrapId, Clock::time_point aNow) const noexcept { return IsWithin(m_fired, aTrapId, aNow, kRefireWindow); }

    // Another game's trigger activated itself here, which fires every trap it is the activate parent of: that game's word of
    // each of them, sent right after, is not fired again.
    void Replayed(uint32_t aTriggerId, Clock::time_point aNow) { m_replayed[aTriggerId] = aNow; }
    [[nodiscard]] bool IsReplayed(uint32_t aTriggerId, Clock::time_point aNow) const noexcept { return IsWithin(m_replayed, aTriggerId, aNow, kRefireWindow); }

    // Drops what no longer counts, so only the traps used lately are kept.
    void Forget(Clock::time_point aNow)
    {
        std::erase_if(m_touched, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kTouchWindow; });
        std::erase_if(m_fired, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kRefireWindow; });
        std::erase_if(m_replayed, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kRefireWindow; });
    }

    void Clear() noexcept
    {
        m_touched.clear();
        m_fired.clear();
        m_replayed.clear();
    }

    [[nodiscard]] size_t Size() const noexcept { return m_touched.size() + m_fired.size() + m_replayed.size(); }

private:
    static bool IsWithin(const std::unordered_map<uint32_t, Clock::time_point>& acTimes, uint32_t aId, Clock::time_point aNow, Clock::duration aWindow) noexcept
    {
        const auto cIt = acTimes.find(aId);
        return cIt != acTimes.end() && aNow - cIt->second < aWindow;
    }

    std::unordered_map<uint32_t, Clock::time_point> m_touched;
    std::unordered_map<uint32_t, Clock::time_point> m_fired;
    std::unordered_map<uint32_t, Clock::time_point> m_replayed;
};

// One step on a trigger fires its trap twice in a game that sees the stepper as a copy: once when the copy steps on it there,
// once for the stepper's own game's word. A trap that toggles (swinging blades, some levers) then stops again. So the copy's
// firing and the word are paired, in whichever order they come: the second of each pair is dropped. A word fired here first
// drops the copy's firing in the Activate hook, before the game runs it; a copy's firing first drops the word.
// Used from the game's threads (the hook and the trigger events) and the world's update, so it locks.
inline constexpr auto kPairWindow = std::chrono::seconds(10);
// More unpaired firings than this of one trap are not kept: a stuck trigger must not grow the ledger.
inline constexpr size_t kMaxUnpaired = 8;

class Ledger
{
public:
    using Clock = std::chrono::steady_clock;

    enum class Toucher
    {
        kNone,   // no actor stepped on, left or used the trigger in the last kTouchWindow
        kLocal,  // an actor this game runs did; its firing is this game's to send
        kRemote, // only a copy of another game's actor did; its firing pairs with that game's word
    };

    // An actor stepped on the trigger, left it or used it; called from the game's trigger events.
    void Touched(uint32_t aTriggerId, bool aRemote, Clock::time_point aNow)
    {
        std::lock_guard lock(m_mutex);
        (aRemote ? m_remoteTouch : m_localTouch)[aTriggerId] = aNow;
    }

    // Who set the trigger off just now; an actor of this game wins over a copy.
    [[nodiscard]] Toucher LastToucher(uint32_t aTriggerId, Clock::time_point aNow) const
    {
        std::lock_guard lock(m_mutex);
        if (IsWithin(m_localTouch, aTriggerId, aNow))
            return Toucher::kLocal;
        if (IsWithin(m_remoteTouch, aTriggerId, aNow))
            return Toucher::kRemote;
        return Toucher::kNone;
    }

    // A copy set the trap off here. False when another game's word for it fired it here already: the game must not fire it
    // again, and that word is paired. Otherwise the firing waits for its word, and true.
    [[nodiscard]] bool CopyFire(uint32_t aTrapId, Clock::time_point aNow)
    {
        std::lock_guard lock(m_mutex);
        Prune(aNow);
        auto& unpaired = m_unpaired[aTrapId];
        if (!unpaired.Words.empty())
        {
            unpaired.Words.pop_front();
            return false;
        }
        Push(unpaired.CopyFires, aNow);
        return true;
    }

    // Another game's word for the trap came. True when a copy's firing here is waiting for it: the word is not fired, and that
    // firing is paired.
    [[nodiscard]] bool TakeCopyFire(uint32_t aTrapId, Clock::time_point aNow)
    {
        std::lock_guard lock(m_mutex);
        Prune(aNow);
        const auto cIt = m_unpaired.find(aTrapId);
        if (cIt == m_unpaired.end() || cIt->second.CopyFires.empty())
            return false;
        cIt->second.CopyFires.pop_front();
        return true;
    }

    // Another game's word fired the trap here; the copy's firing for the same step is dropped when it comes.
    void WordFired(uint32_t aTrapId, Clock::time_point aNow)
    {
        std::lock_guard lock(m_mutex);
        Prune(aNow);
        Push(m_unpaired[aTrapId].Words, aNow);
    }

    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_localTouch.clear();
        m_remoteTouch.clear();
        m_unpaired.clear();
    }

    [[nodiscard]] size_t Size(Clock::time_point aNow)
    {
        std::lock_guard lock(m_mutex);
        Prune(aNow);
        size_t size = m_localTouch.size() + m_remoteTouch.size();
        for (const auto& [cId, cUnpaired] : m_unpaired)
            size += cUnpaired.CopyFires.size() + cUnpaired.Words.size();
        return size;
    }

private:
    struct Unpaired
    {
        std::deque<Clock::time_point> CopyFires;
        std::deque<Clock::time_point> Words;
    };

    static bool IsWithin(const std::unordered_map<uint32_t, Clock::time_point>& acTimes, uint32_t aId, Clock::time_point aNow) noexcept
    {
        const auto cIt = acTimes.find(aId);
        return cIt != acTimes.end() && aNow - cIt->second < kTouchWindow;
    }

    static void Push(std::deque<Clock::time_point>& aTimes, Clock::time_point aNow)
    {
        if (aTimes.size() >= kMaxUnpaired)
            aTimes.pop_front();
        aTimes.push_back(aNow);
    }

    // Drops what no longer counts: touches older than kTouchWindow, unpaired firings older than kPairWindow.
    void Prune(Clock::time_point aNow)
    {
        std::erase_if(m_localTouch, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kTouchWindow; });
        std::erase_if(m_remoteTouch, [aNow](const auto& acEntry) { return aNow - acEntry.second >= kTouchWindow; });
        const auto cOld = [aNow](const Clock::time_point acAt)
        {
            return aNow - acAt >= kPairWindow;
        };
        for (auto it = m_unpaired.begin(); it != m_unpaired.end();)
        {
            std::erase_if(it->second.CopyFires, cOld);
            std::erase_if(it->second.Words, cOld);
            if (it->second.CopyFires.empty() && it->second.Words.empty())
                it = m_unpaired.erase(it);
            else
                ++it;
        }
    }

    mutable std::mutex m_mutex;
    std::unordered_map<uint32_t, Clock::time_point> m_localTouch;
    std::unordered_map<uint32_t, Clock::time_point> m_remoteTouch;
    std::unordered_map<uint32_t, Unpaired> m_unpaired;
};

// Set while this game fires a trap for another game's word, so the Activate hook does not count that firing as a copy's.
inline thread_local bool t_firingForWord = false;

class ScopedFiringForWord
{
public:
    ScopedFiringForWord() noexcept
        : m_was(t_firingForWord)
    {
        t_firingForWord = true;
    }
    ~ScopedFiringForWord() noexcept { t_firingForWord = m_was; }
    ScopedFiringForWord(const ScopedFiringForWord&) = delete;
    ScopedFiringForWord& operator=(const ScopedFiringForWord&) = delete;

private:
    bool m_was;
};

// The one ledger of this game, shared by the Activate hook and ObjectService.
inline Ledger& SharedLedger()
{
    static Ledger s_ledger;
    return s_ledger;
}

// The server relays a handful of traps a second from one player at most: a linker can fire several at once, a stuck
// trigger must not flood the others.
struct RateLimit
{
    static constexpr uint32_t kPerSecond = 8;

    std::chrono::steady_clock::time_point Since{};
    uint32_t Count{};

    bool Allow(std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (Since == std::chrono::steady_clock::time_point{} || aNow - Since >= std::chrono::seconds(1))
        {
            Since = aNow;
            Count = 0;
        }
        if (Count >= kPerSecond)
            return false;
        ++Count;
        return true;
    }
};
} // namespace TrapSync
