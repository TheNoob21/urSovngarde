#pragma once

#include <Events/EventDispatcher.h>

#include <Structs/Inventory.h>
#include <Messages/NotifyDroppedItem.h>
#include <DroppedItemIdentity.h>
#include <Services/DroppedItemReplies.h>

#include <Services/HeldObjectPlayback.h>
#include <Services/WorldObjectBodies.h>
#include <Services/WorldObjectWatch.h>

struct World;
struct TransportService;
struct ConnectedEvent;
struct DisconnectedEvent;
struct CellChangeEvent;
struct ItemDroppedEvent;
struct ItemPickedUpEvent;
struct NotifyDroppedItem;
struct NotifyDroppedItemRemoved;
struct NotifyDroppedItemMove;
struct NotifyWorldObjectMove;
struct NotifyPropBodies;
struct UpdateEvent;
struct TESObjectREFR;
struct TESObjectCELL;

/**
 * @brief Keeps items players drop lying in every player's world, including items dropped in earlier sessions.
 *
 * Reports local drops and pick-ups to the server, and adopts the server's items if already here or places them.
 * Passes on the movement of dropped items and of placed world objects (a bottle, a cart), and the swing of their hinged bodies (a shop sign).
 * Also announces older drops, read from the game's ExtraDroppedItemList on the player.
 */
class DroppedItemService
{
public:
    DroppedItemService(World&, entt::dispatcher&, TransportService&);

private:
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnItemDropped(const ItemDroppedEvent&) noexcept;
    void OnItemPickedUp(const ItemPickedUpEvent&) noexcept;
    void OnNotifyDroppedItem(const NotifyDroppedItem&) noexcept;
    void RemovePickedUpItem(uint32_t aId, const Inventory::Entry& acItem) noexcept;
    void OnNotifyDroppedItemRemoved(const NotifyDroppedItemRemoved&) noexcept;
    void OnNotifyDroppedItemMove(const NotifyDroppedItemMove&) noexcept;
    void OnNotifyWorldObjectMove(const NotifyWorldObjectMove&) noexcept;
    void OnNotifyPropBodies(const NotifyPropBodies&) noexcept;
    //! A dropped item starting to move here, or a move of one sent in a blast, this pass. Judged with the placed objects'
    //! starts, so a blast that knocks items about shares the objects' budget.
    struct ItemSend
    {
        uint32_t Id{};
        uint32_t RefFormId{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        glm::mat3 Turn{1.f};
        bool Start{};
    };
    void UpdateWorldObjects(std::chrono::steady_clock::time_point aNow, const std::vector<ItemSend>& acItemSends) noexcept;
    void SendItemMove(uint32_t aId, const glm::vec3& acPosition, const glm::vec3& acRotation, bool aAtRest, bool aStart, bool aFixed = false) noexcept;
    void ShowHeldObjects(std::chrono::steady_clock::time_point aNow) noexcept;
    //! Every frame: the hinged bodies another player swings, driven toward how they lie there.
    void DrivePropBodies() noexcept;
    //! Whether a placed object hangs bodies that are watched, known once per base form.
    bool HasPropBodies(TESObjectREFR* apRef) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;

    //! Send a reference that lies on the floor to the server. False when its place cannot be named to the server.
    //! Each request id comes back with the reply to this add alone.
    bool SendAdd(TESObjectREFR* apRef, const Inventory::Entry& acItem, bool aAnnouncement) noexcept;
    void AnnounceOldDrops() noexcept;
    //! The reference standing for a mapped item.
    TESObjectREFR* RefOf(uint32_t aRefFormId) const noexcept;

    void MapServerIdToRef(uint32_t aId, uint32_t aRefFormId) noexcept;

    World& m_world;
    TransportService& m_transport;

    //! Server id to the reference that stands for it here, and back.
    std::unordered_map<uint32_t, uint32_t> m_refById;
    std::unordered_map<uint32_t, uint32_t> m_idByRef;

    //! The hands of the other players' copies, a frame ahead of where their 3D has them (HeldObject::HandLead), by hand key.
    std::unordered_map<uint64_t, HeldObject::HandLead> m_handLeads;

    //! This player's hands (VR) or what the grab key holds (flat), read once a pass (ReadHolds).
    struct Holds
    {
        HeldObject::Carry::Hands Hands{};
        uint32_t GrabbedFormId{};
    };
    Holds m_holds{};

