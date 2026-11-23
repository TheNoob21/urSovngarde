#include <Services/ExtensionService.h>
#include <Services/ExtensionAreas.h>
#include <algorithm>
#include <World.h>
#include <Services/FurnitureService.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestExtension.h>

ExtensionService::ExtensionService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_store("extension_records")
    , m_request(aDispatcher.sink<PacketEvent<RequestExtension>>().connect<&ExtensionService::OnRequest>(this))
    , m_update(aDispatcher.sink<UpdateEvent>().connect<&ExtensionService::OnUpdate>(this))
    , m_leave(aDispatcher.sink<PlayerLeaveEvent>().connect<&ExtensionService::OnLeave>(this))
{
}

bool ExtensionService::Rate::Take(Clock::time_point aNow, size_t aSize, uint32_t aMax, size_t aMaxBytes)
{
    if (aNow - Start >= std::chrono::seconds(1))
        *this = {aNow};
    if (Messages >= aMax || Bytes + aSize > aMaxBytes)
        return false;
    ++Messages;
    Bytes += aSize;
    return true;
}

bool ExtensionService::WithinBudget(Player& aSender, const ExtensionData& acMessage, Clock::time_point aNow)
{
    auto& peer = m_peers[aSender.GetId()];
    if (!peer.Total.Take(aNow, acMessage.Bytes.size(), 512, 512 * 1024))
        return false;
    if (!peer.Addons.contains(acMessage.Addon) && peer.Addons.size() >= 32)
    {
        std::erase_if(peer.Addons, [&](const auto& acPair) { return aNow - acPair.second.Start >= std::chrono::seconds(1); });
        if (peer.Addons.size() >= 32)
            return false;
    }
    return peer.Addons[acMessage.Addon].Take(aNow, acMessage.Bytes.size(), 256, 256 * 1024);
}

bool ExtensionService::Authorize(Player& aPlayer, ExtensionData& aMessage) const
{
    const auto cCharacter = aPlayer.GetCharacter();
    if (!cCharacter || !aPlayer.GetCellComponent())
        return false;
    const auto cView = m_world.view<OwnerComponent, CellIdComponent, CharacterComponent>();
    const auto cSubject = aMessage.Subject ? static_cast<entt::entity>(aMessage.Subject) : *cCharacter;
    if (!cView.contains(cSubject))
        return false;
    const auto& cOwner = cView.get<OwnerComponent>(cSubject);
    const auto& cCell = cView.get<CellIdComponent>(cSubject);
    if (cOwner.GetOwner() != &aPlayer || !cOwner.OwnershipEpoch)
        return false;
    if (aMessage.Subject && aMessage.SubjectEpoch != cOwner.OwnershipEpoch)
        return false;
    if (!cCell || !aPlayer.GetCellComponent().IsInRange(cCell, cView.get<CharacterComponent>(cSubject).IsDragon()))
        return false;
    if (aMessage.Target)
    {
        const auto cTarget = static_cast<entt::entity>(aMessage.Target);
        if (!cView.contains(cTarget))
            return false;
        const auto& cTargetOwner = cView.get<OwnerComponent>(cTarget);
        const auto& cTargetCell = cView.get<CellIdComponent>(cTarget);
        const bool cDragon = cView.get<CharacterComponent>(cTarget).IsDragon();
        if (!cTargetOwner.GetOwner() || (aMessage.RequiresOwnedTarget() && cTargetOwner.GetOwner() != &aPlayer))
            return false;
        if (cTargetOwner.OwnershipEpoch != aMessage.TargetEpoch || !cTargetCell)
            return false;
        if (!cCell.IsInRange(cTargetCell, cDragon) || !aPlayer.GetCellComponent().IsInRange(cTargetCell, cDragon))
            return false;
    }
    if (aMessage.Scope == UR_EXTENSION_PARTY && !aPlayer.GetParty().JoinedPartyId)
        return false;
    aMessage.Subject = World::ToInteger(cSubject);
    aMessage.SubjectEpoch = cOwner.OwnershipEpoch;
    return true;
}

