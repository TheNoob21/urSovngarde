#include <TiltedOnlinePCH.h>

#include <Services/MountService.h>
#include <Services/CopyWorkGate.h>
#include <Services/CopyWorkQueue.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/MountEvent.h>
#include <Events/ActivateEvent.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/RequestDismount.h>
#include <Messages/NotifyDismount.h>

#include <Games/ActorExtension.h>
#include <Games/Memory.h>
#include <Interface/UI.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Forms/TESNPC.h>
#include <Forms/TESFaction.h>
#include <Services/PartyService.h>
#include <Services/PapyrusService.h>
#include <Components.h>
#include <World.h>
#include <Utils.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <mutex>

namespace
{
//! How long this player's getting on a party member's horse waits for the horse to be handed over.
constexpr double kRideClaimFor = 3.0;
//! This player gets on a horse handed over only within this many units of it.
constexpr float kRideClaimRange = 400.f;

// Owners as the game keeps them (ObjectReference.GetActorOwner and the others), on the main thread.
using ObjectReference = TESObjectREFR;

TESNPC* ActorOwnerOf(TESObjectREFR* apReference) noexcept
{
    PAPYRUS_FUNCTION(TESNPC*, ObjectReference, GetActorOwner);
    return s_pGetActorOwner(apReference);
}

TESFaction* FactionOwnerOf(TESObjectREFR* apReference) noexcept
{
    PAPYRUS_FUNCTION(TESFaction*, ObjectReference, GetFactionOwner);
    return s_pGetFactionOwner(apReference);
}

void SetActorOwnerOf(TESObjectREFR* apReference, TESNPC* apOwner) noexcept
{
    PAPYRUS_FUNCTION(void, ObjectReference, SetActorOwner, TESNPC*);
    s_pSetActorOwner(apReference, apOwner);
}

void SetFactionOwnerOf(TESObjectREFR* apReference, TESFaction* apOwner) noexcept
{
    PAPYRUS_FUNCTION(void, ObjectReference, SetFactionOwner, TESFaction*);
    s_pSetFactionOwner(apReference, apOwner);
}

// The owners shared horses had here. Changed by RunQueuedWork, read by the character save.
std::mutex s_ownersBeforeLock;
MountSync::HorseOwners s_ownersBefore;

void PutOwnerBack(TESObjectREFR* apHorse, const MountSync::OwnerBefore& acBefore) noexcept
{
    if (acBefore.FactionOwner)
        SetFactionOwnerOf(apHorse, Cast<TESFaction>(TESForm::GetById(acBefore.FactionOwner)));
    else
        SetActorOwnerOf(apHorse, acBefore.ActorOwner ? Cast<TESNPC>(TESForm::GetById(acBefore.ActorOwner)) : nullptr);
}

TESNPC* PlayerBase() noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
}

// Copies that replayed their owner's climb onto a horse, by form id, and when (Now()).
std::mutex s_ownerClimbsLock;
TiltedPhoques::Map<uint32_t, double> s_ownerClimbs;

uint32_t SitState(const Actor* apActor) noexcept
{
    return (apActor->actorState.flags1 >> 14) & 0xF;
}

double Now() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Actor* ActorByFormId(const uint32_t aFormId) noexcept
{
    return aFormId ? Cast<Actor>(TESForm::GetById(aFormId)) : nullptr;
}

std::optional<entt::entity> EntityByFormId(World& aWorld, const uint32_t aFormId) noexcept
{
    return Utils::FindByFormId(aWorld.view<FormIdComponent>(entt::exclude<ObjectComponent>), aFormId);
}

//! The server id and ownership epoch of an actor this game runs or shows.
bool ServerIdentity(World& aWorld, const entt::entity aEntity, uint32_t& aId, uint32_t& aEpoch) noexcept
{
    if (const auto* pLocal = aWorld.try_get<LocalComponent>(aEntity))
    {
        aId = pLocal->Id;
        aEpoch = pLocal->OwnershipEpoch;
        return true;
    }
    if (const auto* pRemote = aWorld.try_get<RemoteComponent>(aEntity))
    {
        aId = pRemote->Id;
        aEpoch = pRemote->OwnershipEpoch;
        return true;
    }
    return false;
}

std::optional<entt::entity> RemoteByServerId(World& aWorld, const uint32_t aServerId) noexcept
{
    if (const auto cEntity = aWorld.FindByServerId(ServerIdIndex::Kind::kRemote, aServerId); cEntity && aWorld.all_of<FormIdComponent>(*cEntity))
        return cEntity;
    return std::nullopt;
}

//! The same interior, or the same worldspace outside (an exterior cell is only 4096 units across: a rider beside its
//! horse is often in the next one).
bool SamePlace(const Actor* apRider, const Actor* apMount) noexcept
{
    const TESObjectCELL* pRiderCell = apRider->GetParentCellEx();
    if (!pRiderCell)
        return false;
    if (pRiderCell == apMount->GetParentCellEx())
        return true;
    const TESWorldSpace* pWorld = apRider->GetWorldSpace();
    return pWorld && pWorld == apMount->GetWorldSpace();
}

float DistanceSquared(const Actor* apRider, const Actor* apMount) noexcept
{
    const glm::vec3 cOffset = glm::vec3(apRider->position) - glm::vec3(apMount->position);
    return glm::dot(cOffset, cOffset);
}

bool IsLiving(Actor* apActor) noexcept
{
    return apActor && !apActor->IsDeleted() && !apActor->actorState.IsDeadOrDying() && apActor->GetNiNode();
}

uint32_t HandleOf(const Actor* apActor) noexcept
{
    return apActor ? apActor->GetHandle().handle.iBits : 0;
}

//! What is done to a copy on the main thread.
enum class Work : uint8_t
{
    //! On its horse by the game's quick mount, climbing on if asked.
    Seat,
    //! Straight into the saddle: its climb did not take.
    Saddle,
    //! The horse the last try named is taken off a copy that stays on foot.
    ClearVehicle,
    //! Climbing off, or off at once when its graph will not.
    Exit,
    //! Off at once.
    Release,
    //! A rider copy the disconnect deletes: off its horse first, then deleted (CharacterService::OnDisconnected).
    ReleaseThenDelete,
};

struct MountWork
{
    Work What{};
    uint32_t RiderServerId{};
    uint32_t RiderFormId{};
    uint32_t RiderHandle{};
    uint32_t MountFormId{};
    uint32_t MountHandle{};
    //! Exit: the horse it climbs off, by server id.
    uint32_t MountServerId{};
    CopyWork::Stamp Stamp{};
    //! The ride a seat puts in place (CopySeat::Ride); an undo names it in Stamp.Undoes.
    uint32_t Ride{};
    //! Seat: shown climbing on. Exit: shown climbing off.
    bool Climb{};
    //! After a disconnect: the copy is no copy any more, and is still taken off the horse this game put it on.
    bool SessionOver{};
    uint8_t Try{};
    std::chrono::steady_clock::time_point QueuedAt{};
};

struct MountWorkDone
{
    Work What{};
    uint32_t RiderServerId{};
    uint32_t Ticket{};
    uint32_t RiderFormId{};
    uint32_t RiderHandle{};
    //! The ride it was done for.
    uint32_t Ride{};
    //! Seat: its graph took the climb. Exit: its graph took the climb off.
    bool Climbs{};
    //! Exit: the horse it climbs off, by server id.
    uint32_t MountServerId{};
};

CopyWorkQueue<MountWork, MountWorkDone> s_work;
// Rider copies handed over to be deleted, by handle, until their deletion is done (MountService::DeletionPending).
std::mutex s_deletingLock;
TiltedPhoques::Set<uint32_t> s_deleting;

void ForgetDeletion(const uint32_t aHandle) noexcept
{
    std::scoped_lock lock(s_deletingLock);
    s_deleting.erase(aHandle);
}
// Rides are told apart by these, so that taking a copy off one ride never takes it off a later one.
std::atomic<uint32_t> s_lastRide{0};

uint32_t NewRide() noexcept
{
    uint32_t ride = ++s_lastRide;
    if (ride == 0)
        ride = ++s_lastRide;
    return ride;
}

