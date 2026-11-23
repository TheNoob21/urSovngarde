#include <TiltedOnlinePCH.h>

#include <Services/DetectionService.h>
#include <Services/TransportService.h>
#include <Services/PartyService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Messages/RequestDetectionLevels.h>
#include <Messages/NotifyDetectionLevels.h>

#include <Games/ActorExtension.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>
#include <Helpers/PerfScope.h>

#include <algorithm>
#include <atomic>
#include <memory>

using DetectionLevels::Band;

namespace
{
using Snapshot = TiltedPhoques::Map<uint32_t, DetectionLevels::Heard>;

// Words per message, under what a game takes in one (DetectionWords::kMax): a heartbeat in a crowded town can have more.
constexpr size_t kWordsPerMessage = 400;

// What the game's threads read: replaced whole by the main thread, never changed in place.
std::atomic<std::shared_ptr<const Snapshot>> s_snapshot;

uint64_t NowMs() noexcept
{
    return GetTickCount64();
}

// Near enough for the owner to read: the same interior, or the same world space, within DetectionLevels::kRange.
bool Near(Actor* apNpc, Actor* apOther) noexcept
{
    if (!apNpc->parentCell || !apOther->parentCell)
        return false;
    const TESWorldSpace* pWorldSpace = apNpc->GetWorldSpace();
    if (pWorldSpace != apOther->GetWorldSpace() || (!pWorldSpace && apNpc->parentCell != apOther->parentCell))
        return false;
    const float cX = apNpc->position.x - apOther->position.x;
    const float cY = apNpc->position.y - apOther->position.y;
    const float cZ = apNpc->position.z - apOther->position.z;
    return cX * cX + cY * cY + cZ * cZ <= DetectionLevels::kRange * DetectionLevels::kRange;
}
} // namespace

DetectionService::DetectionService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DetectionService::OnUpdate>(this);
    m_notifyConnection = aDispatcher.sink<NotifyDetectionLevels>().connect<&DetectionService::OnNotifyDetectionLevels>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&DetectionService::OnDisconnected>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&DetectionService::OnPartyLeft>(this);
}

int32_t DetectionService::CopyLevel(const uint32_t aCopyFormId) noexcept
{
    const std::shared_ptr<const Snapshot> cSnapshot = s_snapshot.load(std::memory_order_acquire);
    const DetectionLevels::Heard* pHeard = nullptr;
    if (cSnapshot)
    {
        if (const auto cIt = cSnapshot->find(aCopyFormId); cIt != cSnapshot->end())
            pHeard = &cIt->second;
    }
    return DetectionLevels::LevelOf(DetectionLevels::Answer(pHeard, NowMs()));
}

void DetectionService::OnUpdate(const UpdateEvent&) noexcept
{
    // The server passes words on only within a party.
    if (!m_transport.IsOnline() || !m_world.GetPartyService().IsInParty())
        return;

    const uint64_t cNow = NowMs();
    if (cNow < m_nextRead)
        return;
    m_nextRead = cNow + DetectionLevels::kReadEveryMs;

    ReadOwnNpcs(cNow);
    Prune(cNow);
}

