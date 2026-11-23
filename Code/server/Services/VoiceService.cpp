#include <Services/VoiceService.h>

#include <GameServer.h>
#include <VoiceRelayRules.h>
#include <Events/PlayerLeaveEvent.h>
#include <World.h>
#include <Components.h>

#include <Messages/NotifyVoiceData.h>
#include <Messages/VoiceDataRequest.h>
#include <Messages/VoiceStateRequest.h>

namespace
{
// A little past the 30 m where the client fades a voice out, so it fades rather than cuts off.
constexpr float kHearingRange = 2400.f;
} // namespace

VoiceService::VoiceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_voiceStateConnection(aDispatcher.sink<PacketEvent<VoiceStateRequest>>().connect<&VoiceService::OnVoiceState>(this))
    , m_voiceDataConnection(aDispatcher.sink<PacketEvent<VoiceDataRequest>>().connect<&VoiceService::OnVoiceData>(this))
    , m_leaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&VoiceService::OnPlayerLeave>(this))
{
}

void VoiceService::OnVoiceState(const PacketEvent<VoiceStateRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetVoiceEnabled(acMessage.Packet.Enabled);
    // Debug: a game can send this as often as it likes, so it must not fill the log.
    spdlog::debug("Voice: {} turned voice {}", acMessage.pPlayer->GetUsername().c_str(), acMessage.Packet.Enabled ? "on" : "off");
}

void VoiceService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_budgets.erase(acEvent.pPlayer->GetId());
}

void VoiceService::OnVoiceData(const PacketEvent<VoiceDataRequest>& acMessage) noexcept
{
    Player* pSpeaker = acMessage.pPlayer;
    const VoiceDataRequest& packet = acMessage.Packet;
    if (!pSpeaker->IsVoiceEnabled() || packet.Data.empty())
        return;

    // Pieces of a size Steam makes, and as many a second as it makes (VoiceRelayRules): each goes to every listener.
    if (!m_budgets[pSpeaker->GetId()].Allow(std::chrono::steady_clock::now(), packet.Data.size()))
        return;

    NotifyVoiceData notify{};
    notify.PlayerId = pSpeaker->GetId();
    notify.Sequence = packet.Sequence;
    notify.Data = packet.Data;

    for (Player* pListener : m_world.GetPlayerManager())
    {
        if (pListener != pSpeaker && pListener->IsVoiceEnabled() && CanHear(*pListener, *pSpeaker))
            pListener->SendUnreliable(notify);
    }
}

bool VoiceService::CanHear(const Player& acListener, const Player& acSpeaker) const noexcept
{
    // The party channel: heard at any distance.
    const auto& speakerParty = acSpeaker.GetParty().JoinedPartyId;
    if (speakerParty.has_value() && acListener.GetParty().JoinedPartyId == speakerParty)
        return true;

    // Proximity: the same area, then the distance between both characters.
    if (!acSpeaker.GetStandingCell().IsInRange(acListener.GetCellComponent(), false))
        return false;

    const auto cSpeakerCharacter = acSpeaker.GetCharacter();
    const auto cListenerCharacter = acListener.GetCharacter();
    if (!cSpeakerCharacter || !cListenerCharacter)
        return false;

    const auto* pSpeakerMovement = m_world.try_get<MovementComponent>(*cSpeakerCharacter);
    const auto* pListenerMovement = m_world.try_get<MovementComponent>(*cListenerCharacter);
    if (!pSpeakerMovement || !pListenerMovement)
        return false;

    return glm::distance(pSpeakerMovement->Position, pListenerMovement->Position) <= kHearingRange;
}
