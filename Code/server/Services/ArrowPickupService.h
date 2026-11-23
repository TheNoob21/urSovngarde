#pragma once

#include <ArrowPickup.h>
#include <Events/PacketEvent.h>

struct World;
struct PlayerLeaveEvent;
struct RequestArrowPickup;

/**
 * @brief Relays an arrow a player took out of a wall or the ground to the other players near them, so their games remove
 * their own copy of it (ArrowPickup). A pick-up is checked first: a usable place, within an arm's reach of where the player's
 * character was last heard, at most eight a second. Nothing is kept: a player who comes later never had those arrows.
 */
struct ArrowPickupService
{
    ArrowPickupService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ArrowPickupService() noexcept = default;

    TP_NOCOPYMOVE(ArrowPickupService);

protected:
    void OnArrowPickup(const PacketEvent<RequestArrowPickup>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

private:
    World& m_world;
    TiltedPhoques::Map<uint32_t, ArrowPickup::RateGate> m_rates;

    entt::scoped_connection m_pickupConnection;
    entt::scoped_connection m_leaveConnection;
};