// The owner's side: each NPC this game runs, against each other player's character near it.
void DetectionService::ReadOwnNpcs(const uint64_t aNowMs) noexcept
{
    struct Other
    {
        Actor* pActor;
        uint32_t PlayerId;
    };
    Vector<Other> others;
    const Vector<uint32_t>& cMembers = m_world.GetPartyService().GetPartyMembers();
    const auto cPlayerView = m_world.view<FormIdComponent, RemoteComponent, PlayerComponent>();
    for (const entt::entity cEntity : cPlayerView)
    {
        const uint32_t cPlayerId = cPlayerView.get<PlayerComponent>(cEntity).Id;
        if (std::find(cMembers.begin(), cMembers.end(), cPlayerId) == cMembers.end())
            continue;
        Actor* pActor = Cast<Actor>(TESForm::GetById(cPlayerView.get<FormIdComponent>(cEntity).Id));
        if (pActor && pActor->HasLoadedProcess() && !pActor->IsDisabled())
            others.push_back({pActor, cPlayerId});
    }

    if (!others.empty())
    {
        PerfScope perfScope("DetectionService::ReadOwnNpcs");

        struct Pair
        {
            Actor* pNpc;
            uint32_t ServerId;
            const Other* pOther;
        };
        Vector<Pair> pairs;
        const auto cNpcView = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<PlayerComponent>);
        for (const entt::entity cEntity : cNpcView)
        {
            Actor* pNpc = Cast<Actor>(TESForm::GetById(cNpcView.get<FormIdComponent>(cEntity).Id));
            const ActorExtension* pExtension = pNpc ? pNpc->GetExtension() : nullptr;
            if (!pNpc || pNpc == PlayerCharacter::Get() || !pExtension || pExtension->IsPlayer() || !pNpc->HasLoadedProcess() || pNpc->IsDisabled() || pNpc->IsDead())
                continue;

            const uint32_t cServerId = cNpcView.get<LocalComponent>(cEntity).Id;
            for (const Other& cOther : others)
            {
                if (Near(pNpc, cOther.pActor))
                    pairs.push_back({pNpc, cServerId, &cOther});
            }
        }

        // Read up to kPairsPerRound of them, from where the last round stopped; the others keep their last word.
        const size_t cCount = pairs.size();
        const size_t cRead = std::min(cCount, DetectionLevels::kPairsPerRound);
        const size_t cFirst = cCount ? m_readCursor % cCount : 0;
        for (size_t i = 0; i < cCount; ++i)
        {
            const Pair& cPair = pairs[(cFirst + i) % cCount];
            if (i < cRead)
                m_outbox.Put(cPair.ServerId, cPair.pOther->PlayerId, DetectionLevels::BandOf(cPair.pNpc->RequestDetectionLevel(cPair.pOther->pActor)));
            else
                m_outbox.Keep(cPair.ServerId, cPair.pOther->PlayerId);
        }
        m_readCursor = cCount ? (cFirst + cRead) % cCount : 0;
    }

    const std::vector<DetectionLevels::Word> cWords = m_outbox.Take(aNowMs);
    if (cWords.empty())
        return;

    for (size_t first = 0; first < cWords.size(); first += kWordsPerMessage)
    {
        RequestDetectionLevels request{};
        const size_t cLast = std::min(cWords.size(), first + kWordsPerMessage);
        request.Words.reserve(cLast - first);
        for (size_t i = first; i < cLast; ++i)
            request.Words.push_back({cWords[i].NpcId, cWords[i].PlayerId, static_cast<uint8_t>(cWords[i].Value)});
        m_transport.Send(request);
    }
}

void DetectionService::OnNotifyDetectionLevels(const NotifyDetectionLevels& acMessage) noexcept
{
    const uint64_t cNow = NowMs();
    for (const DetectionWord& cWord : acMessage.Words)
    {
        if (cWord.Band >= DetectionLevels::kBandCount)
            continue;

        const std::optional<entt::entity> cEntity = Utils::FindEntityByServerId(cWord.NpcId);
        if (!cEntity || m_world.all_of<PlayerComponent>(*cEntity))
            continue;
        const auto* pRemote = m_world.try_get<RemoteComponent>(*cEntity);
        const auto* pFormId = m_world.try_get<FormIdComponent>(*cEntity);
        if (!pRemote || !pFormId)
            continue;

        m_heard[pFormId->Id] = {{static_cast<Band>(cWord.Band), cNow}, *cEntity, cWord.NpcId, pRemote->OwnershipEpoch};
    }
    Publish();
}

// A word goes when it is old, when its copy is gone or has another owner, or when its form id now names another character.
void DetectionService::Prune(const uint64_t aNowMs) noexcept
{
    bool changed = false;
    for (auto it = m_heard.begin(); it != m_heard.end();)
    {
        const Entry& cEntry = it->second;
        bool keep = aNowMs - cEntry.Word.AtMs <= DetectionLevels::kExpireMs;
        if (keep)
        {
            const bool cValid = m_world.valid(cEntry.Entity);
            const auto* pRemote = cValid ? m_world.try_get<RemoteComponent>(cEntry.Entity) : nullptr;
            const auto* pFormId = cValid ? m_world.try_get<FormIdComponent>(cEntry.Entity) : nullptr;
            keep = pRemote && pFormId && pFormId->Id == it->first && pRemote->Id == cEntry.ServerId && pRemote->OwnershipEpoch == cEntry.OwnershipEpoch;
        }

        if (keep)
        {
            ++it;
            continue;
        }
        it = m_heard.erase(it);
        changed = true;
    }

    if (changed)
        Publish();
}

void DetectionService::Publish() noexcept
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->reserve(m_heard.size());
    for (const auto& [cFormId, cEntry] : m_heard)
        snapshot->emplace(cFormId, cEntry.Word);
    s_snapshot.store(std::move(snapshot), std::memory_order_release);
}

void DetectionService::Clear() noexcept
{
    m_heard.clear();
    m_outbox = {};
    m_readCursor = 0;
    s_snapshot.store(nullptr, std::memory_order_release);
}

void DetectionService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Clear();
}

void DetectionService::OnPartyLeft(const PartyLeftEvent&) noexcept
{
    Clear();
}
