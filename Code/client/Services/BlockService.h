#pragma once

#include <PvpBlock.h>

#include <optional>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyHealthChangeBroadcast;
struct NotifyClash;
struct Actor;

/**
 * @brief A flat player's block in the defender's rule, and the clashes a flat game hears.
 *
 * In VR another player's blow is stopped when this player's blade or shield met that player's weapon (VRBodySync, the defender's rule
 * in ActorValueService). A flat game has no hands to meet anything with: its player holds the block, as against anyone else.
 * A blow that comes while the block is up (PvpBlock) is weighed by the game's own numbers for a melee hit (HitData): the game says
 * whether the block takes it, how much of it the block takes and how hard it staggers. The rest of the blow is taken here.
 * The one who struck is told (ClashRequest): in VR they hear it and feel it in the hand holding the weapon.
 * Another player's clash is heard here too. Flat builds only.
 */
struct BlockService
{
    BlockService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~BlockService() noexcept = default;

    TP_NOCOPYMOVE(BlockService);

    //! Whether a flat block counts here: flat builds.
    //! The defender's rule holds another player's blows on flat only then (ActorValueService::OnHealthChangeBroadcast).
    static bool Active() noexcept;

    //! What the game makes of a melee blow (HitData::Populate).
    struct GameBlow
    {
        float TotalDamage{};
        //! How much of the blow the target's block takes, 0 to 1.
        float PercentBlocked{};
        float Stagger{};
        uint32_t Flags{};
    };

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) noexcept;
    void OnNotifyClash(const NotifyClash& acMessage) const noexcept;

private:
    //! Another player's blow on this player, waiting to be seen here.
    struct HeldBlow
    {
        //! This player's server id, as the blow named it.
        uint32_t Id{};
        uint32_t AttackerPlayerId{};
        uint64_t Tick{};
        float Damage{};
        //! The game's numbers for it, taken while the block was up as it came.
        std::optional<GameBlow> Game{};
    };

    void Decide(uint64_t aNow, bool aBlocking) noexcept;
    void Blocked(const HeldBlow& acBlow, Actor* apPlayer, Actor* apCopy, uint64_t aSeen) noexcept;
    void SendBlocked(const Actor* apPlayer, const Actor* apCopy, uint64_t aSeen, bool aHeardHere) noexcept;

    World& m_world;
    TransportService& m_transport;

    PvpBlock::History m_history;
    TiltedPhoques::Vector<HeldBlow> m_blows;
    bool m_wasBlocking = false;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_healthConnection;
    entt::scoped_connection m_clashConnection;
};
