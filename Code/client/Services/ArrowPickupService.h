#pragma once

#include <ArrowPickup.h>

struct World;
struct TransportService;
struct ArrowPickupEvent;
struct NotifyArrowPickup;
struct UpdateEvent;
struct TESObjectCELL;

/**
 * @brief An arrow stuck in a wall or the ground goes in every game when one player takes it (ArrowPickup).
 *
 * Every arrow is replayed as a real arrow in the other games, so each game has its own copy stuck where it landed. This
 * game's player taking one (the game accepted it and the arrow left the world) is sent as its projectile, cell and place.
 * Another player's is answered by removing this game's copy: a shot (temporary) arrow of the same projectile, not with an
 * actor or a body, nearest that place within 30 units; indoors in that cell, outdoors in the cells of that worldspace around
 * the place. A copy removed here is never sent back. Arrows in bodies are StuckArrows'.
 */
struct ArrowPickupService
{
    ArrowPickupService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~ArrowPickupService() noexcept = default;

    TP_NOCOPYMOVE(ArrowPickupService);

    //! Whether an arrow at this place is with an actor or a body in these cells (ArrowPickup::IsWithActor).
    static bool IsWithActor(const TiltedPhoques::Vector<TESObjectCELL*>& acCells, float aX, float aY, float aZ) noexcept;

    //! The loaded cells an arrow at this place in aCell may lie in: aCell indoors; outdoors the attached cells of its worldspace
    //! under the place and around it.
    static TiltedPhoques::Vector<TESObjectCELL*> CellsAround(TESObjectCELL* apCell, float aX, float aY) noexcept;

protected:
    void OnArrowPickupEvent(const ArrowPickupEvent& acEvent) noexcept;
    void OnNotifyArrowPickup(const NotifyArrowPickup& acMessage) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

private:
    //! A copy that may be the taken arrow, as first read.
    struct PendingRef
    {
        uint32_t FormId{};
        float X{};
        float Y{};
        float Z{};
        ArrowPickup::Candidate Candidate{};
    };
    //! Another player's pick-up, waiting for the second read of its candidates (ArrowPickup::RestReadDue).
    struct Pending
    {
        uint64_t FirstReadMs{};
        uint32_t ProjectileId{};
        TiltedPhoques::Vector<PendingRef> Refs;
    };

    //! Removes the nearest candidate that did not move since its first read, if any.
    void Decide(Pending& aPending) noexcept;

    World& m_world;
    TransportService& m_transport;
    //! Copies removed for another player's pick-up, never sent back (a few, the oldest forgotten first).
    TiltedPhoques::Vector<uint32_t> m_removedHere;
    TiltedPhoques::Vector<Pending> m_pending;

    entt::scoped_connection m_eventConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_updateConnection;
};