//! The actor a retained reference names, if it is still the one a piece of work was for (not deleted; its 3D is checked apart).
Actor* WorkActor(const RetainedReference& acReference, const uint32_t aFormId) noexcept
{
    Actor* pActor = Cast<Actor>(acReference.Get());
    return pActor && pActor->formID == aFormId && !pActor->IsDeleted() ? pActor : nullptr;
}

bool IsCopy(Actor* apActor) noexcept
{
    const ActorExtension* pExtension = apActor ? apActor->GetExtension() : nullptr;
    return pExtension && pExtension->IsRemote();
}

void QueueWork(MountWork aWork) noexcept
{
    aWork.Stamp.LoadingScreens = UI::LoadingScreensQueued();
    aWork.QueuedAt = std::chrono::steady_clock::now();
    s_work.Push(aWork);
}

//! Horse work done on the main thread (RunQueuedWork), checked there the way copies' work is (MountSync::CheckHorse).
struct HorseWork
{
    MountSync::HorseJob What{};
    uint32_t HorseFormId{};
    MountSync::HorseStamp Stamp{};
    std::chrono::steady_clock::time_point QueuedAt{};
    //! A Ride: until when its claim waits for the horse (Now()).
    double Until{};
};

//! What the update takes back from the main thread. A share it did not make: the update forgets it, and shares the horse again if it
//! still should. A Ride it did or dropped: its claim is spent.
struct HorseWorkDone
{
    uint32_t HorseFormId{};
    uint32_t Ticket{};
};

// Shares and their undoing, by horse form id; rides apart, so that a claim never withdraws a share or the other way round.
CopyWorkQueue<HorseWork, HorseWorkDone> s_shares;
CopyWorkQueue<HorseWork, HorseWorkDone> s_rides;
// The connection horse work is decided in: a disconnect starts the next one, which drops the work of the last.
std::atomic<uint32_t> s_session{1};

void QueueHorseWork(
    CopyWorkQueue<HorseWork, HorseWorkDone>& aQueue, const MountSync::HorseJob aWhat, const uint32_t aHorseFormId, const uint32_t aHandle, const uint32_t aTicket,
    const uint32_t aUndoes = 0) noexcept
{
    HorseWork work{};
    work.What = aWhat;
    work.HorseFormId = aHorseFormId;
    work.Stamp.Work.Ticket = aTicket;
    work.Stamp.Work.Undoes = aUndoes;
    work.Stamp.Work.LoadingScreens = UI::LoadingScreensQueued();
    work.Stamp.Session = s_session;
    work.Stamp.Handle = aHandle;
    work.QueuedAt = std::chrono::steady_clock::now();
    aQueue.Push(work);
}

//! This player gets on a horse handed over for a claim, under what the claim was asked for under and until its time is up.
void QueueRide(const uint32_t aHorseFormId, const MountSync::RideClaim& acClaim) noexcept
{
    HorseWork work{};
    work.What = MountSync::HorseJob::Ride;
    work.HorseFormId = aHorseFormId;
    work.Stamp = acClaim.Stamp;
    work.Until = acClaim.Until;
    work.QueuedAt = std::chrono::steady_clock::now();
    s_rides.Push(work);
}

//! The share on a horse goes: its pending work is withdrawn, and its owner here is put back if the share is in place.
void QueueUnshare(const uint32_t aHorseFormId, const uint32_t aHandle, const uint32_t aShare) noexcept
{
    s_shares.Retire(aHorseFormId);
    QueueHorseWork(s_shares, MountSync::HorseJob::Unshare, aHorseFormId, aHandle, 0, aShare);
}

void RunHorseWork(CopyWorkQueue<HorseWork, HorseWorkDone>& aQueue) noexcept
{
    TiltedPhoques::Vector<HorseWork> work = aQueue.TakeWork();
    if (work.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<HorseWork> held;
    for (const HorseWork& cWork : work)
    {
        // Looked at again for each piece: a load or the exit can start partway through.
        if (IsProcessExiting())
        {
            aQueue.Clear();
            return;
        }

        // Read before the lookup, so that nothing is looked up during a load.
        const CopyWork::Moment cBefore = LoadMomentNow();
        // Held while the work runs, and only if its handle still names the horse it was decided for.
        const RetainedReference cReference(cBefore.Loading || cBefore.Exiting ? 0 : cWork.Stamp.Handle);
        Actor* pHorse = WorkActor(cReference, cWork.HorseFormId);
        MountSync::HorseFound found{};
        found.Loading = cBefore.Loading;
        found.LoadingScreens = cBefore.LoadingScreens;
        found.Session = s_session;
        found.SameHorse = pHorse != nullptr;
        found.Living = IsLiving(pHorse);
        found.IsMount = pHorse && pHorse->IsMount();
        found.RunHere = pHorse && !IsCopy(pHorse);
        found.Live = cWork.Stamp.Work.Ticket && aQueue.Live(cWork.HorseFormId, cWork.Stamp.Work.Ticket);
        found.WaitedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - cWork.QueuedAt).count());
        found.Expired = cWork.What == MountSync::HorseJob::Ride && Now() >= cWork.Until;

        // Changed only here, read by the character save.
        std::scoped_lock ownersLock(s_ownersBeforeLock);
        found.Shared = s_ownersBefore.Shared(cWork.HorseFormId);

        const CopyWork::Verdict cFirst = MountSync::CheckHorse(cWork.What, cWork.Stamp, found);
        // Looked at again right before the engine is called, in the same step.
        const CopyWork::Verdict cVerdict = cFirst == CopyWork::Verdict::kDo ? CopyWork::Recheck(cWork.Stamp.Work, cBefore, LoadMomentNow()) : cFirst;
        if (cVerdict == CopyWork::Verdict::kHold)
        {
            held.push_back(cWork);
            continue;
        }

        switch (cWork.What)
        {
        case MountSync::HorseJob::Share:
        {
            TESNPC* pPlayerBase = PlayerBase();
            if (cVerdict != CopyWork::Verdict::kDo || !pPlayerBase)
            {
                aQueue.Give({cWork.HorseFormId, cWork.Stamp.Work.Ticket});
                break;
            }
            // Shared already: this share carries it on.
            if (s_ownersBefore.CarryOn(cWork.HorseFormId, cWork.Stamp.Handle, cWork.Stamp.Work.Ticket))
                break;
            const TESNPC* pActorOwner = ActorOwnerOf(pHorse);
            const TESFaction* pFactionOwner = FactionOwnerOf(pHorse);
            s_ownersBefore.Record(
                cWork.HorseFormId, {pActorOwner ? pActorOwner->formID : 0, pFactionOwner ? pFactionOwner->formID : 0, cWork.Stamp.Handle, cWork.Stamp.Work.Ticket});
            SetActorOwnerOf(pHorse, pPlayerBase);
            spdlog::debug(
                "Mount: horse {:X}, ridden by a party member, counts as this player's too (owner here was {:X}/{:X})", cWork.HorseFormId, pActorOwner ? pActorOwner->formID : 0,
                pFactionOwner ? pFactionOwner->formID : 0);
            break;
        }
        case MountSync::HorseJob::Unshare:
        {
            // A later share's record stays. One whose horse a loading screen kept out of reach stays owed (SettleOwedOwners).
            MountSync::OwnerBefore before{};
            const MountSync::Undo cUndo = s_ownersBefore.Unshare(cWork.HorseFormId, cWork.Stamp.Work.Undoes, cVerdict, found.Loading, before);
            if (cUndo == MountSync::Undo::kPutBack)
                PutOwnerBack(pHorse, before);
            else if (cUndo == MountSync::Undo::kOwe)
                spdlog::debug("Mount: horse {:X} stays shared until the loading screen is gone", cWork.HorseFormId);
            break;
        }
        case MountSync::HorseJob::Ride:
        {
            // Done or dropped, the claim is spent.
            aQueue.Give({cWork.HorseFormId, cWork.Stamp.Work.Ticket});
            if (cVerdict != CopyWork::Verdict::kDo)
                break;
            // This player gets on it now, as the game does when the player uses a horse.
            PlayerCharacter* pPlayer = PlayerCharacter::Get();
            if (!pPlayer || pPlayer->IsOnMount() || glm::distance(glm::vec3(pPlayer->position), glm::vec3(pHorse->position)) > kRideClaimRange)
                break;
            const bool cTaken = pHorse->Activate(pPlayer, 0, nullptr, 1, 1);
            spdlog::debug("Mount: horse {:X}, handed to this game, used by this player ({})", cWork.HorseFormId, cTaken ? "taken" : "not taken");
            break;
        }
        }
    }
    aQueue.Hold(held);
}