bool ExtensionService::Visible(const Player& acViewer, const ExtensionData& acMessage, std::optional<uint32_t> aParty) const
{
    const auto cPeer = m_peers.find(acViewer.GetId());
    if (cPeer == m_peers.end() || !cPeer->second.Subscriptions.contains(acMessage.Addon))
        return false;
    if (!acViewer.GetCharacter() || !acViewer.GetCellComponent())
        return false;
    if (acMessage.Scope == UR_EXTENSION_PARTY)
        return aParty && acViewer.GetParty().JoinedPartyId == aParty;
    const auto cView = m_world.view<OwnerComponent, CellIdComponent, CharacterComponent>();
    for (const auto cId : {acMessage.Subject, acMessage.Target})
    {
        if (!cId)
            continue;
        const auto cEntity = static_cast<entt::entity>(cId);
        if (!cView.contains(cEntity))
            return false;
        if (!acViewer.GetCellComponent().IsInRange(cView.get<CellIdComponent>(cEntity), cView.get<CharacterComponent>(cEntity).IsDragon()))
            return false;
    }
    return acMessage.Scope != UR_EXTENSION_OWNER || cView.get<OwnerComponent>(static_cast<entt::entity>(acMessage.Target)).GetOwner() == &acViewer;
}

bool ExtensionService::TakeLease(uint32_t aPlayer, const ExtensionData& acMessage, Clock::time_point aNow)
{
    const LeaseKey cKey{acMessage.Addon, acMessage.Target, acMessage.TargetEpoch, acMessage.Key};
    const auto cIt = m_leases.find(cKey);
    const bool cHeld = cIt != m_leases.end() && cIt->second.Until > aNow;
    if (cHeld && (cIt->second.Player != aPlayer || cIt->second.Subject != acMessage.Subject))
        return false;
    if (cHeld && cIt->second.Epoch != acMessage.SubjectEpoch)
        return false;
    if (acMessage.State == UR_EXTENSION_RELEASE)
    {
        if (cHeld)
            m_leases.erase(cIt);
        return cHeld;
    }
    if (!cHeld && m_leases.size() >= 1024)
        return false;
    m_leases.insert_or_assign(cKey, Lease{aPlayer, acMessage.Subject, acMessage.SubjectEpoch, aNow + std::chrono::milliseconds(750)});
    return true;
}

bool ExtensionService::Reserved(uint32_t aActor, uint32_t aExceptAddon) const
{
    const auto cIt = m_reservations.find(aActor);
    if (cIt == m_reservations.end() || cIt->second.Until <= Clock::now())
        return false;
    if (cIt->second.Message.Data.Addon == aExceptAddon)
        return false;
    const auto* pOwner = m_world.try_get<OwnerComponent>(static_cast<entt::entity>(aActor));
    if (!pOwner || !pOwner->GetOwner())
        return false;
    return pOwner->GetOwner()->GetId() == cIt->second.Message.Sender && pOwner->OwnershipEpoch == cIt->second.Message.Data.SubjectEpoch;
}

void ExtensionService::EndReservation(Retained& aState)
{
    auto& message = aState.Message;
    message.Data.State = UR_EXTENSION_UNRESERVE;
    message.Data.Bytes.clear();
    for (auto* pViewer : m_world.GetPlayerManager())
        if (aState.Delivered.contains(pViewer->GetId()) || message.Sender == pViewer->GetId())
            pViewer->Send(message);
}

