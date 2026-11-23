#include <TiltedOnlinePCH.h>

#include <Services/RagdollPoseService.h>
#include <Services/TransportService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/RequestRagdollPose.h>
#include <Messages/NotifyRagdollPose.h>

#include <Actor.h>
#include <Games/ActorExtension.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>

#include <mutex>
#include <unordered_map>

using namespace std::chrono_literals;
using RagdollBodiesPose::Body;
using Clock = std::chrono::steady_clock;

namespace
{
// A body this game runs, as last read, waiting for the update to send it.
struct OwnBodies
{
    Clock::time_point ReadAt{};
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    glm::vec3 Root{};
    std::vector<Body> Bodies;
    uint32_t Layout{};
};

struct RemoteBodies
{
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    RagdollBodiesPose::Track Poses;

    bool Matches(const CopyBodyRules::CopyAuthority& acAuthority) const noexcept
    {
        return acAuthority.Valid() && acAuthority.Owner == OwnershipEpoch && (acAuthority.Entity >> 32) == ServerId;
    }
};

// A dead creature's bodies at rest, sent with its spawn before its copy's ragdoll was there (VRBodySync::PlaceCorpse).
struct HeldBodies
{
    uint32_t Layout{};
    uint32_t Count{};
    std::vector<Body> Bodies;
    bool Handed{}; // handed to the copy's settle
};

// All are filled and read on the client's update, and the copies' poses are also read between frames (VRBodySync).
std::mutex s_lock;
std::unordered_map<uint32_t, OwnBodies> s_own;
std::unordered_map<uint32_t, RemoteBodies> s_tracks;
std::unordered_map<uint32_t, HeldBodies> s_rest;

bool RememberRemote(uint32_t aForm, uint32_t aId, uint32_t aEpoch, uint64_t aTick, uint32_t aLayout, uint32_t aTotal, std::vector<Body> aBodies)
{
    std::scoped_lock lock(s_lock);
    if (s_tracks.size() >= 64 && s_tracks.find(aForm) == s_tracks.end())
        return false;
    auto& track = s_tracks[aForm];
    if (track.ServerId != aId || track.OwnershipEpoch != aEpoch)
        track = RemoteBodies{aId, aEpoch};
    track.Poses.Push(aTick, aLayout, aTotal, std::move(aBodies), Clock::now());
    return true;
}

void HoldRest(const uint32_t aFormId, const uint32_t aLayout, const uint32_t aCount, const std::vector<Body>& acBodies)
{
    std::scoped_lock lock(s_lock);
    const auto it = s_rest.find(aFormId);
    // Sent again while the copy already lies so.
    if (it != s_rest.end() && it->second.Handed && it->second.Layout == aLayout && it->second.Count == aCount && it->second.Bodies.size() == acBodies.size() &&
        !RagdollBodiesPose::ArticulationChanged(it->second.Bodies, acBodies))
        return;
    if (it == s_rest.end() && s_rest.size() >= 64)
    {
        std::erase_if(s_rest, [](const auto& acEntry) { return acEntry.second.Handed; });
        if (s_rest.size() >= 64)
            return;
    }
    s_rest[aFormId] = HeldBodies{aLayout, aCount, acBodies, false};
}

bool Said(TiltedPhoques::Map<uint32_t, Clock::time_point>& aSaidAt, const uint32_t aFormId, const Clock::time_point aNow) noexcept
{
    if (aSaidAt.size() > 256)
        aSaidAt.clear();
    auto& saidAt = aSaidAt[aFormId];
    if (saidAt != Clock::time_point{} && aNow - saidAt < 10s)
        return true;
    saidAt = aNow;
    return false;
}
} // namespace