//! Owners whose Unshare a loading screen outlasted go back once it is gone, on the horse the share was made on; if its handle names no
//! such horse any more, there is nothing to put them back on.
void SettleOwedOwners() noexcept
{
    if (IsProcessExiting() || LoadingScreenUp())
        return;
    std::scoped_lock ownersLock(s_ownersBeforeLock);
    s_ownersBefore.Settle(
        [](const uint32_t aHorseFormId, const MountSync::OwnerBefore& acBefore)
        {
            const RetainedReference cReference(acBefore.Handle);
            Actor* pHorse = WorkActor(cReference, aHorseFormId);
            if (pHorse)
                PutOwnerBack(pHorse, acBefore);
            spdlog::debug("Mount: horse {:X} no longer shared after the loading screen ({})", aHorseFormId, pHorse ? "owner put back" : "horse gone");
        });
}

//! Off at once. Papyrus Dismount would give the copy a package, which it never runs.
void ReleaseNow(Actor* apRider) noexcept
{
    // The horse SetVehicle named goes even when the climb off already took the copy off: left, it marks the horse as this copy's.
    if (!apRider->IsOnMount())
    {
        apRider->SetVehicle(nullptr);
        return;
    }
    apRider->ExitMountQuick();
    apRider->SetVehicle(nullptr);
    spdlog::debug("Mount: copy {:X} got off; on a horse now: {}", apRider->formID, apRider->IsOnMount());
}
} // namespace

MountService::MountService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MountService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&MountService::OnDisconnected>(this);
    m_mountConnection = aDispatcher.sink<MountEvent>().connect<&MountService::OnMountEvent>(this);
    m_notifyMountConnection = aDispatcher.sink<NotifyMount>().connect<&MountService::OnNotifyMount>(this);
    m_notifyDismountConnection = aDispatcher.sink<NotifyDismount>().connect<&MountService::OnNotifyDismount>(this);
    m_activateConnection = aDispatcher.sink<ActivateEvent>().connect<&MountService::OnActivate>(this);
    // A rider or horse changing owner withdraws what was decided for its ride at once, before the next frame does it.
    m_remoteAddedConnection = m_world.on_construct<RemoteComponent>().connect<&MountService::OnOwnerChanged>(this);
    m_remoteChangedConnection = m_world.on_update<RemoteComponent>().connect<&MountService::OnOwnerChanged>(this);
    m_remoteRemovedConnection = m_world.on_destroy<RemoteComponent>().connect<&MountService::OnOwnerChanged>(this);
}

void MountService::OnOwnerChanged(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    if (m_seats.empty())
        return;
    const auto* pRemote = aRegistry.try_get<RemoteComponent>(aEntity);
    if (!pRemote)
        return;
    for (auto it = m_seats.begin(); it != m_seats.end(); ++it)
        if (it->first == pRemote->Id || it->second.MountServerId == pRemote->Id)
            it.value().Ticket = s_work.Issue(it->first);
}

bool MountService::ReleaseBeforeDelete(const uint32_t aRiderServerId, Actor* apRider) noexcept
{
    const uint32_t cRide = s_work.InPlace(aRiderServerId);
    if (!cRide || !apRider || apRider->IsDeleted())
        return false;

    MountWork work{};
    work.What = Work::ReleaseThenDelete;
    work.RiderServerId = aRiderServerId;
    work.RiderFormId = apRider->formID;
    work.RiderHandle = HandleOf(apRider);
    work.Stamp.Undoes = cRide;
    work.Ride = cRide;
    work.SessionOver = true;
    {
        std::scoped_lock lock(s_deletingLock);
        s_deleting.insert(work.RiderHandle);
    }
    QueueWork(work);
    return true;
}

bool MountService::DeletionPending(const uint32_t aHandle) noexcept
{
    std::scoped_lock lock(s_deletingLock);
    return aHandle && s_deleting.count(aHandle) != 0;
}

bool MountService::TakesClimbs() noexcept
{
    return Actor::CanReadMount();
}

void MountService::NoteOwnerClimb(const uint32_t aRiderFormId) noexcept
{
    std::scoped_lock lock(s_ownerClimbsLock);
    // Copies removed meanwhile leave theirs behind.
    if (s_ownerClimbs.size() > 64)
        s_ownerClimbs.clear();
    s_ownerClimbs[aRiderFormId] = Now();
}

MountService::ShareSaveGuard::ShareSaveGuard(Actor* apActor) noexcept
{
    if (!apActor)
        return;
    // Held over the owner change, so that the share is not made or taken back halfway through it.
    std::scoped_lock lock(s_ownersBeforeLock);
    const MountSync::OwnerBefore* pBefore = s_ownersBefore.Find(apActor->formID);
    const uint32_t cHandle = HandleOf(apActor);
    if (!MountSync::GuardLifts(pBefore != nullptr, pBefore ? MountSync::SharedOwner{pBefore->Handle, pBefore->Share} : MountSync::SharedOwner{}, cHandle))
        return;
    m_pActor = apActor;
    m_handle = cHandle;
    m_share = pBefore->Share;
    m_lifted = true;
    PutOwnerBack(apActor, *pBefore);
}

MountService::ShareSaveGuard::~ShareSaveGuard() noexcept
{
    if (!m_lifted || !m_pActor)
        return;
    std::scoped_lock lock(s_ownersBeforeLock);
    const MountSync::OwnerBefore* pBefore = s_ownersBefore.Find(m_pActor->formID);
    if (MountSync::GuardRestores(
            pBefore != nullptr, pBefore ? MountSync::SharedOwner{pBefore->Handle, pBefore->Share} : MountSync::SharedOwner{}, {m_handle, m_share}, HandleOf(m_pActor)))
        SetActorOwnerOf(m_pActor, PlayerBase());
}

void MountService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const double cNow = Now();
    TakeDoneWork(cNow);
    UpdatePendingWords(cNow);
    UpdateRideClaims(cNow);
    // Looked at even with nobody riding: a party member who left the party leaves the horses they rode.
    UpdateSharedHorses();
    WatchRidersAlreadyOn(cNow);
    if (m_watches.empty() && m_seats.empty() && m_leaving.empty())
        return;
    if (cNow < m_nextWatchAt && !m_everyFrame)
        return;
    if (cNow >= m_nextWatchAt)
        m_nextWatchAt = cNow + MountSync::kWatchInterval;

    UpdateLeaving(cNow);
    UpdateSeats(cNow);
    UpdateWatches(cNow);

    // Getting on or off is looked at every frame, so that the others are told, and the copies seated, without the wait.
    m_everyFrame = !m_leaving.empty();
    for (const auto& [cFormId, cWatch] : m_watches)
        m_everyFrame = m_everyFrame || MountSync::ReadEveryFrame(cWatch.State);
    for (const auto& [cServerId, cSeat] : m_seats)
        m_everyFrame = m_everyFrame || cSeat.State.Climbing;
}

void MountService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // The disconnect that comes with quitting arrives while the game is tearing itself down: nothing is touched then.
    if (IsProcessExiting())
    {
        s_work.Clear();
        s_shares.Clear();
        s_rides.Clear();
        ++s_session;
        m_watches.clear();
        m_seats.clear();
        m_leaving.clear();
        m_everyFrame = false;
        return;
    }

    // This game's own riders stay as they are. The copies this game put on a horse are taken off it at the start of the next
    // frame: a horse left ridden by a copy that is then deleted may refuse the player. Their entities are gone by now (the
    // characters hear of the disconnect first), so they are named by what the seats kept.
    s_work.TakeDone();
    for (const auto& [cRiderServerId, cSeat] : m_seats)
    {
        s_work.Retire(cRiderServerId);
        if (const auto riderEntity = RemoteByServerId(m_world, cRiderServerId))
            MarkSeated(*riderEntity, entt::null);
        QueueRelease(cRiderServerId, cSeat.RiderFormId, cSeat.Bound.RiderHandle, cSeat.Ride, true);
    }
    for (const auto& [cFormId, cLeaving] : m_leaving)
        QueueRelease(cLeaving.RiderServerId, cFormId, cLeaving.RiderHandle, cLeaving.Ride, true);
    // Horse work decided in this connection is dropped: no horse is got on or shared after it. Shared horses get their owners
    // here back.
    ++s_session;
    s_rides.TakeDone();
    m_rideClaims.Clear([](const uint32_t aHorseFormId) { s_rides.Retire(aHorseFormId); });
    s_shares.TakeDone();
    for (const auto& [cHorse, cShared] : m_sharedHorses)
        QueueUnshare(cHorse, cShared.Handle, cShared.Share);
    m_watches.clear();
    m_seats.clear();
    m_leaving.clear();
    m_pendingWords.clear();
    m_sharedHorses.clear();
    m_everyFrame = false;
}