// A game asks to hold or release an actor for an add-on.
void ExtensionService::OnReservation(Player& aSender, Retained& aState, Clock::time_point aNow)
{
    auto& message = aState.Message;
    const auto cId = message.Data.Subject;
    auto it = m_reservations.find(cId);
    if (it != m_reservations.end() && !Reserved(cId))
    {
        EndReservation(it->second);
        m_reservations.erase(it);
        it = m_reservations.end();
    }
    const bool cSame = it != m_reservations.end() && it->second.Message.Data.Addon == message.Data.Addon && it->second.Message.Data.Key == message.Data.Key &&
                       it->second.Message.Sender == aSender.GetId();
    bool accepted = cSame;
    if (message.Data.State == UR_EXTENSION_RESERVE && (cSame || it == m_reservations.end()))
    {
        const auto cEntity = static_cast<entt::entity>(cId);
        const auto& cCharacter = m_world.get<CharacterComponent>(cEntity);
        const auto cRides = m_world.view<RideComponent>();
        const bool cRidden = std::any_of(cRides.begin(), cRides.end(), [&](auto aRider) { return aRider == cEntity || cRides.get<RideComponent>(aRider).Ride.Mount == cId; });
        accepted = !cCharacter.IsDead() && !cRidden && !m_world.ctx().at<FurnitureService>().IsSeated(cId) && !m_world.GetCharacterService().IsHeld(cId) &&
                   std::none_of(
                       m_leases.begin(), m_leases.end(),
                       [&](const auto& acLease) { return acLease.second.Until > aNow && (std::get<1>(acLease.first) == cId || acLease.second.Subject == cId); }) &&
                   (cSame || m_reservations.size() < 512);
        if (accepted)
        {
            aState.Until = aNow + std::chrono::seconds(2);
            if (cSame)
                aState.Delivered = std::move(it->second.Delivered);
            auto& held = m_reservations.insert_or_assign(cId, aState).first->second;
            Relay(held, true);
        }
    }
    else if (cSame && message.Data.State == UR_EXTENSION_UNRESERVE)
    {
        EndReservation(it->second);
        m_reservations.erase(it);
    }
    message.Data.Result = accepted ? 1 : 2;
    aSender.Send(message);
}

void ExtensionService::Retire(const Retained& acState, Player* apViewer) const
{
    auto notice = acState.Message;
    notice.Data.Bytes.clear();
    notice.Data.State = notice.Data.IsReservation() ? UR_EXTENSION_UNRESERVE : notice.Data.RequiresOwnedTarget() ? UR_EXTENSION_ERASE_OWNED_TARGET : UR_EXTENSION_ERASE;
    if (apViewer)
        apViewer->Send(notice);
    else
        for (auto* pViewer : m_world.GetPlayerManager())
            if (acState.Delivered.contains(pViewer->GetId()))
                pViewer->Send(notice);
}

void ExtensionService::Relay(Retained& aState, bool aNew)
{
    auto& message = aState.Message;
    for (auto* pViewer : m_world.GetPlayerManager())
    {
        const auto cId = pViewer->GetId();
        if (cId == message.Sender && message.Data.Scope != UR_EXTENSION_OWNER)
            continue;
        if (!Visible(*pViewer, message.Data, aState.Party))
        {
            if (aState.Delivered.erase(cId))
                Retire(aState, pViewer);
            continue;
        }
        if (!aNew)
        {
            if (aState.Delivered.contains(cId))
                continue;
            auto& replays = m_peers.at(cId).Replays;
            if (replays >= 32)
                continue;
            ++replays;
        }
        pViewer->Send(message);
        if (message.Data.IsReplacement() || message.Data.State == UR_EXTENSION_RESERVE)
            aState.Delivered.insert(cId);
    }
}

