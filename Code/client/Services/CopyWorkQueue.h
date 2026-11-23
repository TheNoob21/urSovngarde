#pragma once

#include <CopyWork.h>

#include <mutex>

/**
 * @brief Work on copies decided in the client's update and done at the start of the next frame on the main thread (CopyWork.h).
 *
 * TWork is what is to be done, TDone what came of it, which the next update takes back. Both sides lock, as the update can run
 * beside the main thread during a load.
 */
template <class TWork, class TDone> struct CopyWorkQueue
{
    uint32_t Issue(const uint32_t aKey) noexcept
    {
        std::scoped_lock lock(m_lock);
        return m_tickets.Issue(aKey);
    }

    void Retire(const uint32_t aKey) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_tickets.Retire(aKey);
    }

    [[nodiscard]] bool Live(const uint32_t aKey, const uint32_t aTicket) noexcept
    {
        std::scoped_lock lock(m_lock);
        return m_tickets.Live(aKey, aTicket);
    }

    void Place(const uint32_t aKey, const uint32_t aTicket) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_tickets.Place(aKey, aTicket);
    }

    [[nodiscard]] uint32_t InPlace(const uint32_t aKey) noexcept
    {
        std::scoped_lock lock(m_lock);
        return m_tickets.InPlace(aKey);
    }

    void Push(const TWork& acWork) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_work.push_back(acWork);
    }

    //! The work to do now. What is held back goes in front of anything queued meanwhile (Hold), so the order is kept.
    TiltedPhoques::Vector<TWork> TakeWork() noexcept
    {
        TiltedPhoques::Vector<TWork> work;
        std::scoped_lock lock(m_lock);
        work.swap(m_work);
        return work;
    }

    void Hold(TiltedPhoques::Vector<TWork>& aHeld) noexcept
    {
        if (aHeld.empty())
            return;
        std::scoped_lock lock(m_lock);
        aHeld.insert(aHeld.end(), m_work.begin(), m_work.end());
        m_work.swap(aHeld);
    }

    void Give(const TDone& acDone) noexcept
    {
        std::scoped_lock lock(m_lock);
        m_done.push_back(acDone);
    }

    TiltedPhoques::Vector<TDone> TakeDone() noexcept
    {
        TiltedPhoques::Vector<TDone> done;
        std::scoped_lock lock(m_lock);
        done.swap(m_done);
        return done;
    }

    //! Nothing more is done: the game is exiting.
    void Clear() noexcept
    {
        std::scoped_lock lock(m_lock);
        m_work.clear();
        m_done.clear();
        m_tickets.Clear();
    }

private:
    std::mutex m_lock;
    TiltedPhoques::Vector<TWork> m_work;
    TiltedPhoques::Vector<TDone> m_done;
    CopyWork::Tickets m_tickets;
};
