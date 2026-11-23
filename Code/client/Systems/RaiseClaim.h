#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

//! When this player raises a dead body another game runs (a reanimate effect), with no game in it.
//! The body is asked for first and raised here once this game owns it, so that it is this game's AI, as in the base game,
//! that follows the player and fights for them. A body not handed over in time has the raise sent to the game that runs it
//! (MagicTarget::IsRaiseOnCopy), which raises it there.
namespace RaiseClaim
{
struct Attempt
{
    bool Connected{};
    bool CasterIsPlayer{}; //!< this game's player made the effect
    bool TargetIsCopy{};   //!< another game runs the body
    bool Reanimate{};      //!< the effect raises the dead
    bool Dead{};
    bool ToOwner{}; //!< a raise already being sent to the game that runs the body (it was not handed over)
};

//! Whether the effect waits for the body instead of being made on the copy now.
constexpr bool AskFirst(const Attempt& acAttempt) noexcept
{
    return acAttempt.Connected && acAttempt.CasterIsPlayer && acAttempt.TargetIsCopy && acAttempt.Reanimate && acAttempt.Dead && !acAttempt.ToOwner;
}

enum class Then : unsigned char
{
    Nothing,   //!< nothing left to make (no longer dead: someone else raised or revived it)
    RaiseHere, //!< made on the body this game now runs
    ToOwner,   //!< sent to the game that runs the body, which raises it there
};

//! The body came to this game while the raise waited for it.
constexpr Then OnHandedOver(const bool aDeadHere) noexcept
{
    return aDeadHere ? Then::RaiseHere : Then::Nothing;
}

//! The body was not handed over in time, or could not be asked for.
constexpr Then OnNotHandedOver(const bool aDeadHere) noexcept
{
    return aDeadHere ? Then::ToOwner : Then::Nothing;
}

//! The body a raise is for, as the hook found it: its server id and handle, in the connection it was made in.
struct Target
{
    uint32_t ServerId{};
    uint32_t Handle{};
    uint32_t Session{};
};

using Clock = std::chrono::steady_clock;

//! How long a raise waits for its body, and how long a raise decided on waits for the main thread.
inline constexpr auto kWaitForBody = std::chrono::seconds(3);
inline constexpr auto kMakeWithin = std::chrono::seconds(1);

//! A raise decided on, to be made on the main thread.
template <class TCast> struct Ready
{
    Target Body{};
    Then Make{Then::Nothing};
    Clock::time_point Until{};
    TCast Cast{};
};

//! Whether the update asks for the body the hook queued a raise for: the hook's handle still gives that reference, and the copy found
//! under its form id is it. A body replaced meanwhile is not asked for, so its owner never changes for this raise.
constexpr bool MayAsk(const bool aSameBody, const bool aBound) noexcept
{
    return aSameBody && aBound;
}

//! The body as the main thread finds it by the handle, when the raise is to be made.
struct Found
{
    uint32_t Session{};
    bool SameBody{}; //!< the handle still gives the reference the raise was made on
    bool Here{};     //!< this game runs it
    bool Dead{};
    bool Loading{}; //!< a load is under way, or the game is exiting
    bool Decided{}; //!< it has not changed hands since the decision (Raises::Decided)
};

//! Whether a raise decided on is still to be made: in the same connection, in time, on the same body, still dead, not changed hands
//! since, and still run by the game it was decided for (here for RaiseHere, the other game for ToOwner).
template <class TCast> constexpr bool StillToMake(const Ready<TCast>& acReady, const Found& acFound, const Clock::time_point aNow) noexcept
{
    if (acReady.Make == Then::Nothing || acFound.Session != acReady.Body.Session || aNow >= acReady.Until || !acFound.SameBody || !acFound.Dead || acFound.Loading ||
        !acFound.Decided)
        return false;
    return acReady.Make == Then::RaiseHere ? acFound.Here : !acFound.Here;
}

/**
 * @brief Raises waiting for their body, and raises decided on for the main thread to make.
 *
 * The update decides (Wait, HandedOver, NotAskedFor, Expire), the main thread makes (Take, then StillToMake). The body changing hands
 * after a decision withdraws it (Withdraw), taken by the main thread or not, up to its last check before the effect (Decided). A
 * disconnect drops everything and refuses raises from before it (Clear).
 */
template <class TCast> class Raises
{
public:
    [[nodiscard]] uint32_t Session() const noexcept { return m_session.load(); }

    //! A disconnect: nothing waiting or decided is made, and raises from before it are refused.
    void Clear() noexcept
    {
        std::scoped_lock lock(m_lock);
        ++m_session;
        m_waiting.clear();
        m_ready.clear();
        m_taken.clear();
    }

    //! A body asked for already and still waited for: the newest cast is the one made. False when it is not waited for.
    bool Renew(const Target& acBody, const Clock::time_point aNow, const TCast& acCast) noexcept
    {
        std::scoped_lock lock(m_lock);
        Waiting* pWaiting = FindWaiting(acBody.ServerId);
        if (!pWaiting || acBody.Session != m_session.load() || aNow >= pWaiting->Until || pWaiting->Body.Handle != acBody.Handle)
            return false;
        pWaiting->Cast = acCast;
        return true;
    }

    //! The body is asked for: the raise waits for it until aNow + kWaitForBody. False for a raise from before a disconnect.
    bool Wait(const Target& acBody, const Clock::time_point aNow, const TCast& acCast) noexcept
    {
        std::scoped_lock lock(m_lock);
        if (acBody.Session != m_session.load())
            return false;
        if (Waiting* pWaiting = FindWaiting(acBody.ServerId))
            *pWaiting = Waiting{acBody, aNow + kWaitForBody, acCast};
        else
            m_waiting.push_back(Waiting{acBody, aNow + kWaitForBody, acCast});
        return true;
    }

    //! The body came to this game. In time and still dead, the raise is made here; late, or no longer dead, it is dropped.
    Then HandedOver(const uint32_t aServerId, const bool aDeadHere, const Clock::time_point aNow) noexcept
    {
        std::scoped_lock lock(m_lock);
        const auto it = std::find_if(m_waiting.begin(), m_waiting.end(), [aServerId](const Waiting& acWaiting) { return acWaiting.Body.ServerId == aServerId; });
        if (it == m_waiting.end())
            return Then::Nothing;
        const Waiting cWaiting = *it;
        m_waiting.erase(it);
        const Then cThen = aNow < cWaiting.Until && cWaiting.Body.Session == m_session.load() ? OnHandedOver(aDeadHere) : Then::Nothing;
        Decide(cWaiting.Body, cThen, aNow, cWaiting.Cast);
        return cThen;
    }

    //! The body could not be asked for: the raise is sent to the game that runs it, if it is still dead.
    Then NotAskedFor(const Target& acBody, const bool aDeadHere, const Clock::time_point aNow, const TCast& acCast) noexcept
    {
        std::scoped_lock lock(m_lock);
        const Then cThen = acBody.Session == m_session.load() ? OnNotHandedOver(aDeadHere) : Then::Nothing;
        Decide(acBody, cThen, aNow, acCast);
        return cThen;
    }

    //! Raises whose body did not come in time are sent to the game that runs it, if still dead (aDeadHere(server id)).
    template <class TDeadHere> size_t Expire(const Clock::time_point aNow, TDeadHere&& aDeadHere)
    {
        std::scoped_lock lock(m_lock);
        size_t expired = 0;
        for (auto it = m_waiting.begin(); it != m_waiting.end();)
        {
            if (aNow < it->Until)
            {
                ++it;
                continue;
            }
            Decide(it->Body, OnNotHandedOver(aDeadHere(it->Body.ServerId)), aNow, it->Cast);
            it = m_waiting.erase(it);
            ++expired;
        }
        return expired;
    }

    //! The body changed hands: a raise decided on for it is not made, as that was decided for its owner then. One the main thread
    //! has taken already is marked, for its check before the effect.
    void Withdraw(const uint32_t aServerId) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_ready.erase(std::remove_if(m_ready.begin(), m_ready.end(), [aServerId](const Ready<TCast>& acReady) { return acReady.Body.ServerId == aServerId; }), m_ready.end());
        for (Taken& taken : m_taken)
            taken.Withdrawn = taken.Withdrawn || taken.ServerId == aServerId;
    }