// An add-on message from a game: subscribed, then routed by its kind.
void ExtensionService::OnRequest(const PacketEvent<RequestExtension>& acEvent)
{
    auto* pSender = acEvent.pPlayer;
    const auto cNow = Clock::now();
    const auto& cInput = acEvent.Packet.Data;
    if (!pSender || !cInput.ValidRequest() || !WithinBudget(*pSender, cInput, cNow))
        return;
    auto& peer = m_peers.at(pSender->GetId());
    if (cInput.State == ExtensionData::kSubscribe || cInput.State == ExtensionData::kResubscribe)
    {
        if (peer.Subscriptions.size() < 32 && peer.Subscriptions.insert(cInput.Addon).second)
            peer.Cell = {};
        else if (cInput.State == ExtensionData::kResubscribe && peer.Subscriptions.contains(cInput.Addon))
            peer.Refresh.try_emplace(cInput.Addon, std::max(cNow, peer.NextRefresh[cInput.Addon]));
        return;
    }
    if (!peer.Subscriptions.contains(cInput.Addon))
        return;

    Retained state;
    auto& message = state.Message;
    message.Data = cInput;
    if (!Authorize(*pSender, message.Data))
        return;
    if (cInput.IsDurable())
    {
        if (peer.Records.Take(cNow, cInput.Bytes.size(), 16, 64 * 1024))
            OnRecord(*pSender, cInput);
        return;
    }
    message.Sender = pSender->GetId();
    state.Party = pSender->GetParty().JoinedPartyId;
    if (cInput.IsReservation())
    {
        OnReservation(*pSender, state, cNow);
        return;
    }
    const auto cDeclineLease = [&]()
    {
        if (cInput.IsLease())
        {
            message.Data.Result = 2;
            message.Data.Bytes.clear();
            pSender->Send(message);
        }
    };
    if (!cInput.IsErase() && cInput.State != UR_EXTENSION_RELEASE && (Reserved(message.Data.Subject, cInput.Addon) || Reserved(message.Data.Target, cInput.Addon)))
    {
        cDeclineLease();
        return;
    }
    if (cInput.IsAreaState())
    {
        OnAreaState(*pSender, message.Data);
        return;
    }
    if (cInput.Scope == UR_EXTENSION_OWNER)
    {
        const auto* pOwner = m_world.get<OwnerComponent>(static_cast<entt::entity>(cInput.Target)).GetOwner();
        if (!pOwner || !Visible(*pOwner, message.Data, state.Party))
        {
            cDeclineLease();
            return;
        }
    }
    if (cInput.IsLease() && !TakeLease(message.Sender, message.Data, cNow))
    {
        cDeclineLease();
        return;
    }
    const StateKey cKey{cInput.Addon, message.Data.Subject, cInput.Key};
    if (cInput.IsReplacement())
    {
        if (!m_states.contains(cKey))
        {
            // For each sender and add-on, each sender and the whole server, so one cannot use up the room of the rest.
            size_t senderCount = 0, addonCount = 0;
            for (const auto& cEntry : m_states)
                if (cEntry.second.Message.Sender == message.Sender)
                {
                    ++senderCount;
                    addonCount += cEntry.second.Message.Data.Addon == cInput.Addon;
                }
            if (m_states.size() >= 1024 || addonCount >= 128 || senderCount >= 256)
                return;
        }
        if (const auto cOld = m_states.find(cKey); cOld != m_states.end())
        {
            const auto& cPrior = cOld->second.Message.Data;
            const bool cSameTarget = cPrior.Target == message.Data.Target && cPrior.TargetEpoch == message.Data.TargetEpoch;
            const bool cSameScope = cPrior.Scope == message.Data.Scope && (cPrior.Scope != UR_EXTENSION_PARTY || cOld->second.Party == state.Party);
            if (!cSameTarget || cPrior.SubjectEpoch != message.Data.SubjectEpoch || !cSameScope)
                Retire(cOld->second);
            else
                state.Delivered = std::move(cOld->second.Delivered);
        }
        auto& retained = m_states.insert_or_assign(cKey, std::move(state)).first->second;
        Relay(retained, true);
        return;
    }
    if (cInput.IsErase())
    {
        const auto cOld = m_states.find(cKey);
        if (cOld == m_states.end())
            return;
        Retire(cOld->second);
        m_states.erase(cOld);
        return;
    }
    Relay(state, true);
}

