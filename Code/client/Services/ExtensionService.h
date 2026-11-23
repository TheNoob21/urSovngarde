#pragma once

#include <Messages/NotifyExtension.h>

#include <chrono>
#include <mutex>
#include <set>
#include <vector>

struct DisconnectedEvent;

struct ExtensionService
{
    explicit ExtensionService(entt::dispatcher& aDispatcher);
    TP_NOCOPYMOVE(ExtensionService);

    //! Deliver network notifications and run registered modules between game frames, on the game's main loop.
    void Update();

private:
    void OnNotify(const NotifyExtension& acMessage);
    void OnDisconnect(const DisconnectedEvent&);

    std::mutex m_inboxLock;
    std::vector<NotifyExtension> m_inbox;
    bool m_disconnected{};
    bool m_resync{};
    std::chrono::steady_clock::time_point m_previous{std::chrono::steady_clock::now()};
    std::set<uint32_t> m_announced;
    std::set<uint32_t> m_refresh;
    uint64_t m_previousSave{};

    entt::scoped_connection m_notify;
    entt::scoped_connection m_disconnect;
};
