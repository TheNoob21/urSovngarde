#pragma once

#include <common/ExtensionApi.h>
#include <array>
#include <mutex>

class ExtensionHandlers
{
public:
    uint32_t Register(uint32_t aAddon, const UrExtensionHandler* apHandler)
    {
        if (!aAddon || !apHandler || !apHandler->Receive)
            return 0;
        std::scoped_lock lock(m_lock);
        if (m_count == m_entries.size())
            return 0;
        for (size_t i = 0; i < m_count; ++i)
            if (m_entries[i].Addon == aAddon)
                return 0;
        m_entries[m_count++] = {aAddon, *apHandler};
        return 1;
    }

    template <class F> void Visit(F&& aVisit)
    {
        std::unique_lock lock(m_lock);
        const auto count = m_count;
        // Published entries never change and a callback may itself register another handler.
        lock.unlock();
        for (size_t i = 0; i < count; ++i)
            aVisit(m_entries[i].Addon, m_entries[i].Handler);
    }

    void Receive(uint32_t aSender, const UrExtensionMessage& acMessage)
    {
        Visit(
            [&](uint32_t aAddon, const UrExtensionHandler& acHandler)
            {
                if (aAddon == acMessage.Addon)
                    acHandler.Receive(aSender, &acMessage);
            });
    }

private:
    struct Entry
    {
        uint32_t Addon;
        UrExtensionHandler Handler;
    };
    std::mutex m_lock;
    std::array<Entry, 32> m_entries{};
    size_t m_count{};
};