// A state an add-on left on a reference in the world, kept for whoever comes near.
void ExtensionService::OnAreaState(Player& aSender, const ExtensionData& acMessage)
{
    const auto cEntity = static_cast<entt::entity>(acMessage.Subject);
    const auto* pForm = m_world.try_get<FormIdComponent>(cEntity);
    if (!pForm || !pForm->Id.BaseId || pForm->Id.BaseId > 0xffffff)
        return;
    if (!aSender.HasModId(pForm->Id.ModId) || m_world.get<CharacterComponent>(cEntity).IsPlayer())
        return;
    const auto cReference = pForm->Id.LogFormat();
    if (acMessage.Key && acMessage.Key != cReference)
        return;
    const auto cKey = std::make_pair(acMessage.Addon, cReference);
    // For each player, each add-on and the whole server, so one cannot use up the room of the rest.
    auto it = m_areas.find(cKey);
    if (it == m_areas.end())
    {
        if (acMessage.State == UR_EXTENSION_AREA_ERASE || !ExtensionAreas::MakeRoom(m_areas, acMessage.Addon, aSender.GetId()))
            return;
        it = m_areas.try_emplace(cKey).first;
        it->second.Owner = aSender.GetId();
    }
    auto& area = it->second;
    area.Owner = ExtensionAreas::NextOwner(area.Owner, aSender.GetId(), ExtensionAreas::Owned(m_areas, aSender.GetId()));
    area.Cell = m_world.get<CellIdComponent>(cEntity);
    area.Message.Data = acMessage;
    area.Message.Data.Subject = area.Message.Data.SubjectEpoch = 0;
    area.Message.Data.Key = cReference;
    RelayArea(area, true);
    if (acMessage.State == UR_EXTENSION_AREA_ERASE)
        m_areas.erase(it);
}

void ExtensionService::RelayArea(AreaState& aState, bool aNew)
{
    const auto& cMessage = aState.Message;
    for (auto* pViewer : m_world.GetPlayerManager())
    {
        const auto cId = pViewer->GetId();
        const auto cPeer = m_peers.find(cId);
        if (cPeer == m_peers.end() || !cPeer->second.Subscriptions.contains(cMessage.Data.Addon))
            continue;
        const bool cInRange = pViewer->GetCharacter() && pViewer->GetCellComponent() && pViewer->GetCellComponent().IsInRange(aState.Cell, false);
        if (cMessage.Data.State == UR_EXTENSION_AREA_ERASE || !cInRange)
        {
            if (aState.Delivered.erase(cId))
            {
                auto remove = cMessage;
                remove.Data.State = UR_EXTENSION_AREA_ERASE;
                remove.Data.Bytes.clear();
                pViewer->Send(remove);
            }
            continue;
        }
        if (!aNew)
        {
            if (aState.Delivered.contains(cId) || cPeer->second.Replays >= 32)
                continue;
            ++cPeer->second.Replays;
        }
        pViewer->Send(cMessage);
        aState.Delivered.insert(cId);
    }
}

ExtensionRecords::Area ExtensionService::AreaOf(const CellIdComponent& acCell) const
{
    const auto cPlugin = [&](const GameId& acId)
    {
        std::string name;
        if (acId)
        {
            const auto& cMods = m_world.ctx().at<ModsComponent>();
            for (const auto* pList : {&cMods.GetStandardMods(), &cMods.GetLiteMods()})
                for (const auto& [key, entry] : *pList)
                    if (entry.id == acId.ModId)
                    {
                        name.assign(key.begin(), key.end());
                        for (auto& c : name)
                            if (c >= 'A' && c <= 'Z')
                                c += 'a' - 'A';
                        return name;
                    }
        }
        return name;
    };
    return {cPlugin(acCell.Cell), acCell.Cell.BaseId, cPlugin(acCell.WorldSpaceId), acCell.WorldSpaceId.BaseId, acCell.CenterCoords.X, acCell.CenterCoords.Y};
}

