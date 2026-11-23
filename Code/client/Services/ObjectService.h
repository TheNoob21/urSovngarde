#pragma once

#include <Events/EventDispatcher.h>

#include <atomic>
#include <Games/Events.h>
#include <Services/Generic/VendorChest.h>
#include <Services/ChestSelection.h>
#include <Structs/Inventory.h>
#include <TrapSync.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct ActivateEvent;
struct TransportService;
struct NotifyActivate;
struct LockChangeEvent;
struct NotifyLockChange;
struct CellChangeEvent;
struct ScriptAnimationEvent;
struct AssignObjectsResponse;
struct NotifyScriptAnimation;
struct ReferenceEnableEvent;
struct NotifyReferenceEnable;
struct ScriptDoorEvent;
struct UpdateEvent;
struct NotifyObjectInventoryChanges;
struct TrapFireEvent;
struct NotifyTrapFire;
struct TESObjectCELL;

/**
 * @brief Handles objects in the environment.
 */
class ObjectService final : public BSTEventSink<TESActivateEvent>, public BSTEventSink<TESTriggerEnterEvent>, public BSTEventSink<TESTriggerLeaveEvent>
{
public:
    ObjectService(World&, entt::dispatcher&, TransportService&);

    // The leader's switch of a placed actor another plugin handed over, for every game of the party.
    void SwitchActor(uint32_t aFormId, bool aEnabled) noexcept;

    //! This game sent a quest step of its own: a member's next switches are shared, as after a lever (SharedHomes::SharesSwitch).
    //! Any thread: the quest event that calls it may come from a script thread.
    void NoteOwnQuestStep() noexcept { m_ownStepAt.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed); }
    //! Another game's quest step was applied here: what its scripts switch now is that game's to send. Any thread.
    void NoteQuestReplay() noexcept { m_questReplayAt.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed); }

    //! A player home (a NoResetZone cell): its chests and mannequins stay each player's own unless homes are shared whole.
    [[nodiscard]] static bool IsPlayerHome(const TESObjectCELL* apCell) noexcept;

private:
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnAssignObjectsResponse(const AssignObjectsResponse&) noexcept;
    void OnActivate(const ActivateEvent&) noexcept;
    void OnActivateNotify(const NotifyActivate&) noexcept;
    void OnLockChange(const LockChangeEvent&) noexcept;
    void OnLockChangeNotify(const NotifyLockChange&) noexcept;
    void OnScriptAnimationEvent(const ScriptAnimationEvent&) noexcept;
    void OnNotifyScriptAnimation(const NotifyScriptAnimation&) noexcept;
    void OnReferenceEnableEvent(const ReferenceEnableEvent&) noexcept;
    void OnNotifyReferenceEnable(const NotifyReferenceEnable&) noexcept;
    void OnScriptDoorEvent(const ScriptDoorEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool ApplyReferenceEnable(uint32_t aFormId, bool aEnabled) noexcept;
    // Whether this game shares the switches its scripts make now (SharedHomes::SharesSwitch).
    [[nodiscard]] bool SharesSwitch() const noexcept;
    void OnNotifyObjectInventoryChanges(const NotifyObjectInventoryChanges&) noexcept;
    bool IsObjectShared(uint32_t aFormId) const noexcept;
    void RunVendorChest() noexcept;
    void ShareVendorChest(uint32_t aChestId) noexcept;
    void SendWholeChest(uint32_t aChestId) noexcept;
    // Traps (TrapSync): one this game's actor set off goes to the other games, which fire their own.
    void OnTrapFire(const TrapFireEvent&) noexcept;
    void OnTrapFireNotify(const NotifyTrapFire&) noexcept;
    // Tells the trap ledger at once, from the game's thread, whether this game's actor or a copy touched the trigger.
    static void NoteTouchInLedger(uint32_t aTriggerId, const TESObjectREFR* apActionRef) noexcept;
    void NoteTriggerTouched(const TESObjectREFR* apTrigger, const TESObjectREFR* apActionRef) noexcept;

    BSTEventResult OnEvent(const TESActivateEvent*, const EventDispatcher<TESActivateEvent>*) override;
    BSTEventResult OnEvent(const TESTriggerEnterEvent*, const EventDispatcher<TESTriggerEnterEvent>*) override;
    BSTEventResult OnEvent(const TESTriggerLeaveEvent*, const EventDispatcher<TESTriggerLeaveEvent>*) override;

    entt::entity CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept;
    // Destroys the entities of objects no longer in the announced cells: the server forgets those objects.
    void ForgetObjectsLeft() noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_onActivateConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_lockChangeNotifyConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_scriptAnimationNotifyConnection;
    entt::scoped_connection m_referenceEnableConnection;
    entt::scoped_connection m_referenceEnableNotifyConnection;
    entt::scoped_connection m_scriptDoorConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_objectInventoryConnection;
    entt::scoped_connection m_trapFireConnection;
    entt::scoped_connection m_trapFireNotifyConnection;

    // References this game's scripts switched and shares, sent at the next update: form id and enabled, in order.
    Vector<std::pair<uint32_t, bool>> m_enableChanges;
    // Switches applied here from another game, by form id: the game's report of them is not sent back.
    Map<uint32_t, bool> m_appliedSwitches;
    // When this game's player last used an activator, and when another player's use was last replayed here; unset for never.
    std::chrono::steady_clock::time_point m_ownUseAt{};
    std::chrono::steady_clock::time_point m_replayAt{};
    // When this game last sent a quest step of its own, and when another game's step was last applied here: steady clock ticks, 0 for never.
    std::atomic<int64_t> m_ownStepAt{0};
    std::atomic<int64_t> m_questReplayAt{0};
    // The leader's switches for references not in this game yet (their cell not loaded), tried again every 2 s.
    Map<uint32_t, bool> m_enableWaiting;
    double m_enableRetry = 0.0;
    // Doors this game's scripts moved and did not share, by form id.
    // The state the server keeps follows only uses of a door, so it does not know of these.
    Set<uint32_t> m_doorsMovedHere;
    // Locks set here as another game had them, by form id, and when: the game's lock change they cause is not sent back.
    Map<uint32_t, std::pair<bool, std::chrono::steady_clock::time_point>> m_lockFromOtherGame;
    // The trade with the merchant spoken to, and its chest as this game last set or sent it.
    VendorChest::Trade m_vendorTrade;
    Inventory m_vendorChestSeen;
    // The merchant chest the server answered for in this trade.
    uint32_t m_vendorChestAnswered{};
    // Trap triggers this game's actors set off lately, and the traps fired here lately.
    TrapSync::Recent m_traps;
    // The containers and doors announced to the server while they stay in the cells around the player.
    ChestSelection::Announced m_announcedObjects;
};
