#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

//! What a save writes masked is put back however the save is left, and a save started inside another (nested) keeps the outer one's
//! masks until the outer one is done. Plain values in and out, so it has unit tests (Code/tests/save_scope.cpp).
namespace SaveScope
{
//! For as long as it lives, one save is under way for apOwner: EnterSave on the way in, LeaveSave on the way out, however the scope is
//! left. The owner counts nested ones itself.
template <class TOwner> class Scoped
{
public:
    explicit Scoped(TOwner* apOwner) noexcept
        : m_pOwner(apOwner)
    {
        if (m_pOwner)
            m_pOwner->EnterSave();
    }

    ~Scoped() noexcept
    {
        if (m_pOwner)
            m_pOwner->LeaveSave();
    }

    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

private:
    TOwner* m_pOwner;
};

//! The forms being written to a save right now, each with the mask its first write put on. Another write of the same form, nested
//! inside it or on another thread, is made only once that mask is on, and the mask comes off when the last of its writes ends.
//! A write that comes while the mask comes off waits until it is off, then puts on its own.
template <class TMask> class FormsWriting
{
public:
    //! Writes aFormId with aWrite(). The first write of it makes its mask with aMake() (a std::unique_ptr<TMask>, or null for none).
    template <class TMake, class TWrite> void Write(const uint32_t aFormId, TMake&& aMake, TWrite&& aWrite)
    {
        struct Leave
        {
            FormsWriting& Writing;
            uint32_t FormId;
            bool Counted{false};
            ~Leave()
            {
                if (Counted)
                    Writing.End(FormId);
            }
        } leave{*this, aFormId};

        std::unique_lock lock(m_lock);
        m_ready.wait(
            lock,
            [this, aFormId]
            {
                const Entry* pEntry = Find(aFormId);
                return !pEntry || !pEntry->Restoring;
            });
        if (Entry* pEntry = Find(aFormId))
        {
            ++pEntry->Writers;
            leave.Counted = true;
            m_ready.wait(lock, [this, aFormId] { return Find(aFormId)->Ready; });
            lock.unlock();
        }
        else
        {
            m_forms.push_back(Entry{aFormId});
            leave.Counted = true;
            lock.unlock();
            // Ready even if the mask cannot be made: the other writes then go ahead unmasked, as this one does.
            struct Publish
            {
                FormsWriting& Writing;
                uint32_t FormId;
                std::unique_ptr<TMask> pMask;
                ~Publish()
                {
                    std::scoped_lock publishLock(Writing.m_lock);
                    Entry* pEntry = Writing.Find(FormId);
                    pEntry->pMask = std::move(pMask);
                    pEntry->Ready = true;
                    Writing.m_ready.notify_all();
                }
            } publish{*this, aFormId};
            publish.pMask = aMake();
        }
        aWrite();
    }

    [[nodiscard]] bool Writing(const uint32_t aFormId) const noexcept
    {
        std::scoped_lock lock(m_lock);
        return std::find_if(m_forms.begin(), m_forms.end(), [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; }) != m_forms.end();
    }

private:
    struct Entry
    {
        uint32_t FormId{};
        uint32_t Writers{1};
        bool Ready{false};
        bool Restoring{false};
        std::unique_ptr<TMask> pMask;
    };

    Entry* Find(const uint32_t aFormId) noexcept
    {
        const auto it = std::find_if(m_forms.begin(), m_forms.end(), [aFormId](const Entry& acEntry) { return acEntry.FormId == aFormId; });
        return it != m_forms.end() ? &*it : nullptr;
    }

    // The last write takes the mask off, outside the lock. The form stays listed until it is off, so no write starts meanwhile.
    void End(const uint32_t aFormId) noexcept
    {
        std::unique_ptr<TMask> pMask;
        {
            std::scoped_lock lock(m_lock);
            Entry* pEntry = Find(aFormId);
            if (!pEntry || --pEntry->Writers != 0)
                return;
            pEntry->Restoring = true;
            pMask = std::move(pEntry->pMask);
        }
        pMask.reset();
        std::scoped_lock lock(m_lock);
        if (Entry* pEntry = Find(aFormId))
            m_forms.erase(m_forms.begin() + (pEntry - m_forms.data()));
        m_ready.notify_all();
    }

    mutable std::mutex m_lock;
    std::condition_variable m_ready;
    std::vector<Entry> m_forms;
};
} // namespace SaveScope