    //! The raises decided on, for the main thread. Each stays open to Withdraw until the next Take.
    std::vector<Ready<TCast>> Take() noexcept
    {
        std::scoped_lock lock(m_lock);
        std::vector<Ready<TCast>> ready;
        ready.swap(m_ready);
        m_taken.clear();
        for (const Ready<TCast>& cReady : ready)
            m_taken.push_back(Taken{cReady.Body.ServerId, false});
        return ready;
    }

    //! Whether the raise at aIndex of the last Take is still decided: its body has not changed hands since, nor was there a disconnect.
    [[nodiscard]] bool Decided(const size_t aIndex) const noexcept
    {
        std::scoped_lock lock(m_lock);
        return aIndex < m_taken.size() && !m_taken[aIndex].Withdrawn;
    }

    [[nodiscard]] bool Waits() const noexcept
    {
        std::scoped_lock lock(m_lock);
        return !m_waiting.empty();
    }

private:
    struct Waiting
    {
        Target Body{};
        Clock::time_point Until{};
        TCast Cast{};
    };

    struct Taken
    {
        uint32_t ServerId{};
        bool Withdrawn{};
    };

    Waiting* FindWaiting(const uint32_t aServerId) noexcept
    {
        const auto it = std::find_if(m_waiting.begin(), m_waiting.end(), [aServerId](const Waiting& acWaiting) { return acWaiting.Body.ServerId == aServerId; });
        return it != m_waiting.end() ? &*it : nullptr;
    }

    void Decide(const Target& acBody, const Then aThen, const Clock::time_point aNow, const TCast& acCast) noexcept
    {
        if (aThen != Then::Nothing)
            m_ready.push_back(Ready<TCast>{acBody, aThen, aNow + kMakeWithin, acCast});
    }

    mutable std::mutex m_lock;
    std::atomic<uint32_t> m_session{1};
    std::vector<Waiting> m_waiting;
    std::vector<Ready<TCast>> m_ready;
    std::vector<Taken> m_taken;
};
} // namespace RaiseClaim