// A saved record from a game: kept if its writer may, and relayed to those near it.
void ExtensionService::OnRecord(Player& aSender, const ExtensionData& acMessage)
{
    ExtensionRecords::Record record;
    NotifyExtension reply;
    reply.Data = acMessage;
    const auto cKey = std::make_pair(acMessage.Addon, acMessage.Key);
    const auto cWriter = m_writers.find(cKey);
    // A player cannot touch more keys in a session than this, whatever add-on or writer it names.
    constexpr size_t cMaxKeysPerPlayer = 1024;
    const bool cRoom =
        cWriter != m_writers.end() || std::count_if(m_writers.begin(), m_writers.end(), [&](const auto& acPair) { return acPair.second == aSender.GetId(); }) < cMaxKeysPerPlayer;
    const bool cOwnsKey = cWriter == m_writers.end() || cWriter->second == aSender.GetId();
    if (cRoom && cOwnsKey && m_store.Apply(AreaOf(aSender.GetStandingCell()), acMessage, record))
    {
        reply.Data = record.Value;
        m_writers[cKey] = aSender.GetId();
    }
    else
        reply.Data.Bytes.clear();
    reply.Data.Result = record.Value.Revision ? 1 : 2;
    reply.Data.WriterHigh = reply.Data.WriterLow = 0;
    aSender.Send(reply);
    if (reply.Data.Result != 1 || acMessage.State == UR_EXTENSION_READ)
        return;
    reply.Data.Result = 0;
    for (auto* pViewer : m_world.GetPlayerManager())
    {
        auto& viewer = m_peers[pViewer->GetId()];
        const bool cSubscribed = pViewer->GetCharacter() && viewer.Subscriptions.contains(acMessage.Addon);
        if (cSubscribed && record.Location.InRange(AreaOf(pViewer->GetCellComponent())))
        {
            if (pViewer != &aSender)
                pViewer->Send(reply);
            viewer.Seen[cKey] = reply.Data.Revision;
        }
    }
}

void ExtensionService::ReplayRecords(Player& aViewer, Peer& aPeer) const
{
    const auto& cCell = aViewer.GetCellComponent();
    if (aPeer.Cell.Cell != cCell.Cell || aPeer.Cell.WorldSpaceId != cCell.WorldSpaceId || aPeer.Cell.CenterCoords != cCell.CenterCoords)
    {
        const auto cAfter = AreaOf(cCell);
        aPeer.Cell = cCell;
        aPeer.Pending.clear();
        for (const auto& [key, record] : m_store.All())
        {
            if (!aPeer.Subscriptions.contains(key.first) || !record.Location.InRange(cAfter))
                aPeer.Seen.erase(key);
            else if (!aPeer.Seen.contains(key) || aPeer.Seen.at(key) != record.Value.Revision)
                aPeer.Pending.push_back(key);
        }
    }
    // Leave room for play traffic while a player enters an area with many saved records.
    for (unsigned i = 0; i < 16 && !aPeer.Pending.empty(); ++i)
    {
        const auto cKey = aPeer.Pending.front();
        aPeer.Pending.pop_front();
        // A removed record queued here may have been trimmed since.
        const auto* pRecord = m_store.Find(cKey);
        if (!pRecord)
            continue;
        const auto& cRecord = *pRecord;
        if (aPeer.Seen[cKey] == cRecord.Value.Revision || !cRecord.Location.InRange(AreaOf(cCell)))
            continue;
        NotifyExtension notify;
        notify.Data = cRecord.Value;
        notify.Data.WriterHigh = notify.Data.WriterLow = 0;
        aViewer.Send(notify);
        aPeer.Seen[cKey] = cRecord.Value.Revision;
    }
}

