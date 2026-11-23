#pragma once

#include <ActivationCatchUp.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyActivationCatchUp;
struct ActivateEvent;
struct TESObjectREFR;

/**
 * @brief Replays the levers, chains, puzzle pillars and claw rings other games used before this one came near.
 * The server sends the uses this game missed as it comes near (ActivationService); each is replayed by this player, in order
 * and one at a time, once its cell is loaded here (ActivationCatchUp.h). Replays are not sent back.
 * A lever or a pillar this player uses reports the end state it comes to rest in; one whose end state comes from the server
 * is used here until it is in that state, and left as it is when it cannot be brought there.
 */
struct ActivationCatchUpService
{
    ActivationCatchUpService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~ActivationCatchUpService() noexcept = default;

    TP_NOCOPYMOVE(ActivationCatchUpService);

    //! Whether this player's use of apObject now comes from a replay's script (ActivationCatchUp::kChainWindow).
    static bool FollowsReplay(const TESObjectREFR* apObject) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnNotifyActivationCatchUp(const NotifyActivationCatchUp& acMessage) noexcept;
    void OnActivate(const ActivateEvent& acEvent) noexcept;

private:
    ActivationCatchUp::Plugin PluginOf(uint32_t aFormId) noexcept;
    ActivationCatchUp::Readiness ReadinessOf(uint32_t aFormId) noexcept;
    void ReplayUse(uint32_t aFormId) noexcept;
    void ReportStates() noexcept;

    World& m_world;
    TransportService& m_transport;

    ActivationCatchUp::Pacer m_pacer;
    ActivationCatchUp::StatePacer m_statePacer;
    ActivationCatchUp::StateReporter m_reporter;
    static inline std::chrono::steady_clock::time_point s_lastReplay{};
    bool m_pluginsLooked{};
    uint32_t m_dawnguardPrefix{0x100};
    uint32_t m_dragonbornPrefix{0x100};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_activateConnection;
};
