#pragma once

#include <Events/PacketEvent.h>
#include <VoiceRelayRules.h>

struct World;
struct VoiceDataRequest;
struct VoiceStateRequest;
struct PlayerLeaveEvent;

/**
 * @brief Relays each voice piece, unreliable, to nearby players with voice on and to the speaker's party.
 */
struct VoiceService
{
    VoiceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~VoiceService() noexcept = default;

    TP_NOCOPYMOVE(VoiceService);

protected:
    void OnVoiceState(const PacketEvent<VoiceStateRequest>& acMessage) const noexcept;
    void OnVoiceData(const PacketEvent<VoiceDataRequest>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

private:
    bool CanHear(const Player& acListener, const Player& acSpeaker) const noexcept;

    World& m_world;
    // Pieces and bytes per speaker this second; past the limit the rest of the second is dropped (VoiceRelayRules).
    Map<uint32_t, VoiceRelayRules::Budget> m_budgets;

    entt::scoped_connection m_voiceStateConnection;
    entt::scoped_connection m_voiceDataConnection;
    entt::scoped_connection m_leaveConnection;
};