uint32_t RagdollPoseService::Present(uint32_t aId, uint32_t aEpoch, uint64_t aTick, uint32_t aLayout, uint32_t aTotal, const UrGameBody* acpData, uint32_t aCount)
{
    if (!acpData || !aId || !aEpoch || !aTick || !aLayout || !aCount || aCount > RagdollPose::kMaxBodies || aCount > aTotal || aTotal > CarriedBodyFollow::kMaxBodies)
        return 0;
    auto* actor = Utils::GetByServerId<Actor>(aId);
    const auto* extension = actor ? actor->GetExtension() : nullptr;
    const auto owner = actor ? Utils::GetRemoteOwnershipToken(actor->formID) : std::nullopt;
    if (!extension || !extension->IsRemote() || extension->IsPlayer() || !owner || owner->ServerId != aId || owner->OwnershipEpoch != aEpoch)
        return 0;
    std::vector<Body> bodies;
    bodies.reserve(aCount);
    for (uint32_t i = 0; i < aCount; ++i)
    {
        const auto& b = acpData[i];
        Body body{{b.Position[0], b.Position[1], b.Position[2]}, {b.Rotation[3], b.Rotation[0], b.Rotation[1], b.Rotation[2]}};
        const auto norm = glm::dot(body.Rotation, body.Rotation);
        if (!std::isfinite(body.Position.x) || !std::isfinite(body.Position.y) || !std::isfinite(body.Position.z) || !std::isfinite(norm) || std::abs(norm - 1.f) > .01f)
            return 0;
        bodies.push_back(body);
    }
    return RememberRemote(actor->formID, aId, aEpoch, aTick, aLayout, aTotal, std::move(bodies));
}

RagdollPoseService::RagdollPoseService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&RagdollPoseService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&RagdollPoseService::OnDisconnected>(this);
    m_notifyConnection = aDispatcher.sink<NotifyRagdollPose>().connect<&RagdollPoseService::OnNotifyRagdollPose>(this);
}

void RagdollPoseService::NoteOwnBodies(
    const uint32_t aFormId, const uint32_t aServerId, const uint32_t aOwnershipEpoch, const glm::vec3& acRoot, std::vector<Body> aBodies, const uint32_t aLayout) noexcept
{
    if (!aOwnershipEpoch || aBodies.empty())
        return;
    std::scoped_lock lock(s_lock);
    if (s_own.size() >= 64 && s_own.find(aFormId) == s_own.end())
        return;
    s_own[aFormId] = OwnBodies{Clock::now(), aServerId, aOwnershipEpoch, acRoot, std::move(aBodies), aLayout};
}

bool RagdollPoseService::HasOwnerBodies(const uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority) noexcept
{
    std::scoped_lock lock(s_lock);
    const auto it = s_tracks.find(aFormId);
    return it != s_tracks.end() && it->second.Matches(acAuthority) && it->second.Poses.Fresh(Clock::now());
}

bool RagdollPoseService::OwnerBodies(
    const uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority, const size_t aCount, const uint32_t aLayout, std::vector<Body>& aOut) noexcept
{
    const auto cNow = Clock::now();
    std::scoped_lock lock(s_lock);
    const auto it = s_tracks.find(aFormId);
    if (it == s_tracks.end() || !it->second.Matches(acAuthority))
        return false;
    if (it->second.Poses.BodiesAt(cNow, aCount, aLayout, aOut))
        return true;
    static TiltedPhoques::Map<uint32_t, Clock::time_point> s_saidAt;
    if (it->second.Poses.Fresh(cNow) && !Said(s_saidAt, aFormId, cNow))
        spdlog::debug("RagdollPose: {:X}'s ragdoll here ({} bodies) is not the one its owner sends; moved whole instead", aFormId, aCount);
    return false;
}

bool RagdollPoseService::HoldsRestBodies(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_lock);
    const auto it = s_rest.find(aFormId);
    return it != s_rest.end() && !it->second.Handed;
}

void RagdollPoseService::HandRestBodies(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_lock);
    if (const auto it = s_rest.find(aFormId); it != s_rest.end())
        it->second.Handed = true;
}

void RagdollPoseService::ForgetRestBodies(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_lock);
    s_rest.erase(aFormId);
}

void RagdollPoseService::ForgetAllRestBodies() noexcept
{
    std::scoped_lock lock(s_lock);
    s_rest.clear();
}

bool RagdollPoseService::RestBodies(const uint32_t aFormId, const size_t aCount, const uint32_t aLayout, std::vector<Body>& aOut) noexcept
{
    std::scoped_lock lock(s_lock);
    const auto it = s_rest.find(aFormId);
    if (it == s_rest.end() || !RagdollBodiesPose::Fits(it->second.Count, it->second.Layout, it->second.Bodies.size(), aCount, aLayout))
        return false;
    aOut = it->second.Bodies;
    return true;
}

