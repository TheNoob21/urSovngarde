#pragma once

#include <MountSync.h>
#include <Messages/NotifyMount.h>

#include <utility>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct MountEvent;
struct NotifyDismount;
struct ActivateEvent;
struct Actor;

/**
 * @brief A rider on a horse in one game sits on the same horse in the others, and gets off when it does.
 *
 * The rider's game sends what it reads back: on the horse (again every few seconds while it stays on, for games that load
 * the pair later), and off it, for whatever reason. The other games put their copy of the rider on their copy of the horse,
 * and take it off again; whether the copy is seated is also read back, never assumed from the order.
 */
struct MountService
{
    MountService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~MountService() noexcept = default;

    TP_NOCOPYMOVE(MountService);

    //! At the start of a frame, on the main thread: the copies are put on and taken off their horses there (CopyWork.h).
    static void RunQueuedWork() noexcept;

    //! A rider copy the disconnect is about to delete: if this game put it on a horse, it is taken off at the start of the next frame
    //! and deleted after that, never before. True when this takes the deletion over.
    static bool ReleaseBeforeDelete(uint32_t aRiderServerId, Actor* apRider) noexcept;
    //! A deletion ReleaseBeforeDelete took over is still to be done on the copy with this handle.
    static bool DeletionPending(uint32_t aHandle) noexcept;

    //! Whether copies' rides are seated here (and their climbs played here, not replayed from their owners' actions).
    static bool TakesClimbs() noexcept;
    //! A copy replayed its owner's climb onto a horse: its next seat climbs on, whatever the word says.
    static void NoteOwnerClimb(uint32_t aRiderFormId) noexcept;

    //! A horse this game treats as its own player's too (party horses) is written to a save with the owner it had here. Only the
    //! horse the share was made for is lifted, and the share is given back only if it is still the one in place.
    struct ShareSaveGuard
    {
        explicit ShareSaveGuard(Actor* apActor) noexcept;
        ~ShareSaveGuard() noexcept;
        TP_NOCOPYMOVE(ShareSaveGuard);

    private:
        Actor* m_pActor{};
        uint32_t m_handle{};
        uint32_t m_share{};
        bool m_lifted{};
    };

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnMountEvent(const MountEvent& acEvent) noexcept;
    void OnNotifyMount(const NotifyMount& acMessage) noexcept;
    void OnNotifyDismount(const NotifyDismount& acMessage) noexcept;
    //! This player getting on a horse a party member's game runs: the horse is asked for, and got on once it is this game's.
    void OnActivate(const ActivateEvent& acEvent) noexcept;
    //! A rider or horse got a new owner, or lost or gained its copy: what was decided for its ride is withdrawn.
    void OnOwnerChanged(entt::registry& aRegistry, entt::entity aEntity) noexcept;

private:
    //! One of this game's riders, by the rider's form id.
    struct RiderWatch
    {
        uint32_t MountFormId{};
        //! Already on its horse when this game took it over: the first word is a refresh, the other copies do not climb.
        bool AlreadyOn{};
        MountSync::Watch State{};
        //! What was last sent, so that the dismount names the same pair.
        uint32_t RiderServerId{};
        uint32_t RiderOwnershipEpoch{};
        uint32_t MountServerId{};
    };

    //! A horse a party member rode here: that member's player id, the share (its ticket) and the horse it was made for, by handle.
    struct SharedHorse
    {
        uint32_t PlayerId{};
        uint32_t Share{};
        uint32_t Handle{};
    };

    //! Another game's rider on its copy here, by the rider's server id.
    struct CopySeat
    {
        uint32_t MountServerId{};
        MountSync::Seat State{};
        //! The epochs the ride was told under and the actors here it was tried on.
        MountSync::Binding Bound{};
        //! The decision the copy's queued work belongs to (CopyWork::Tickets).
        uint32_t Ticket{};
        //! The rider's form id here, for when its entity is gone (a disconnect clears it before this service hears of it).
        uint32_t RiderFormId{};
        //! This ride, as the main thread knows the one it put in place: taking the copy off names it.
        uint32_t Ride{};
    };

    //! A copy shown climbing off its horse, by form id.
    struct Leaving
    {
        double Since{};
        uint32_t RiderServerId{};
        uint32_t RiderHandle{};
        uint32_t Ride{};
        //! The horse, by server id.
        uint32_t MountServerId{};
    };

    void UpdateWatches(double aNow) noexcept;
    //! Gives a watch to each of this game's riders that reads as on a horse with none: a connect, a reconnect or a load while
    //! riding tells of no ride (the mount package does, and only while connected).
    void WatchRidersAlreadyOn(double aNow) noexcept;
    //! The horse this rider is on, or 0 (MountSync::HorseRiddenHere).
    uint32_t RiddenHorseNear(const Actor* apRider) noexcept;
    void UpdateSeats(double aNow) noexcept;
    void UpdateLeaving(double aNow) noexcept;
    //! Seats a copy as a word says. False when the rider or the horse has no copy here yet.
    bool TakeMountWord(const NotifyMount& acMessage) noexcept;
    void UpdatePendingWords(double aNow) noexcept;
    void UpdateRideClaims(double aNow) noexcept;
    void UpdateSharedHorses() noexcept;
    //! Lets every copy this game shows on the horse with this server id off it at the start of the next frame: still climbing off, or
    //! seated by a word whose dismount never came. True when there was one.
    bool LetCopiesOff(uint32_t aMountServerId) noexcept;
    //! What the main thread did since the last update.
    void TakeDoneWork(double aNow) noexcept;
    //! Takes a copy off the ride aRide at the start of the next frame, if that ride is still the one in place (aSessionOver: after a
    //! disconnect, when it is no copy any more).
    static void QueueRelease(uint32_t aRiderServerId, uint32_t aRiderFormId, uint32_t aRiderHandle, uint32_t aRide, bool aSessionOver = false) noexcept;
    //! Marks a rider's copy as seated on aMount for the interpolation (entt::null: not seated).
    void MarkSeated(entt::entity aRider, entt::entity aMount) noexcept;
    void SendLegacyMount(uint32_t aRiderFormId, uint32_t aMountFormId) const noexcept;

    World& m_world;
    TransportService& m_transport;

    TiltedPhoques::Map<uint32_t, RiderWatch> m_watches;
    TiltedPhoques::Map<uint32_t, CopySeat> m_seats;
    TiltedPhoques::Map<uint32_t, Leaving> m_leaving;
    //! Words whose rider or horse had no copy here yet, by rider server id, and since when.
    TiltedPhoques::Map<uint32_t, std::pair<NotifyMount, double>> m_pendingWords;
    //! Horses a party member's game runs that this player is getting on, by form id, until their Ride is done or dropped.
    MountSync::RideClaims m_rideClaims;
    //! Horses a party member rode here, by form id: this game treats them as its player's too.
    TiltedPhoques::Map<uint32_t, SharedHorse> m_sharedHorses;
    double m_nextWatchAt{};
    double m_nextAlreadyOnAt{};
    //! Something is getting on or off: looked at every frame until it is done.
    bool m_everyFrame{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_notifyMountConnection;
    entt::scoped_connection m_notifyDismountConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_remoteAddedConnection;
    entt::scoped_connection m_remoteChangedConnection;
    entt::scoped_connection m_remoteRemovedConnection;
};
