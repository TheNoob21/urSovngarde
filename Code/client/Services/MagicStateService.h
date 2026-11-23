#pragma once

#include <Messages/NotifySpellCast.h>
#include <Structs/GameId.h>
#include <Structs/MagicState.h>

#include <chrono>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct SpellCastEvent;
struct InterruptCastEvent;
struct NotifyInterruptCast;
struct NotifyMagicState;
struct MagicItem;

/**
 * @brief Other players see this player's cloak, Oakflesh or held spell even when they arrive after it was cast.
 *
 * A player's own lasting effects and held spells reach the other games live, as an effect and as a cast. A player who comes
 * into range later, or whose copy of the caster is made after the cast, got neither. This game also says what its player has
 * now (MagicStates): the server keeps it and sends it with the character, and the other games put on their copy what it
 * lacks, for the time it has left, and end what its owner no longer has. A cast that came before the copy's body is cast once
 * the body is there. The copy's effects only show: what a cloak does to others is decided by the caster's game.
 * A hand charging a spell is said at once, and the copy's hand glows with the spell's casting art until the charge ends.
 */
struct MagicStateService
{
    MagicStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~MagicStateService() noexcept = default;

    TP_NOCOPYMOVE(MagicStateService);

    //! A shrine's or a standing stone's blessing: it stays with the player who used it, and is never sent.
    [[nodiscard]] static bool IsPersonalBlessing(const MagicItem* apSpell) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnSpellCastEvent(const SpellCastEvent& acEvent) noexcept;
    void OnInterruptCastEvent(const InterruptCastEvent& acEvent) noexcept;
    void OnNotifySpellCast(const NotifySpellCast& acMessage) noexcept;
    void OnNotifyInterruptCast(const NotifyInterruptCast& acMessage) noexcept;
    void OnNotifyMagicState(const NotifyMagicState& acMessage) noexcept;

private:
    struct Hand
    {
        GameId Spell{};
        bool IsDualCasting{};
    };

    //! What this game knows of another player's magic, by their character's server id.
    struct Copy
    {
        MagicState State{};
        std::chrono::steady_clock::time_point StateAt{};
        //! A new state came: the copy's effects it does not name end.
        bool EndMissing{};
        //! Something is still to be done to the copy once its body is there.
        bool Pending{};
        //! What the owner casts from each hand, and what the copy is casting here.
        Hand Wanted[2]{};
        GameId CastingHere[2]{};
        //! Other casts that came before the copy did, with when they came.
        TiltedPhoques::Vector<std::pair<std::chrono::steady_clock::time_point, NotifySpellCast>> Early{};
        std::chrono::steady_clock::time_point LastSeen{};
        //! The spell each hand of the copy glows with here, when its glow ends, and the body it was played on.
        GameId GlowingHere[2]{};
        std::chrono::steady_clock::time_point GlowUntil[2]{};
        const void* pGlowBody{};
    };

    void ReadOwnCharges(GameId (&aCharges)[2]) const noexcept;
    void SendOwnState() noexcept;
    void UpdateCopies() noexcept;
    void ApplyEffects(uint32_t aServerId, Copy& aCopy) noexcept;
    void ApplyCasts(uint32_t aServerId, Copy& aCopy) noexcept;
    void ApplyCharges(uint32_t aServerId, Copy& aCopy) noexcept;
    //! Plays again, every frame, a copy's glow about to run out (MagicStates::kGlowRenewMs).
    void RenewDueGlows(std::chrono::steady_clock::time_point aNow) noexcept;
    static void StopGlows(Copy& aCopy) noexcept;
    void ClearCopies() noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    Hand m_ownCasts[2]{};
    //! The spell each of the player's hands charges, as last read.
    GameId m_ownCharges[2]{};
    MagicState m_sent{};
    bool m_sentBefore{};
    std::chrono::steady_clock::time_point m_sentAt{};
    std::chrono::steady_clock::time_point m_readAt{};
    //! Set while this service hands a cast to MagicService, so its own handlers leave it alone.
    bool m_replaying{};

    TiltedPhoques::Map<uint32_t, Copy> m_copies;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_spellCastEventConnection;
    entt::scoped_connection m_interruptCastEventConnection;
    entt::scoped_connection m_notifySpellCastConnection;
    entt::scoped_connection m_notifyInterruptCastConnection;
    entt::scoped_connection m_notifyMagicStateConnection;
};