void MountService::OnMountEvent(const MountEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Without the read-back the ride cannot be watched: it is sent as it starts, as before.
    if (!Actor::CanReadMount())
    {
        SendLegacyMount(acEvent.RiderID, acEvent.MountID);
        return;
    }

    auto it = m_watches.find(acEvent.RiderID);
    if (it != m_watches.end())
    {
        if (it->second.MountFormId == acEvent.MountID)
            return;

        // A new horse: the old pair ends first.
        if (it->second.State.Sent)
        {
            RequestDismount request{};
            request.RiderId = it->second.RiderServerId;
            request.RiderOwnershipEpoch = it->second.RiderOwnershipEpoch;
            request.MountId = it->second.MountServerId;
            m_transport.Send(request);
        }
        m_watches.erase(it);
    }

    RiderWatch watch{};
    watch.MountFormId = acEvent.MountID;
    watch.State.StartedAt = Now();
    m_watches[acEvent.RiderID] = watch;
}

void MountService::WatchRidersAlreadyOn(const double aNow) noexcept
{
    if (aNow < m_nextAlreadyOnAt)
        return;
    m_nextAlreadyOnAt = aNow + MountSync::kAlreadyOnScanInterval;
    if (!Actor::CanReadMount() || LoadingScreenUp())
        return;

    auto localView = m_world.view<LocalComponent, FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const auto cEntity : localView)
    {
        const uint32_t cRiderFormId = localView.get<FormIdComponent>(cEntity).Id;
        if (m_watches.find(cRiderFormId) != m_watches.end())
            continue;
        // A rider with no 3D (a cell change, a load) cannot be read; it is looked at again later.
        Actor* pRider = ActorByFormId(cRiderFormId);
        if (!pRider || pRider->IsDeleted() || pRider->IsMount() || pRider->IsDead() || !pRider->GetNiNode() || !pRider->IsOnMount())
            continue;
        const uint32_t cHorseFormId = RiddenHorseNear(pRider);
        if (!cHorseFormId)
            continue;

        // Already on: the first word is a refresh, and the other games seat their copy without a climb.
        RiderWatch watch{};
        watch.MountFormId = cHorseFormId;
        watch.AlreadyOn = true;
        watch.State.StartedAt = aNow;
        m_watches[cRiderFormId] = watch;
        spdlog::debug("Mount: {:X} is already on {:X}; its ride is shared from now on", cRiderFormId, cHorseFormId);
    }
}

uint32_t MountService::RiddenHorseNear(const Actor* apRider) noexcept
{
    // Only a horse this game runs: a ride is got on with a horse of this game's own (a party member's horse is handed over
    // first), and another game's rider copy on its horse copy may stand right beside this rider.
    TiltedPhoques::Vector<MountSync::NearHorse> horses;
    auto view = m_world.view<LocalComponent, FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const auto cEntity : view)
    {
        const uint32_t cFormId = view.get<FormIdComponent>(cEntity).Id;
        Actor* pHorse = ActorByFormId(cFormId);
        if (!pHorse || pHorse == apRider || !pHorse->IsMount() || pHorse->IsDeleted() || pHorse->IsDead() || !pHorse->GetNiNode())
            continue;
        const float cX = pHorse->position.x - apRider->position.x;
        const float cY = pHorse->position.y - apRider->position.y;
        const float cZ = pHorse->position.z - apRider->position.z;
        const float cDistance = std::sqrt(cX * cX + cY * cY + cZ * cZ);
        if (!(cDistance <= MountSync::kAlreadyOnUnits))
            continue;
        const bool cWatched = std::any_of(m_watches.begin(), m_watches.end(), [cFormId](const auto& acWatch) { return acWatch.second.MountFormId == cFormId; });
        horses.push_back({cFormId, cDistance, pHorse->IsBeingRidden(), cWatched});
    }
    return MountSync::HorseRiddenHere(horses.data(), horses.size());
}

void MountService::UpdateWatches(const double aNow) noexcept
{
    for (auto it = m_watches.begin(); it != m_watches.end();)
    {
        const uint32_t cRiderFormId = it->first;
        RiderWatch& watch = it.value();

        const auto riderEntity = EntityByFormId(m_world, cRiderFormId);
        const auto* pRiderLocal = riderEntity ? m_world.try_get<LocalComponent>(*riderEntity) : nullptr;
        Actor* pRider = ActorByFormId(cRiderFormId);
        if (!pRiderLocal || !pRider || pRider->IsDeleted())
        {
            // Gone, or handed to another game, which watches it from now on.
            it = m_watches.erase(it);
            continue;
        }

        uint32_t mountServerId = 0;
        uint32_t mountEpoch = 0;
        const auto mountEntity = EntityByFormId(m_world, watch.MountFormId);
        const bool cIdsKnown = mountEntity && ServerIdentity(m_world, *mountEntity, mountServerId, mountEpoch) &&
                               MountSync::Valid({pRiderLocal->Id, pRiderLocal->OwnershipEpoch, mountServerId, mountEpoch});

        // A rider with no 3D (a cell change, a load) cannot be read: the look is skipped, not counted as off.
        if (!pRider->GetNiNode())
        {
            ++it;
            continue;
        }

        const bool cOnMount = pRider->IsOnMount();
        if (!cOnMount && watch.State.OffReads == 0)
            watch.State.OffSince = aNow;
        watch.State.OffReads = cOnMount ? 0 : static_cast<uint8_t>(std::min<int>(watch.State.OffReads + 1, 255));
        const MountSync::WatchStep cStep = MountSync::NextWatchStep(watch.State, cOnMount, cIdsKnown, aNow);
        if (cStep == MountSync::WatchStep::SendMount)
        {
            MountRequest request{};
            request.RiderId = pRiderLocal->Id;
            request.RiderOwnershipEpoch = pRiderLocal->OwnershipEpoch;
            request.MountId = mountServerId;
            request.MountOwnershipEpoch = mountEpoch;
            // Getting on, said again a few times while it climbs, so that every copy climbs on with it; a refresh after that.
            request.IsRefresh = !MountSync::IsBoarding(watch.State, watch.AlreadyOn, aNow);
            m_transport.Send(request);
            if (!watch.State.Sent)
                watch.State.FirstSentAt = aNow;

            if (!watch.State.Sent)
                spdlog::debug("Mount: {:X} (server id {:X}) is on {:X} (server id {:X})", cRiderFormId, request.RiderId, watch.MountFormId, request.MountId);

            watch.State.Sent = true;
            watch.State.SentAt = aNow;
            watch.RiderServerId = request.RiderId;
            watch.RiderOwnershipEpoch = request.RiderOwnershipEpoch;
            watch.MountServerId = request.MountId;
        }
        else if (cStep == MountSync::WatchStep::SendDismount)
        {
            RequestDismount request{};
            request.RiderId = pRiderLocal->Id;
            request.RiderOwnershipEpoch = pRiderLocal->OwnershipEpoch;
            request.MountId = watch.MountServerId;
            m_transport.Send(request);

            spdlog::debug("Mount: {:X} (server id {:X}) got off {:X}", cRiderFormId, request.RiderId, watch.MountFormId);
            it = m_watches.erase(it);
            continue;
        }
        else if (cStep == MountSync::WatchStep::Drop)
        {
            it = m_watches.erase(it);
            continue;
        }

        ++it;
    }
}

