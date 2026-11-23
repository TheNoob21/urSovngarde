#pragma once

#include <Events/PacketEvent.h>
#include <PlayerFurniture.h>

struct World;
struct Player;
struct RequestNpcFurniture;
struct RequestPlayerFurniture;
struct CharacterRemoveEvent;
struct UpdateEvent;

/**
 * @brief Passes on where NPCs sit or sleep, from the game that runs them to the other games near them.
 */
struct FurnitureService
{
    FurnitureService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~FurnitureService() noexcept = default;

    TP_NOCOPYMOVE(FurnitureService);

    bool IsSeated(uint32_t aActor) const noexcept;
    void SendPlayerSeat(Player* apViewer, entt::entity aEntity) const noexcept;

protected:
    void OnNpcFurniture(const PacketEvent<RequestNpcFurniture>& acMessage) noexcept;
    void OnPlayerFurniture(const PacketEvent<RequestPlayerFurniture>& acMessage) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnCharacterRemove(const CharacterRemoveEvent& acEvent) noexcept;

private:
    void RelayPlayerSeat(const PlayerFurniture::Claim& acClaim, bool aReleased) const noexcept;

    World& m_world;
    PlayerFurniture::Claims m_playerSeats;
    uint64_t m_nextSeatCheck{};

    entt::scoped_connection m_npcFurnitureConnection;
    entt::scoped_connection m_playerFurnitureConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_characterRemoveConnection;
};
