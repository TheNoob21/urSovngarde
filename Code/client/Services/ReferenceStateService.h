#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/RequestReferenceState.h>
#include <ReferenceStateSettle.h>
#include <ReferenceStates.h>

#include <atomic>
#include <chrono>
#include <unordered_map>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct ActivateEvent;
struct BearTrapEvent;
struct NotifyReferenceStates;
struct TESObjectREFR;

/**
 * @brief Shares the world reference states this game changes with the other games, and brings this game's references to
 * the states they changed (ReferenceStates.h).
 *
 * Sent, a few a second at most: a shared destructible's new stage (an ore vein depleted is one), a bear trap sprung or armed,
 * a plant this player picked, a rune trap gone off, and one of those back as new once its cell reset it. Applied as
 * ReferenceStateSettle.h says. A kept state comes only while it is fresh for the game's respawn time. Nothing applied here is
 * sent back.
 */
class ReferenceStateService final : public BSTEventSink<TESDestructionStageChangedEvent>, public BSTEventSink<TESLoadGameEvent>
{
public:
    ReferenceStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~ReferenceStateService() noexcept = default;

    TP_NOCOPYMOVE(ReferenceStateService);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnActivate(const ActivateEvent& acEvent) noexcept;
    void OnBearTrap(const BearTrapEvent& acEvent) noexcept;
    void OnNotifyReferenceStates(const NotifyReferenceStates& acMessage) noexcept;
    void OnStageChanged(uint32_t aFormId) noexcept;
    //! This game's own cell reset brought aFormId back as new.
    void NoteReset(uint32_t aFormId) noexcept;

    BSTEventResult OnEvent(const TESDestructionStageChangedEvent* acpEvent, const EventDispatcher<TESDestructionStageChangedEvent>*) override;
    //! A save was loaded: what this game tracked belongs to the world before it (ForgetTracked, at the next update).
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    //! Forgets what this game tracked and was waiting to send: on a disconnect, and after a load.
    void ForgetTracked() noexcept;

    void Send(const TESObjectREFR* apRef, ReferenceStates::Kind aKind, uint8_t aValue) noexcept;
    void ApplyUsed(uint32_t aFormId) noexcept;
    void WatchRunes() noexcept;
    ReferenceStates::Look LookOf(uint32_t aFormId) noexcept;
    bool IsOreVein(TESObjectREFR* apRef) noexcept;
    bool IsSharedDestructible(TESObjectREFR* apRef) noexcept;
    ActivationCatchUp::Plugin PluginOf(uint32_t aFormId) noexcept;

    World& m_world;
    TransportService& m_transport;

    ReferenceStates::BearTraps m_bearTraps;
    ReferenceStates::StageSettle m_stages;
    ReferenceStates::Pending m_pending;
    ReferenceStates::UseWatch m_uses;
    ReferenceStates::Resets m_resets;
    ReferenceStates::SendPacer<RequestReferenceState> m_outbox;
    //! Destructibles brought to another game's stage, by when: their stage change is that game's, not sent back.
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_settled;
    std::chrono::steady_clock::time_point m_nextWatch{};
    bool m_pluginsLooked{};
    //! Set by the load event, which may come on another thread; taken at the start of the next update.
    std::atomic<bool> m_saveLoaded{false};
    uint32_t m_dragonbornPrefix{0x100};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_bearTrapConnection;
    entt::scoped_connection m_notifyConnection;
};