void MountService::OnNotifyMount(const NotifyMount& acMessage) noexcept
{
    // A word that comes before the rider's or the horse's copy (the horse handed over for the ride a moment later) waits for it.
    if (TakeMountWord(acMessage))
        m_pendingWords.erase(acMessage.RiderId);
    else
        m_pendingWords[acMessage.RiderId] = {acMessage, Now()};
}

void MountService::UpdatePendingWords(const double aNow) noexcept
{
    for (auto it = m_pendingWords.begin(); it != m_pendingWords.end();)
    {
        if (aNow - it->second.second >= MountSync::kPendingWordFor || TakeMountWord(it->second.first))
            it = m_pendingWords.erase(it);
        else
            ++it;
    }
}

bool MountService::TakeMountWord(const NotifyMount& acMessage) noexcept
{
    const auto riderEntity = RemoteByServerId(m_world, acMessage.RiderId);
    const auto mountEntity = RemoteByServerId(m_world, acMessage.MountId);
    if (!riderEntity || !mountEntity)
        return false;

    const auto& cRiderRemote = m_world.get<RemoteComponent>(*riderEntity);
    const auto& cMountRemote = m_world.get<RemoteComponent>(*mountEntity);
    if (cRiderRemote.DeadAtOwner || cMountRemote.DeadAtOwner)
        return true;
    // Not the epochs here yet: the horse's new owner may still be on its way, so the word waits (OnNotifyMount).
    if (!MountSync::Current(
            {acMessage.RiderId, acMessage.RiderOwnershipEpoch, acMessage.MountId, acMessage.MountOwnershipEpoch}, cRiderRemote.OwnershipEpoch, cMountRemote.OwnershipEpoch))
        return false;

    Actor* pRider = ActorByFormId(m_world.get<FormIdComponent>(*riderEntity).Id);
    if (!Actor::CanReadMount())
    {
        Actor* pMount = ActorByFormId(m_world.get<FormIdComponent>(*mountEntity).Id);
        if (IsLiving(pRider) && IsLiving(pMount) && pMount->IsMount())
            pRider->InitiateMountPackage(pMount);
        return true;
    }

    // Since when the copy replayed its owner's own climb, if lately.
    double sinceOwnerClimbed = -1.0;
    if (pRider)
    {
        std::scoped_lock lock(s_ownerClimbsLock);
        if (const auto climbed = s_ownerClimbs.find(pRider->formID); climbed != s_ownerClimbs.end())
            sinceOwnerClimbed = Now() - climbed->second;
    }

    // A refresh of the same pair only keeps the copy on (one that gave up gets one more try); a new horse starts over.
    // A refresh from a new owner of the rider or the horse carries the ride on under the new epochs.
    const auto seatIt = m_seats.find(acMessage.RiderId);
    const bool cKnown = seatIt != m_seats.end();
    if (cKnown && seatIt->second.MountServerId == acMessage.MountId)
    {
        CopySeat& known = seatIt.value();
        known.State.ToldAt = Now();
        MountSync::RetryAfterRefresh(known.State);
        if (MountSync::Rebind(known.Bound, acMessage.RiderOwnershipEpoch, acMessage.MountOwnershipEpoch))
            known.Ticket = s_work.Issue(acMessage.RiderId);
        // A copy not tried yet climbs on if its rider is getting on, whichever word said so first.
        if (MountSync::ShowsClimb(!acMessage.IsRefresh, sinceOwnerClimbed, known.State.Seated || known.State.Tries != 0))
        {
            if (!known.State.ShowClimb)
                known.State.BoardingAt = known.State.ToldAt;
            known.State.ShowClimb = true;
        }
        return true;
    }

    // Still on the last horse (its dismount lost or still playing): off it first, or the read-back would call it seated.
    if (cKnown)
    {
        MarkSeated(*riderEntity, entt::null);
        if (pRider)
            QueueRelease(acMessage.RiderId, pRider->formID, HandleOf(pRider), seatIt->second.Ride);
    }
    // Climbing off still: off at once, as it gets on again.
    if (pRider)
        if (const auto leavingIt = m_leaving.find(pRider->formID); leavingIt != m_leaving.end())
        {
            QueueRelease(acMessage.RiderId, pRider->formID, leavingIt->second.RiderHandle, leavingIt->second.Ride);
            m_leaving.erase(leavingIt);
        }
    CopySeat seat{};
    seat.MountServerId = acMessage.MountId;
    seat.State.ToldAt = Now();
    // Only a rider that is just getting on is shown climbing (the word says so, or the copy just replayed its owner's climb): one
    // already riding (a refresh, a late load) is put in the saddle.
    seat.State.ShowClimb = MountSync::ShowsClimb(!acMessage.IsRefresh, sinceOwnerClimbed, false);
    seat.State.BoardingAt = seat.State.ToldAt;
    seat.Bound.RiderEpoch = acMessage.RiderOwnershipEpoch;
    seat.Bound.MountEpoch = acMessage.MountOwnershipEpoch;
    seat.Ticket = s_work.Issue(acMessage.RiderId);
    seat.Ride = NewRide();
    seat.RiderFormId = m_world.get<FormIdComponent>(*riderEntity).Id;
    m_seats[acMessage.RiderId] = seat;
    // Seated at the next update, not at the next look.
    m_nextWatchAt = 0.0;
    return true;
}

void MountService::OnNotifyDismount(const NotifyDismount& acMessage) noexcept
{
    // Off, copies are only given the mount package, as before rides were read back, and a dismount is left to them.
    if (!Actor::CanReadMount())
        return;

    // A word to get on the same horse that still waits is older than this: it must not seat the copy after it got off, even when the
    // copy is not here yet, or not under this epoch yet. A newer ride (another horse, a later owner) stays.
    if (const auto pending = m_pendingWords.find(acMessage.RiderId); pending != m_pendingWords.end())
    {
        const NotifyMount& cWaiting = pending->second.first;
        if (MountSync::DismountCancels(
                {cWaiting.RiderId, cWaiting.RiderOwnershipEpoch, cWaiting.MountId, cWaiting.MountOwnershipEpoch}, acMessage.RiderId, acMessage.RiderOwnershipEpoch,
                acMessage.MountId))
            m_pendingWords.erase(pending);
    }

    const auto riderEntity = RemoteByServerId(m_world, acMessage.RiderId);
    if (!riderEntity || m_world.get<RemoteComponent>(*riderEntity).OwnershipEpoch != acMessage.RiderOwnershipEpoch)
        return;

    // A late dismount from an earlier horse leaves the ride on the new one alone.
    const auto seatIt = m_seats.find(acMessage.RiderId);
    uint32_t ride = 0;
    if (seatIt != m_seats.end())
    {
        if (seatIt->second.MountServerId != acMessage.MountId)
            return;
        ride = seatIt->second.Ride;
        s_work.Retire(acMessage.RiderId);
        m_seats.erase(seatIt);
    }
    // Only a ride this game put in place is taken away.
    if (!ride)
        ride = s_work.InPlace(acMessage.RiderId);

    // Still held on its horse while it climbs off: the climb plays from the saddle. It is let go once it is off (RunRemoteUpdates).
    Actor* pRider = ActorByFormId(m_world.get<FormIdComponent>(*riderEntity).Id);
    if (!pRider || pRider->IsDeleted())
        return;

    // Shown climbing off, as its rider does, and taken off its horse once it stands (UpdateLeaving); off at once when its graph
    // will not climb off. Either at the start of the next frame.
    MountWork work{};
    work.What = Work::Exit;
    work.RiderServerId = acMessage.RiderId;
    work.RiderFormId = pRider->formID;
    work.RiderHandle = HandleOf(pRider);
    work.MountServerId = acMessage.MountId;
    work.Stamp.Undoes = ride;
    work.Ride = ride;
    work.Climb = true;
    if (ride)
        QueueWork(work);
}