    //! This player's own drops and announcements, waiting for the id the server gives them.
    using Pending = DroppedItemIdentity::Pending;
    std::vector<Pending> m_pending;
    std::vector<DroppedItemReplies::AwaitingScene> m_awaitingItems;
    std::unordered_map<uint32_t, NotifyDroppedItem> m_deferredItems;
    //! Local pickups awaiting the server's removal, including pickups before their add reply.
    std::unordered_set<uint32_t> m_removingItems;
    bool m_retryDeferred{};

    //! How each remembered item has been moving here, by server id.
    //! Only one side sends an item's position at a time, so an item moved by the other side is held (keyframed) here.
    struct Motion
    {
        glm::vec3 LastPosition{};
        glm::vec3 LastRotation{};
        //! The 3D's rotation where LastPosition was taken.
        //! Its Euler angles can wrap around or lag the 3D, so a move or a restart is judged on this.
        glm::mat3 LastTurn{1.f};
        std::chrono::steady_clock::time_point LastLocalMove{};
        std::chrono::steady_clock::time_point LastRemote{};
        std::chrono::steady_clock::time_point QuietUntil{};
        //! The item's base form when it was mapped.
        //! The game reuses a deleted item's FF id for another object, which must not be moved: it could be an actor.
        uint32_t BaseFormId{};
        uint32_t RefHandle{};
        bool LegacyItemData{};
        bool Moving{};
        bool Held{};
        //! Measurement: the other side's moves applied while held, and how many of them rebuilt the object's 3D.
        uint32_t HeldMoves{};
        uint32_t HeldRebuilds{};
        //! Where the other side last put it, to compare with where its 3D is drawn when it is let go.
        glm::vec3 RemoteTarget{};
        //! The other side's last update said it was put down.
        //! Still held for a moment, since a still hand sends the same.
        bool RemoteAtRest{};
        //! Its owner's script holds it still where it was put (a weapon rack's weapon): kept out of physics here too.
        bool RemoteFixed{};
        //! To be put exactly where it was left and held still once its 3D is there.
        bool FixPending{};
        //! Held still here for another game at some point: its stillness is not this game's script's, so never sent as Fixed.
        bool WasRemoteFixed{};
        //! This game's own drop: its own script holds it, so the server's copy of that is not taken back.
        bool OwnDrop{};
        //! Its 3D was there on the last pass. A cell loading settles every item in it, which is not a move.
        bool Loaded{};
        //! A 3D load in a fresh drop's first two seconds is part of the throw, not a cell reload.
        std::chrono::steady_clock::time_point FreshDropUntil{};
        //! When a placed object was first watched: many starting soon after is its cell settling.
        std::chrono::steady_clock::time_point FirstSeen{};
        //! Part of a blast until then: sent by the nearest player's game, a few at a time.
        std::chrono::steady_clock::time_point BlastUntil{};
        //! Sent from here as part of a blast: its moves share the blast's budget until it rests.
        bool Blast{};
        //! When a blast left it to another player's game, while that may still be claimed (WorldObjectWatch::ClaimsLeftObject).
        std::chrono::steady_clock::time_point LeftAt{};
        //! The newest of the other side's updates taken, so a late one cannot undo a later one.
        HeldObject::Order RemoteOrder{};
        //! While held for the other side and shown on their clock: their latest updates, the
        //! hand it sits in, and where it was drawn last frame.
        HeldObject::Playback Shown{};
        HeldObject::Timeline Line{};
        HeldObject::Delay ShownDelay{};
        HeldObject::Grip InHand{};
        HeldObject::Pose Drawn{};
        bool HasDrawn{};
        //! The player moving it over there, as the server named them (0: not named).
        uint32_t MoverPlayerId{};
        //! A throw from here: whether a hand of this player carries it or let go of it, how it flies, and where and when it was
        //! polled last.
        HeldObject::Carry OwnCarry{};
        HeldObject::Release OwnRelease{};
        HeldObject::Flight OwnFlight{};
        HeldObject::Pose PolledPose{};
        double PolledAt{-1.0};
        //! A throw over there: handed to physics here once its let-go is shown; and the moment of their line shown last.
        HeldObject::Launch RemoteThrow{};
        //! Caught here by a hand of this player's while it flew after that throw.
        HeldObject::CatchHold Caught{};
        double ShownAt{};
        bool HasShownAt{};
        //! A placed object's hinged bodies, as this game swings them, or as another player's are driven here.
        WorldObjectBodies::Track Bodies{};
        WorldObjectBodies::Driven BodiesDriven{};
        uint32_t BodiesLayout{};

