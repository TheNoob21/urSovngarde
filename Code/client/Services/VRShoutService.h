#pragma once

#include <VRShout.h>

struct World;
struct UpdateEvent;
struct ActionEvent;
struct SpellCastEvent;
struct DisconnectedEvent;

/**
 * @brief A VR player's shouts play as real shouts on the other games' copies of that player.
 *
 * The VR game casts a shout, and its effects already reach the other games, but the VR body performs no voice action, so the
 * copies stood still while the shout went off. Each shout this game's player casts (a dragon shout or a beast's howl, never a
 * power) is sent as the two voice actions a flat player's game performs for it (VRShout), timed at the cast: the copy shouts.
 * When the game performed a voice action for the shout itself, nothing more is sent.
 *
 * Only while connected; a shout from before a disconnect is dropped. No new message: these are ordinary actions.
 *
 * VR only.
 */
struct VRShoutService
{
    VRShoutService(World& aWorld, entt::dispatcher& aDispatcher);
    ~VRShoutService() noexcept = default;

    TP_NOCOPYMOVE(VRShoutService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnActionEvent(const ActionEvent& acEvent) noexcept;
    void OnSpellCastEvent(const SpellCastEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

private:
    void Send(uint32_t aActionId, std::string_view acEventName, uint64_t aTick) noexcept;

    World& m_world;

    //! The shout cast waiting to be sent (0: none), and when the last one was shown.
    uint64_t m_pendingTick{};
    uint64_t m_shownTick{};
    //! The release, sent on the frame after the start.
    bool m_releaseNext{};
    uint64_t m_releaseTick{};
    //! The newest voice action the game performed for this player itself.
    uint64_t m_ownTick{};
    //! The actions this service sent, so that they are not taken for the game's own.
    TiltedPhoques::Vector<std::pair<uint32_t, uint64_t>> m_sent;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_spellCastConnection;
    entt::scoped_connection m_disconnectedConnection;
};