void MountService::UpdateLeaving(const double aNow) noexcept
{
    for (auto it = m_leaving.begin(); it != m_leaving.end();)
    {
        // Gone, made again, or taken over by this game, whose own AI has the rider now: nothing of this game's to finish.
        Actor* pRider = ActorByFormId(it->first);
        if (!pRider || pRider->IsDeleted() || HandleOf(pRider) != it->second.RiderHandle || !RemoteByServerId(m_world, it->second.RiderServerId))
        {
            it = m_leaving.erase(it);
            continue;
        }
        // Off already by its own climb: the horse it named is still taken off it, so that the horse is free to ride.
        if (!pRider->IsOnMount())
        {
            QueueRelease(it->second.RiderServerId, it->first, it->second.RiderHandle, it->second.Ride);
            it = m_leaving.erase(it);
            continue;
        }
        if (MountSync::ExitDone(SitState(pRider), aNow - it->second.Since))
        {
            spdlog::debug("Mount: copy {:X} climbed off in {:.2f} s (sit state {})", pRider->formID, aNow - it->second.Since, SitState(pRider));
            QueueRelease(it->second.RiderServerId, it->first, it->second.RiderHandle, it->second.Ride);
            it = m_leaving.erase(it);
            continue;
        }
        ++it;
    }
}

void MountService::TakeDoneWork(const double aNow) noexcept
{
    for (const MountWorkDone& cDone : s_work.TakeDone())
    {
        if (cDone.What == Work::Seat)
        {
            // Only the ride and decision it was done for.
            const auto seatIt = m_seats.find(cDone.RiderServerId);
            if (seatIt == m_seats.end() || seatIt->second.Ticket != cDone.Ticket || seatIt->second.Ride != cDone.Ride)
                continue;
            seatIt.value().State.Climbing = true;
            seatIt.value().State.ClimbAt = aNow;
            m_everyFrame = true;
        }
        else if (cDone.What == Work::Exit)
        {
            // Told to ride again meanwhile: off the ride it was climbing off at once, as it gets on again (OnNotifyMount). That ride
            // only: once the new one is in place, this takes nothing away.
            if (m_seats.count(cDone.RiderServerId))
            {
                QueueRelease(cDone.RiderServerId, cDone.RiderFormId, cDone.RiderHandle, cDone.Ride);
                continue;
            }
            m_leaving[cDone.RiderFormId] = {aNow, cDone.RiderServerId, cDone.RiderHandle, cDone.Ride, cDone.MountServerId};
            m_everyFrame = true;
        }
    }
}

void MountService::MarkSeated(const entt::entity aRider, const entt::entity aMount) noexcept
{
    if (auto* pInterpolation = m_world.try_get<InterpolationComponent>(aRider))
        pInterpolation->SeatedOn = aMount;
}

void MountService::UpdateSeats(const double aNow) noexcept
{
    for (auto it = m_seats.begin(); it != m_seats.end();)
    {
        const uint32_t cRiderServerId = it->first;
        CopySeat& seat = it.value();

        const auto riderEntity = RemoteByServerId(m_world, cRiderServerId);
        if (!riderEntity)
        {
            // Handed to this game while seated: it is ours to watch now, and its new owner tells the others at once.
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            for (const auto cEntity : localView)
            {
                if (localView.get<LocalComponent>(cEntity).Id != cRiderServerId)
                    continue;

                std::optional<entt::entity> mountEntity;
                for (const auto cMount : m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>))
                {
                    uint32_t id = 0;
                    uint32_t epoch = 0;
                    if (ServerIdentity(m_world, cMount, id, epoch) && id == seat.MountServerId)
                    {
                        mountEntity = cMount;
                        break;
                    }
                }

                if (seat.State.Seated && mountEntity)
                {
                    RiderWatch watch{};
                    watch.MountFormId = m_world.get<FormIdComponent>(*mountEntity).Id;
                    watch.AlreadyOn = true;
                    watch.State.StartedAt = aNow;
                    m_watches[localView.get<FormIdComponent>(cEntity).Id] = watch;
                }
                break;
            }

            s_work.Retire(cRiderServerId);
            it = m_seats.erase(it);
            continue;
        }

        Actor* pRider = ActorByFormId(m_world.get<FormIdComponent>(*riderEntity).Id);
        const auto mountEntity = RemoteByServerId(m_world, seat.MountServerId);
        Actor* pMount = mountEntity ? ActorByFormId(m_world.get<FormIdComponent>(*mountEntity).Id) : nullptr;
        if (!pRider || pRider->IsDeleted())
        {
            MarkSeated(*riderEntity, entt::null);
            s_work.Retire(cRiderServerId);
            it = m_seats.erase(it);
            continue;
        }

        // Looked at again before anything is done: the owners the ride was told under, and the actors here it was tried on.
        bool held = !mountEntity;
        if (mountEntity)
        {
            const uint32_t cRiderHandle = HandleOf(pRider);
            const uint32_t cMountHandle = HandleOf(pMount);
            switch (MountSync::CheckBinding(
                seat.Bound, m_world.get<RemoteComponent>(*riderEntity).OwnershipEpoch, m_world.get<RemoteComponent>(*mountEntity).OwnershipEpoch, cRiderHandle, cMountHandle))
            {
            case MountSync::BindingStep::Hold:
                // Nothing decided under the old owners is done any more.
                held = true;
                seat.Ticket = s_work.Issue(cRiderServerId);
                break;
            case MountSync::BindingStep::Restart:
                // The copy, or its horse, was made again here: off the old one, and seated again on the actors here now.
                spdlog::debug("Mount: copy {:X} (server id {:X}) or its horse is another actor here now; its ride starts over", pRider->formID, cRiderServerId);
                QueueRelease(cRiderServerId, seat.RiderFormId, seat.Bound.RiderHandle, seat.Ride);
                MountSync::Restart(seat.State);
                seat.Bound.RiderHandle = 0;
                seat.Bound.MountHandle = 0;
                seat.Ticket = s_work.Issue(cRiderServerId);
                [[fallthrough]];
            case MountSync::BindingStep::Keep:
                if (!seat.Bound.RiderHandle)
                    seat.Bound.RiderHandle = cRiderHandle;
                if (!seat.Bound.MountHandle)
                    seat.Bound.MountHandle = cMountHandle;
                seat.RiderFormId = pRider->formID;
                break;
            }
        }

        const bool cPairReady = IsLiving(pRider) && IsLiving(pMount) && pMount->IsMount() && SamePlace(pRider, pMount) && !m_world.get<RemoteComponent>(*riderEntity).DeadAtOwner &&
                                MountSync::InSeatRange(DistanceSquared(pRider, pMount));
        const bool cOnMount = pRider->IsOnMount();

        MountWork work{};
        work.RiderServerId = cRiderServerId;
        work.RiderFormId = pRider->formID;
        work.RiderHandle = seat.Bound.RiderHandle;
        work.MountFormId = pMount ? pMount->formID : 0;
        work.MountHandle = seat.Bound.MountHandle;
        work.Stamp.Ticket = seat.Ticket;
        work.Ride = seat.Ride;

        // Shown climbing on: in the saddle by now, or put there at once.
        if (seat.State.Climbing)
        {
            const MountSync::ClimbStep cClimb = cOnMount ? MountSync::NextClimbStep(SitState(pRider), aNow - seat.State.ClimbAt) : MountSync::ClimbStep::Done;
            if (cClimb != MountSync::ClimbStep::Wait)
            {
                seat.State.Climbing = false;
                if (cClimb == MountSync::ClimbStep::Instant && !held)
                {
                    spdlog::debug(
                        "Mount: copy {:X} (server id {:X}) still not in the saddle {:.1f} s into its climb (sit state {}); put there at once", pRider->formID, cRiderServerId,
                        aNow - seat.State.ClimbAt, SitState(pRider));
                    work.What = Work::Saddle;
                    QueueWork(work);
                }
                else if (cOnMount)
                    spdlog::debug("Mount: copy {:X} (server id {:X}) climbed on in {:.2f} s", pRider->formID, cRiderServerId, aNow - seat.State.ClimbAt);
            }
        }

        MountSync::SeatStep step = MountSync::NextSeatStep(seat.State, cPairReady, cOnMount, aNow);
        // A ride whose rider or horse changed owner is not tried again until its new owner says it rides on.
        if (held && (step == MountSync::SeatStep::Try || step == MountSync::SeatStep::GiveUp))
            step = MountSync::SeatStep::Wait;

        switch (step)
        {
        case MountSync::SeatStep::Try:
            ++seat.State.Tries;
            seat.State.TriedAt = aNow;
            // A mount package is no use here: a copy runs no AI to act on it, and the half-started ride bars the next one. The game's
            // quick mount needs neither: SetVehicle names the horse it takes. The graph then climbs on, as its rider just did, or is
            // put straight into the saddle (RunQueuedWork).
            work.What = Work::Seat;
            work.Climb = MountSync::ClimbsOnTry(seat.State, aNow);
            work.Try = seat.State.Tries;
            QueueWork(work);
            break;
        case MountSync::SeatStep::Seated:
            seat.State.Seated = true;
            seat.State.GaveUp = false;
            // Seated once: a later seat on this ride (thrown off, put back) does not climb on again.
            seat.State.ShowClimb = false;
            spdlog::debug("Mount: copy {:X} (server id {:X}) seated on {:X} after {} tries", pRider->formID, cRiderServerId, pMount ? pMount->formID : 0, seat.State.Tries);
            break;
        case MountSync::SeatStep::Unseated: seat.State.Seated = false; break;
        case MountSync::SeatStep::GiveUp:
            seat.State.GaveUp = true;
            // The horse named by the last try is not left on a copy that walks.
            work.What = Work::ClearVehicle;
            QueueWork(work);
            spdlog::debug("Mount: copy {:X} (server id {:X}) could not be seated; it stays on foot", pRider->formID, cRiderServerId);
            break;
        case MountSync::SeatStep::Release:
            MarkSeated(*riderEntity, entt::null);
            s_work.Retire(cRiderServerId);
            if (cOnMount)
                QueueRelease(cRiderServerId, pRider->formID, HandleOf(pRider), seat.Ride);
            it = m_seats.erase(it);
            continue;
        case MountSync::SeatStep::Wait: break;
        }

        // On its horse, the copy is placed at its horse's playback tick (CharacterService::RunRemoteUpdates).
        MarkSeated(*riderEntity, mountEntity && cOnMount ? *mountEntity : entt::entity{entt::null});
        ++it;
    }
}