        void StopShowing() noexcept
        {
            Shown.Clear();
            Line = {};
            ShownDelay = {};
            InHand = {};
            HasDrawn = false;
            HasShownAt = false;
        }
    };
    void PutAtNewest(TESObjectREFR* apRef, Motion& aMotion, std::chrono::steady_clock::time_point aNow) noexcept;
    //! Another player's move of an object held here, as a throw: Skip while it flies here, and one that takes it back from
    //! its flight is held again from where it is.
    HeldObject::Launch::Step NoteThrow(
        TESObjectREFR* apRef, Motion& aMotion, std::chrono::steady_clock::time_point aNow, uint64_t aTick, bool aThrown, const HeldObject::Flight& acFlight, bool aAtRest,
        const glm::vec3& acPosition) noexcept;
    //! Hands an object another player threw to physics here, with the flight they let it go at.
    void LaunchHere(TESObjectREFR* apRef, const Motion& acMotion, const char* acpKind, uint32_t aId) noexcept;
    //! Stops following an object another player threw, caught here in its flight: this game moves it from now on.
    void CatchHere(const TESObjectREFR* apRef, Motion& aMotion, std::chrono::steady_clock::time_point aNow, const char* acpKind, uint32_t aId) noexcept;
    //! What this player's hands hold this pass: a moving object they let go of is sent as thrown.
    void ReadHolds() noexcept;
    //! A poll of whether this player holds the object now: the grab key's (flat), or a hand it moves with (VR).
    bool PollHeld(const TESObjectREFR* apRef, Motion& aMotion, const glm::vec3& acPosition) noexcept;
    //! A poll of an object this game moves: whether it is thrown now, and how it flies.
    void NoteFlight(TESObjectREFR* apRef, Motion& aMotion, const glm::vec3& acPosition, const glm::mat3& acTurn, double aNow, bool aHeld, bool aMoving) noexcept;
    //! A RemoteFixed item put exactly where it was left and held still there, once it has 3D.
    void HoldFixed(TESObjectREFR* apRef, Motion& aMotion) noexcept;
    bool Follow(
        TESObjectREFR* apRef, Motion& aMotion, std::chrono::steady_clock::time_point aNow, const glm::vec3& acPosition, const glm::vec3& acRotation, bool aAtRest,
        uint64_t aTick) noexcept;
    std::unordered_map<uint32_t, Motion> m_motion;

    //! Objects placed in the world near the players, by reference id, watched for moves here or held for another player.
    //! The list is rebuilt once a second from the player's cell and, outdoors, the attached cells around it, nearest first.
    std::unordered_map<uint32_t, Motion> m_objectMotion;
    std::vector<uint32_t> m_objectCandidates;
    std::chrono::steady_clock::time_point m_candidatesAt{};
    //! Whether each base form's mesh hangs bodies, once its 3D has told, and when to ask again one that could not tell yet.
    std::unordered_map<uint32_t, bool> m_hasBodiesByBase;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_bodiesAskAgainAt;
    //! First looks at a mesh this pass: each walks its 3D checking every pointer, so only a few are made at once.
    uint32_t m_bodyProbes{};
    //! The moves sent this second, which a blast's sends share.
    WorldObjectWatch::Budget m_moveBudget;

    //! Old drops already announced this session, so a cell change does not send them again.
    std::unordered_set<uint32_t> m_announced;
    //! What the player dropped while not connected (reference and item), announced once connected.
    //! The game's own dropped-item list is not found on the player, so the client keeps this one.
    std::vector<std::pair<uint32_t, Inventory::Entry>> m_droppedOffline;
    //! Never reset, so replies from an earlier connection cannot match another add.
    uint32_t m_addRequests{};
    //! Whether this connection has logged what it found on the player.
    bool m_reportedOldDrops{false};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_droppedConnection;
    entt::scoped_connection m_pickedUpConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_removedConnection;
    entt::scoped_connection m_moveConnection;
    entt::scoped_connection m_objectMoveConnection;
    entt::scoped_connection m_propBodiesConnection;
    entt::scoped_connection m_updateConnection;
};
