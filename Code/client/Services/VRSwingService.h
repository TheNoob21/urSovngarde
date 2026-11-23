#pragma once

#include <VRSwing.h>

struct World;
struct UpdateEvent;
struct ActionEvent;
struct DisconnectedEvent;

/**
 * @brief A VR player's swings, power attacks and bashes play as real attacks on the other games' copies of that player.
 *
 * The VR game marks a swing in the player's attack state and picks the attack's data, but performs no attack action, so the
 * copies only moved their arms (through the body pose). Each new swing or bash is sent as the attack actions a flat
 * player's game would perform for it (VRSwing::StepsFor), timed at the swing: the copy plays the attack, with its sound,
 * and the NPCs around it see an attack. The damage stays this game's report, as before.
 *
 * Only while connected and out of a loading screen: a swing, a bash's second step or a raised block from before a
 * disconnect or a load is dropped, never sent into the next session.
 *
 * VR only.
 */
struct VRSwingService
{
    VRSwingService(World& aWorld, entt::dispatcher& aDispatcher);
    ~VRSwingService() noexcept = default;

    TP_NOCOPYMOVE(VRSwingService);

    //! A melee hit this game's player landed (aHand 0 right, 1 left; aFlags as the hit event's), reported by a physics mod's add-on:
    //! shown as the swing it was when the game marked no swing for it. Any thread; taken on the next update.
    static void ReportHit(uint32_t aHand, uint32_t aFlags) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnActionEvent(const ActionEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

private:
    //! Forgets the swing in progress, a bash's step still to send and the actions sent so far.
    void Reset() noexcept;
    void Send(uint32_t aActionId, std::string_view acEventName, uint64_t aTick) noexcept;

    struct Pending
    {
        VRSwing::Kind Kind{VRSwing::Kind::None};
        uint64_t Tick{};
        TiltedPhoques::String Event{};
        bool Blocking{};
        //! Taken from a hit an add-on reported, not from the attack state.
        bool FromHit{};
    };

    World& m_world;

    uint32_t m_lastState{};
    //! Bow and crossbow steps logged this session (TEMPORARY, for the crossbow reload).
    uint32_t m_bowStepsLogged{};
    //! The loading screens queued when the swing in progress was seen (UI::LoadingScreensQueued).
    uint32_t m_loads{};
    //! A swing seen, sent once the game has had a moment to perform an attack action of its own for it.
    Pending m_pending{};
    //! The second step of a bash, sent on the next frame.
    VRSwing::Step m_nextStep{};
    bool m_hasNextStep{};
    //! A block raised only for a bash, lowered when the bash ends.
    bool m_raisedBlock{};
    //! A block the player holds up (IsBlocking) was shown raised, by this service or the game's own action.
    bool m_heldBlock{};
    //! By hand (0 right, 1 left, 2 both), when an attack was last shown, so that a hit reported for it shows no second swing.
    std::array<uint64_t, 3> m_shownAt{};
    //! The same, for swings shown from a hit only: the attack state the game marks for that swing after it is not sent again.
    std::array<uint64_t, 3> m_hitShownAt{};
    uint64_t m_raisedAt{};
    //! The newest attack action the game performed for this player itself: its hand and tick.
    int m_ownHand{-1};
    uint64_t m_ownTick{};
    //! The actions this service sent, so that they are not taken for the game's own.
    TiltedPhoques::Vector<std::pair<uint32_t, uint64_t>> m_sent;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_disconnectedConnection;
};