bool MountService::LetCopiesOff(const uint32_t aMountServerId) noexcept
{
    bool letOff = false;
    for (auto it = m_seats.begin(); it != m_seats.end();)
    {
        if (it->second.MountServerId != aMountServerId)
        {
            ++it;
            continue;
        }
        if (const auto riderEntity = RemoteByServerId(m_world, it->first))
            MarkSeated(*riderEntity, entt::null);
        s_work.Retire(it->first);
        QueueRelease(it->first, it->second.RiderFormId, it->second.Bound.RiderHandle, it->second.Ride);
        spdlog::debug("Mount: copy {:X} (server id {:X}) let off horse server id {:X}, now this game's", it->second.RiderFormId, it->first, aMountServerId);
        it = m_seats.erase(it);
        letOff = true;
    }
    for (auto it = m_leaving.begin(); it != m_leaving.end();)
    {
        if (it->second.MountServerId != aMountServerId)
        {
            ++it;
            continue;
        }
        QueueRelease(it->second.RiderServerId, it->first, it->second.RiderHandle, it->second.Ride);
        spdlog::debug(
            "Mount: copy {:X} (server id {:X}) let off horse server id {:X} before its climb off is done, now this game's", it->first, it->second.RiderServerId, aMountServerId);
        it = m_leaving.erase(it);
        letOff = true;
    }
    return letOff;
}

void MountService::QueueRelease(const uint32_t aRiderServerId, const uint32_t aRiderFormId, const uint32_t aRiderHandle, const uint32_t aRide, const bool aSessionOver) noexcept
{
    if (!aRiderHandle || !aRide)
        return;

    MountWork work{};
    work.What = Work::Release;
    work.RiderServerId = aRiderServerId;
    work.RiderFormId = aRiderFormId;
    work.RiderHandle = aRiderHandle;
    work.Stamp.Undoes = aRide;
    work.Ride = aRide;
    work.SessionOver = aSessionOver;
    QueueWork(work);
}

void MountService::RunQueuedWork() noexcept
{
    RunHorseWork(s_shares);
    SettleOwedOwners();
    RunHorseWork(s_rides);

    TiltedPhoques::Vector<MountWork> work = s_work.TakeWork();
    if (work.empty())
        return;

    static BSFixedString s_enter("HorseEnter");
    static BSFixedString s_enterInstant("HorseEnterInstant");
    static BSFixedString s_exit("HorseExit");

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<MountWork> held;
    for (const MountWork& cWork : work)
    {
        // Looked at again for each piece, before anything is looked up: a load or the exit can start partway through.
        if (IsProcessExiting())
        {
            s_work.Clear();
            return;
        }

        // Read before the lookup, so that nothing is looked up during a load.
        const CopyWork::Moment cBefore = LoadMomentNow();
        // Held while the work runs, so that the game cannot free it meanwhile.
        const RetainedReference cRiderReference(cBefore.Loading || cBefore.Exiting ? 0 : cWork.RiderHandle);
        Actor* pRider = WorkActor(cRiderReference, cWork.RiderFormId);
        CopyWork::Found found{};
        found.Loading = cBefore.Loading;
        found.LoadingScreens = cBefore.LoadingScreens;
        found.Actor = pRider && pRider->GetNiNode();
        found.Copy = IsCopy(pRider);
        found.Live = s_work.Live(cWork.RiderServerId, cWork.Stamp.Ticket);
        found.InPlace = s_work.InPlace(cWork.RiderServerId);
        found.WaitedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - cWork.QueuedAt).count());

        const CopyWork::Verdict cFirst = CopyWork::Check(cWork.Stamp, found, cWork.SessionOver);
        // Looked at again right before the engine is called, in the same step.
        const CopyWork::Verdict cVerdict = cFirst == CopyWork::Verdict::kDo ? CopyWork::Recheck(cWork.Stamp, cBefore, LoadMomentNow()) : cFirst;

        // The deletion handed over is done once the copy is off its horse; a ridden copy that cannot be taken off yet waits.
        if (cWork.What == Work::ReleaseThenDelete)
        {
            const bool cRidden = pRider && (pRider->IsOnMount() || found.InPlace == cWork.Stamp.Undoes);
            switch (CopyWork::StepForDelete(cFirst, cVerdict, found, pRider != nullptr, cRidden))
            {
            case CopyWork::DeleteStep::kReleaseThenDelete:
                ReleaseNow(pRider);
                s_work.Place(cWork.RiderServerId, 0);
                pRider->Delete();
                break;
            case CopyWork::DeleteStep::kDelete: pRider->Delete(); break;
            case CopyWork::DeleteStep::kWait: held.push_back(cWork); continue;
            case CopyWork::DeleteStep::kNothing: break;
            }
            ForgetDeletion(cWork.RiderHandle);
            continue;
        }

        if (cVerdict == CopyWork::Verdict::kHold)
        {
            held.push_back(cWork);
            continue;
        }

        if (cVerdict == CopyWork::Verdict::kDrop)
            continue;

        switch (cWork.What)
        {
        case Work::Seat:
        {
            // The horse too: the same actor, alive, a mount, and still another game's.
            const RetainedReference cMountReference(cWork.MountHandle);
            Actor* pMount = WorkActor(cMountReference, cWork.MountFormId);
            if (!IsLiving(pRider) || !IsLiving(pMount) || !pMount->IsMount() || !IsCopy(pMount))
                break;
            // And again after the horse's lookup.
            if (CopyWork::Recheck(cWork.Stamp, cBefore, LoadMomentNow()) != CopyWork::Verdict::kDo)
                break;

            pRider->SetVehicle(pMount);
            pRider->MountVehicleQuick();
            if (!pRider->IsOnMount())
            {
                // The horse SetVehicle named is not left on a copy that stays on foot: it marks the horse as this copy's.
                pRider->SetVehicle(nullptr);
                spdlog::debug("Mount: the game would not put copy {:X} (server id {:X}) on {:X} at try {}", pRider->formID, cWork.RiderServerId, pMount->formID, cWork.Try);
                break;
            }
            s_work.Place(cWork.RiderServerId, cWork.Ride);
            const bool cClimbs = cWork.Climb && pRider->SendAnimationEvent(&s_enter);
            const bool cTaken = cClimbs || pRider->SendAnimationEvent(&s_enterInstant);
            spdlog::debug(
                "Mount: copy {:X} (server id {:X}) on {:X} at try {}; its graph {} {}", pRider->formID, cWork.RiderServerId, pMount->formID, cWork.Try,
                cTaken ? "took" : "did not take", cClimbs ? "the climb" : "the saddle");
            if (cClimbs)
                s_work.Give({Work::Seat, cWork.RiderServerId, cWork.Stamp.Ticket, pRider->formID, cWork.RiderHandle, cWork.Ride, true});
            break;
        }
        case Work::Saddle:
            if (pRider->IsOnMount())
            {
                const bool cTaken = pRider->SendAnimationEvent(&s_enterInstant);
                spdlog::debug("Mount: copy {:X} (server id {:X}) put in the saddle at once ({})", pRider->formID, cWork.RiderServerId, cTaken ? "taken" : "not taken");
            }
            break;
        case Work::ClearVehicle:
            if (!pRider->IsOnMount())
                pRider->SetVehicle(nullptr);
            break;
        case Work::Exit:
            if (!pRider->IsOnMount())
            {
                s_work.Place(cWork.RiderServerId, 0);
                break;
            }
            if (cWork.Climb && pRider->SendAnimationEvent(&s_exit))
            {
                spdlog::debug("Mount: copy {:X} (server id {:X}) climbs off", pRider->formID, cWork.RiderServerId);
                s_work.Give({Work::Exit, cWork.RiderServerId, 0, pRider->formID, cWork.RiderHandle, cWork.Ride, true, cWork.MountServerId});
                break;
            }
            ReleaseNow(pRider);
            s_work.Place(cWork.RiderServerId, 0);
            break;
        case Work::Release:
            ReleaseNow(pRider);
            s_work.Place(cWork.RiderServerId, 0);
            break;
        case Work::ReleaseThenDelete: break;
        }
    }
    s_work.Hold(held);
}