void RagdollPoseService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto cNow = Clock::now();
    std::vector<std::pair<uint32_t, OwnBodies>> due;
    {
        std::scoped_lock lock(s_lock);
        // Copies whose owners stopped sending long ago.
        for (auto it = s_tracks.begin(); it != s_tracks.end();)
            it = cNow - it->second.Poses.NewestAt() > 10s ? s_tracks.erase(it) : std::next(it);
        if (!m_transport.IsConnected())
        {
            s_own.clear();
            return;
        }
        for (auto it = s_own.begin(); it != s_own.end();)
        {
            const auto cSent = m_sentAt.find(it->first);
            if (!RagdollBodiesPose::SendDue(cSent != m_sentAt.end() ? cSent->second : Clock::time_point{}, cNow))
            {
                ++it;
                continue;
            }
            due.emplace_back(it->first, std::move(it->second));
            it = s_own.erase(it);
        }
    }
    if (due.empty())
        return;

    if (m_sentAt.size() > 256)
        m_sentAt.clear();
    static TiltedPhoques::Map<uint32_t, Clock::time_point> s_saidAt;
    const auto cView = m_world.view<FormIdComponent, LocalComponent>();
    for (const auto& [cFormId, cOwn] : due)
    {
        // Another game runs it now.
        const auto cEntity = Utils::FindByFormId(cView, cFormId);
        if (!cEntity)
            continue;
        const auto& local = cView.get<LocalComponent>(*cEntity);
        if (local.Id != cOwn.ServerId || local.OwnershipEpoch != cOwn.OwnershipEpoch)
            continue;
        RequestRagdollPose request{};
        request.ServerId = cOwn.ServerId;
        request.OwnershipEpoch = cOwn.OwnershipEpoch;
        const auto cTick = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cOwn.ReadAt.time_since_epoch()).count());
        if (!RagdollBodiesPose::Encode(cTick, cOwn.Layout, cOwn.Root, cOwn.Bodies, request.Pose))
            continue;
        // Like movement: a lost one is not sent again, the next replaces it.
        if (!m_transport.Send(request, TiltedPhoques::kUnreliable))
            continue;
        m_sentAt[cFormId] = cNow;
        if (!Said(s_saidAt, cFormId, cNow))
            spdlog::debug("RagdollPose: {:X} is moved here; {} of its {} ragdoll bodies sent to the other players", cFormId, request.Pose.Bodies.size(), cOwn.Bodies.size());
    }
}

void RagdollPoseService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    std::scoped_lock lock(s_lock);
    s_own.clear();
    s_tracks.clear();
    s_rest.clear();
    m_sentAt.clear();
}

void RagdollPoseService::OnNotifyRagdollPose(const NotifyRagdollPose& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
    // Only a copy: what this game runs moves with its own physics.
    if (!pExtension || !pExtension->IsRemote() || pExtension->IsPlayer())
        return;
    const auto cOwner = Utils::GetRemoteOwnershipToken(pActor->formID);
    if (!cOwner || !acMessage.OwnershipEpoch || cOwner->ServerId != acMessage.ServerId || cOwner->OwnershipEpoch != acMessage.OwnershipEpoch)
        return;
    std::vector<Body> bodies;
    if (!RagdollBodiesPose::Decode(acMessage.Pose, bodies) || bodies.empty())
        return;
    // A dead creature's bodies are also held for its copy, whatever its track makes of them: those sent with its spawn come before
    // the copy's ragdoll does (VRBodySync::PlaceCorpse).
    const auto cEntity = m_world.FindByServerId(ServerIdIndex::Kind::kRemote, acMessage.ServerId);
    if (const auto* pRemote = cEntity ? m_world.try_get<RemoteComponent>(*cEntity) : nullptr; pRemote && pRemote->DeadAtOwner)
        HoldRest(pActor->formID, acMessage.Pose.Layout, acMessage.Pose.BodyCount, bodies);
    RememberRemote(pActor->formID, acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.Pose.Tick, acMessage.Pose.Layout, acMessage.Pose.BodyCount, std::move(bodies));
}
