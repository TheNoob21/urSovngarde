#pragma once

#include <NpcFurniture.h>
#include <PlayerFurniture.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyNpcFurniture;
struct NotifyPlayerFurniture;
struct DisconnectedEvent;
struct Actor;
struct TESObjectREFR;

/**
 * @brief NPCs at work spots: sitting, sleeping, working a forge or a counter, on every screen.
 *
 * The package that seats an NPC runs only in the game that runs the NPC (its owner): copies run no AI. This game reads
 * where each of its own NPCs is settled in furniture and tells the other games every 250 ms when it changes. Their copies
 * are then put in that furniture by the game's own sit/sleep state change, and taken out of it when the owner's NPC gets up.
 * The way in and out is not played here: the owner's animation replay plays it.
 */
struct FurnitureService
{
    FurnitureService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~FurnitureService() noexcept = default;

    TP_NOCOPYMOVE(FurnitureService);

    //! At the start of a frame, on the main thread: the copies are seated and stood there (CopyWork.h).
    static void RunQueuedWork() noexcept;

    //! For the character save: while it lives, a copy this game seated is written standing in no furniture (NpcFurniture::SeatSaveMask),
    //! and its seat is put back when it goes, however the save is left.
    struct SeatSaveGuard
    {
        explicit SeatSaveGuard(Actor* apActor) noexcept;
        ~SeatSaveGuard() noexcept;

        SeatSaveGuard(const SeatSaveGuard&) = delete;
        SeatSaveGuard& operator=(const SeatSaveGuard&) = delete;

    private:
        Actor* m_pActor;
        NpcFurniture::SeatSaveMask m_mask;
    };

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyNpcFurniture(const NotifyNpcFurniture& acMessage) noexcept;
    void OnNotifyPlayerFurniture(const NotifyPlayerFurniture& acMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    //! A copy got a new owner, or became this game's: the seat decided for it is withdrawn.
    void OnOwnerChanged(entt::registry& aRegistry, entt::entity aEntity) noexcept;

private:
    struct PlayerSeat
    {
        uint32_t Actor{};
        uint32_t Epoch{};
        GameId Cell{};
        PlayerFurniture::Seat Where{};
        uint64_t Sequence{};
        uint64_t Lease{};
        uint64_t SendAt{};
        uint8_t SentState{};
        bool Denied{};
    };

    void ReadOwnPlayerSeat(uint64_t aNowMs) noexcept;
    void SendPlayerSeat(PlayerFurniture::Action aAction, uint8_t aState, uint64_t aNowMs) noexcept;
    void QueueDeniedSeat(Actor* apPlayer, const TESObjectREFR* apFurniture, uint32_t aLoadingScreens, uint64_t aNowMs) noexcept;
    void HandlePlayerSeatReply(const NotifyPlayerFurniture& acMessage) noexcept;
    void ClearPlayerSeat(bool aSendExit) noexcept;
    void ReadCopySeat(entt::entity aEntity, uint32_t aActor, uint32_t aEpoch, const NpcFurniture::Seat& acWhere, uint64_t aNowMs, uint64_t aLease = 0, GameId aCell = {}) noexcept;
    void ReadOwnSeats(uint64_t aNowMs) noexcept;
    void UpdateCopySeats(uint64_t aNowMs) noexcept;
    //! Seats or stands one copy as its owner said; false when its word is done with.
    bool ApplyCopySeat(entt::entity aEntity, Actor* apCopy, uint64_t aNowMs) noexcept;
    //! What the main thread did since the last round.
    void TakeDoneWork(uint64_t aNowMs) noexcept;
    //! Drops a copy's word, and any seat decided under it that the main thread has not done yet.
    void Forget(entt::entity aEntity) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_npcFurnitureConnection;
    entt::scoped_connection m_playerFurnitureConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_remoteAddedConnection;
    entt::scoped_connection m_remoteChangedConnection;
    entt::scoped_connection m_remoteRemovedConnection;

    NpcFurniture::Outbox m_seatOutbox;
    PlayerSeat m_playerSeat;
    uint64_t m_playerSequence{};
    //! The seats and stands handed to the main thread, numbered for all copies at once.
    NpcFurniture::WorkSerials m_workSerials;
    uint64_t m_nextSeatRead{};
};