void MountService::SendLegacyMount(const uint32_t aRiderFormId, const uint32_t aMountFormId) const noexcept
{
    const auto riderEntity = EntityByFormId(m_world, aRiderFormId);
    const auto mountEntity = EntityByFormId(m_world, aMountFormId);
    const auto* pRiderLocal = riderEntity ? m_world.try_get<LocalComponent>(*riderEntity) : nullptr;
    uint32_t mountServerId = 0;
    uint32_t mountEpoch = 0;
    if (!pRiderLocal || !mountEntity || !ServerIdentity(m_world, *mountEntity, mountServerId, mountEpoch))
        return;

    MountRequest request{};
    request.RiderId = pRiderLocal->Id;
    request.RiderOwnershipEpoch = pRiderLocal->OwnershipEpoch;
    request.MountId = mountServerId;
    request.MountOwnershipEpoch = mountEpoch;
    if (MountSync::Valid({request.RiderId, request.RiderOwnershipEpoch, request.MountId, request.MountOwnershipEpoch}))
        m_transport.Send(request);
}

void MountService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected() || !TakesClimbs())
        return;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acEvent.ActivatorId != pPlayer->formID)
        return;
    Actor* pHorse = ActorByFormId(acEvent.ObjectId);
    if (!IsLiving(pHorse) || !pHorse->IsMount() || pPlayer->IsOnMount())
        return;

    // Another game's horse: this game can only seat its player on a horse it runs, so it asks for it (only a party member's is given).
    const auto horseEntity = EntityByFormId(m_world, pHorse->formID);
    const auto* pRemote = horseEntity ? m_world.try_get<RemoteComponent>(*horseEntity) : nullptr;
    if (!pRemote || !pRemote->OwnershipEpoch)
        return;
    for (const auto& [cRiderServerId, cSeat] : m_seats)
        if (cSeat.MountServerId == pRemote->Id)
            return;

    RequestOwnershipClaim request{};
    request.ServerId = pRemote->Id;
    request.ExpectedOwnershipEpoch = pRemote->OwnershipEpoch;
    request.ForRiding = true;
    if (m_transport.Send(request))
    {
        // The claim keeps what it was asked under: the horse handed over later must be this one, with no load since.
        MountSync::RideClaim claim{};
        claim.Until = Now() + kRideClaimFor;
        claim.Stamp.Work.Ticket = s_rides.Issue(pHorse->formID);
        claim.Stamp.Work.LoadingScreens = UI::LoadingScreensQueued();
        claim.Stamp.Session = s_session;
        claim.Stamp.Handle = HandleOf(pHorse);
        m_rideClaims.Ask(pHorse->formID, claim);
        spdlog::debug("Mount: this player is getting on horse {:X} (server id {:X}), which another game runs; asked for it", pHorse->formID, pRemote->Id);
    }
}

void MountService::UpdateRideClaims(const double aNow) noexcept
{
    // A Ride the main thread did or dropped spends its claim.
    for (const HorseWorkDone& cDone : s_rides.TakeDone())
        if (m_rideClaims.Spent(cDone.HorseFormId, cDone.Ticket))
            s_rides.Retire(cDone.HorseFormId);
    if (m_rideClaims.Empty())
        return;

    // Out of the party, or sharing turned off: no horse is got on for a claim made before, its Ride queued or not. The time a claim
    // waits is up before a late handover is looked at.
    const bool cStillShared = m_world.GetPartyService().IsInParty();
    m_rideClaims.Update(
        aNow, cStillShared,
        [this](const uint32_t aHorseFormId)
        {
            const auto horseEntity = EntityByFormId(m_world, aHorseFormId);
            const auto* pLocal = horseEntity ? m_world.try_get<LocalComponent>(*horseEntity) : nullptr;
            return MountSync::RideReady(pLocal != nullptr, pLocal && LetCopiesOff(pLocal->Id));
        },
        [](const uint32_t aHorseFormId, const MountSync::RideClaim& acClaim) { QueueRide(aHorseFormId, acClaim); },
        [](const uint32_t aHorseFormId)
        {
            s_rides.Retire(aHorseFormId);
            spdlog::debug("Mount: horse {:X} is not got on for this player's claim any more", aHorseFormId);
        });
}

void MountService::UpdateSharedHorses() noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& members = party.GetPartyMembers();
    const auto cInParty = [&](const uint32_t aPlayerId)
    {
        return party.IsInParty() && std::find(members.begin(), members.end(), aPlayerId) != members.end();
    };

    // A share the main thread did not make is forgotten, and made again below if the horse should still be shared.
    for (const HorseWorkDone& cDone : s_shares.TakeDone())
        if (const auto it = m_sharedHorses.find(cDone.HorseFormId); it != m_sharedHorses.end() && it->second.Share == cDone.Ticket)
            m_sharedHorses.erase(it);

    // A horse a party member's copy sits on here.
    for (const auto& [cRiderServerId, cSeat] : m_seats)
    {
        if (!cSeat.State.Seated)
            continue;
        const auto riderEntity = RemoteByServerId(m_world, cRiderServerId);
        const auto mountEntity = RemoteByServerId(m_world, cSeat.MountServerId);
        const auto* pPlayer = riderEntity ? m_world.try_get<PlayerComponent>(*riderEntity) : nullptr;
        if (!pPlayer || !mountEntity)
            continue;
        const uint32_t cHorse = m_world.get<FormIdComponent>(*mountEntity).Id;
        if (m_sharedHorses.count(cHorse) || !MountSync::SharesHorse(true, true, cInParty(pPlayer->Id)))
            continue;
        const uint32_t cHandle = HandleOf(ActorByFormId(cHorse));
        if (!cHandle)
            continue;
        const uint32_t cShare = s_shares.Issue(cHorse);
        m_sharedHorses[cHorse] = {pPlayer->Id, cShare, cHandle};
        QueueHorseWork(s_shares, MountSync::HorseJob::Share, cHorse, cHandle, cShare);
    }

    // No longer: the rider left the party, sharing was turned off, or that form id names another horse now (after a load).
    for (auto it = m_sharedHorses.begin(); it != m_sharedHorses.end();)
    {
        if (MountSync::SharesHorse(true, true, cInParty(it->second.PlayerId)) && HandleOf(ActorByFormId(it->first)) == it->second.Handle)
        {
            ++it;
            continue;
        }
        QueueUnshare(it->first, it->second.Handle, it->second.Share);
        it = m_sharedHorses.erase(it);
    }
}