// Every 250 ms: ends old leases, refreshes subscribers and replays what is kept to games that came near.
void ExtensionService::OnUpdate(const UpdateEvent&)
{
    const auto cNow = Clock::now();
    if (cNow < m_nextReplay)
        return;
    m_nextReplay = cNow + std::chrono::milliseconds(250);
    std::erase_if(m_leases, [&](const auto& acPair) { return acPair.second.Until <= cNow; });
    for (auto& [id, peer] : m_peers)
    {
        peer.Replays = 0;
        for (auto it = peer.Refresh.begin(); it != peer.Refresh.end();)
        {
            if (it->second > cNow)
            {
                ++it;
                continue;
            }
            const auto cAddon = it->first;
            std::erase_if(peer.Seen, [&](const auto& acPair) { return acPair.first.first == cAddon; });
            peer.Cell = {};
            for (auto& [key, state] : m_states)
                if (state.Message.Data.Addon == cAddon)
                    state.Delivered.erase(id);
            for (auto& [key, state] : m_reservations)
                if (state.Message.Data.Addon == cAddon)
                    state.Delivered.erase(id);
            for (auto& [key, state] : m_areas)
                if (key.first == cAddon)
                    state.Delivered.erase(id);
            peer.NextRefresh[cAddon] = cNow + std::chrono::seconds(2);
            it = peer.Refresh.erase(it);
        }
    }
    for (auto it = m_reservations.begin(); it != m_reservations.end();)
    {
        auto* pSender = m_world.GetPlayerManager().GetById(it->second.Message.Sender);
        if (!Reserved(it->first) || !pSender || !Authorize(*pSender, it->second.Message.Data))
        {
            EndReservation(it->second);
            it = m_reservations.erase(it);
        }
        else
        {
            Relay(it->second, false);
            ++it;
        }
    }
    for (auto it = m_states.begin(); it != m_states.end();)
    {
        auto& state = it->second;
        auto* pSender = m_world.GetPlayerManager().GetById(state.Message.Sender);
        const bool cPartyGone = pSender && state.Message.Data.Scope == UR_EXTENSION_PARTY && pSender->GetParty().JoinedPartyId != state.Party;
        if (!pSender || !Authorize(*pSender, state.Message.Data) || cPartyGone)
        {
            Retire(it->second);
            it = m_states.erase(it);
        }
        else
        {
            Relay(state, false);
            ++it;
        }
    }
    for (auto& [key, state] : m_areas)
        RelayArea(state, false);
    if (m_store.All().empty())
        return;
    for (auto* pViewer : m_world.GetPlayerManager())
    {
        if (!pViewer->GetCharacter() || !pViewer->GetCellComponent())
            continue;
        ReplayRecords(*pViewer, m_peers[pViewer->GetId()]);
    }
}

// A player leaves: whatever it held for the add-ons goes with it; the states it left in the world stay.
void ExtensionService::OnLeave(const PlayerLeaveEvent& acEvent)
{
    const auto cId = acEvent.pPlayer->GetId();
    m_peers.erase(cId);
    ExtensionAreas::Leave(m_areas, cId);
    for (auto it = m_reservations.begin(); it != m_reservations.end();)
        if (it->second.Message.Sender == cId)
        {
            EndReservation(it->second);
            it = m_reservations.erase(it);
        }
        else
        {
            it->second.Delivered.erase(cId);
            ++it;
        }
    std::erase_if(m_writers, [&](const auto& acPair) { return acPair.second == cId; });
    std::erase_if(m_leases, [&](const auto& acPair) { return acPair.second.Player == cId; });
    for (auto it = m_states.begin(); it != m_states.end();)
        if (it->second.Message.Sender == cId)
        {
            Retire(it->second);
            it = m_states.erase(it);
        }
        else
        {
            it->second.Delivered.erase(cId);
            ++it;
        }
}
