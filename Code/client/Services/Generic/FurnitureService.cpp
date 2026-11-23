#include <Services/GameExtension.h>
#include <Services/FurnitureService.h>
#include <Helpers/PerfScope.h>
#include <Services/CopyWorkGate.h>
#include <Services/CopyWorkQueue.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/RequestNpcFurniture.h>
#include <Messages/NotifyNpcFurniture.h>
#include <Messages/RequestPlayerFurniture.h>
#include <Messages/NotifyPlayerFurniture.h>

#include <AI/AIProcess.h>
#include <Forms/TESObjectCELL.h>
#include <Games/TES.h>
#include <Structs/GridCellCoords.h>
#include <Systems/ModSystem.h>
#include <World.h>
#include <Utils.h>

#include <Games/ActorExtension.h>
#include <Games/Memory.h>
#include <Interface/UI.h>
#include <PlayerCharacter.h>
#include <Copies/EpochMiss.h>

namespace
{
// Words per message, under what a game takes in one (NpcFurnitureWords::kMax).
constexpr size_t kSeatWordsPerMessage = 400;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

using FurnitureHandle = BSPointerHandle<TESObjectREFR>;

// The furniture an actor's process holds (AE 39916, VR 0x140688370): the handle at middle-high process +0x208 on both,
// or the invalid handle when the process has no middle-high part.
TP_THIS_FUNCTION(TGetCurrentFurniture, FurnitureHandle*, AIProcess, FurnitureHandle* apOut);
// The furniture marker an actor's process holds (AE 39914, VR 0x140688330): middle-high process +0x2E8 on both, else 0.
TP_THIS_FUNCTION(TGetFurnitureMarker, uint32_t, AIProcess);
// The game's sit/sleep state change (AE 39912, VR 0x1406878D0). Into 3 (sitting) or 7 (sleeping) it reserves the marker,
// attaches the actor to it, raises the furniture enter event and keeps the furniture and marker on the process; into 0
// it raises the exit event and stands the actor. The sit package calls it when its animation settles, and so does the
// load of a cell whose NPC starts seated. A copy runs no package, so this is its only way in.
TP_THIS_FUNCTION(TSetSitSleepState, void, AIProcess, Actor* apActor, uint32_t aState, FurnitureHandle* apFurniture, uint32_t aMarker);
// The same change on the furniture the actor's process already holds (AE 39000, VR 0x140645A40); what the actor's graph
// events call to stand it up.
TP_THIS_FUNCTION(TSetSitSleepStateHere, void, Actor, uint32_t aState);

uint8_t SitSleepState(const Actor* apActor) noexcept
{
    // SIT_SLEEP_STATE, bits 14-17 of the actor state's first flags (as AE 39912 reads it).
    return static_cast<uint8_t>((apActor->actorState.flags1 >> 14) & 0xF);
}

//! Whether the actor has the process the game seats with: SetSitSleepState does nothing without a middle-high part.
bool CanSeat(const Actor* apActor) noexcept
{
    return apActor->currentProcess && apActor->currentProcess->middleProcess;
}

uint32_t CurrentFurnitureHandle(Actor* apActor) noexcept
{
    if (!CanSeat(apActor))
        return 0;
    POINTER_SKYRIMSE(TGetCurrentFurniture, s_getCurrentFurniture, 39916, 39916);
    FurnitureHandle handle{};
    const FurnitureHandle* pHandle = TiltedPhoques::ThisCall(s_getCurrentFurniture.Get(), apActor->currentProcess, &handle);
    return pHandle && *pHandle ? pHandle->handle.iBits : 0;
}

TESObjectREFR* CurrentFurniture(Actor* apActor) noexcept
{
    const uint32_t cHandle = CurrentFurnitureHandle(apActor);
    return cHandle ? TESObjectREFR::GetByHandle(cHandle) : nullptr;
}

uint32_t FurnitureMarker(Actor* apActor) noexcept
{
    if (!CanSeat(apActor))
        return 0;
    POINTER_SKYRIMSE(TGetFurnitureMarker, s_getFurnitureMarker, 39914, 39914);
    return TiltedPhoques::ThisCall(s_getFurnitureMarker.Get(), apActor->currentProcess);
}

void Seat(Actor* apActor, TESObjectREFR* apFurniture, const uint8_t aState, const uint32_t aMarker) noexcept
{
    POINTER_SKYRIMSE(TSetSitSleepState, s_setSitSleepState, 39912, 39912);
    FurnitureHandle handle = apFurniture->GetHandle();
    TiltedPhoques::ThisCall(s_setSitSleepState.Get(), apActor->currentProcess, apActor, static_cast<uint32_t>(aState), &handle, aMarker);
}

void Stand(Actor* apActor) noexcept
{
    POINTER_SKYRIMSE(TSetSitSleepStateHere, s_setSitSleepStateHere, 39000, 39000);
    TiltedPhoques::ThisCall(s_setSitSleepStateHere.Get(), apActor, static_cast<uint32_t>(NpcFurniture::kStateNone));
}

//! Whether the reference is furniture: a rider's process can name its horse, which is no seat.
bool IsFurniture(const TESObjectREFR* apReference) noexcept
{
    return apReference && apReference->baseForm && apReference->baseForm->formType == FormType::Furniture;
}

//! Whether the actor rides something, which also reads as seated.
bool IsMounted(const Actor* apActor) noexcept
{
    return Actor::CanReadMount() && apActor->IsOnMount();
}

bool ChoosingSeat(const uint8_t aState) noexcept
{
    return aState == 1 || aState == 2 || aState == 5 || aState == 6;
}

Actor* ActorOf(World& aWorld, const entt::entity aEntity) noexcept
{
    const auto* pFormId = aWorld.try_get<FormIdComponent>(aEntity);
    return pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
}

uint32_t HandleOf(const TESObjectREFR* apReference) noexcept
{
    return apReference ? apReference->GetHandle().handle.iBits : 0;
}

bool InRecordedSeat(Actor* apActor, const uint32_t aFurnitureId, const uint32_t aMarker) noexcept
{
    const RetainedReference cHeld(CurrentFurnitureHandle(apActor));
    const TESObjectREFR* pHeld = cHeld.Get();
    return IsFurniture(pHeld) && pHeld->formID == aFurnitureId && FurnitureMarker(apActor) == aMarker;
}

bool InFurnitureCell(const TESObjectREFR* apFurniture, const uint32_t aCellFormId) noexcept
{
    if (!apFurniture || !aCellFormId)
        return false;
    const TESObjectCELL* pCell = apFurniture->GetParentCellEx();
    if (pCell && pCell->formID == aCellFormId)
        return pCell->IsAttached();

    // A persistent exterior reference can name the worldspace's persistent cell.
    auto* pWorldSpace = apFurniture->GetWorldSpace();
    auto* pModManager = pWorldSpace ? ModManager::Get() : nullptr;
    if (!pModManager)
        return false;
    const auto cGrid = GridCellCoords::CalculateGridCellCoords(apFurniture->position.x, apFurniture->position.y);
    pCell = pModManager->GetCellFromCoordinates(cGrid.X, cGrid.Y, pWorldSpace, false);
    return pCell && pCell->formID == aCellFormId && pCell->IsAttached();
}

//! The furniture placed at run time here that a word names: of its base form (aBaseFormId), within NpcFurniture::kRuntimeSeatReach of
//! where it stands at its owner's, in the copy's cell or, outside, the cell at that place. Null when there is none here.
TESObjectREFR* FindRuntimeFurniture(const Actor* apCopy, const uint32_t aBaseFormId, const RuntimeSeat& acPlacement) noexcept
{
    if (!apCopy || !aBaseFormId || !acPlacement.Placed)
        return nullptr;
    Vector<TESObjectCELL*> cells;
    if (TESObjectCELL* pCell = apCopy->GetParentCellEx(); pCell && pCell->IsAttached())
        cells.push_back(pCell);
    if (auto* pWorldSpace = apCopy->GetWorldSpace(); pWorldSpace && ModManager::Get())
    {
        const auto cGrid = GridCellCoords::CalculateGridCellCoords(static_cast<float>(acPlacement.X), static_cast<float>(acPlacement.Y));
        TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(cGrid.X, cGrid.Y, pWorldSpace, false);
        if (pCell && pCell->IsAttached() && std::find(cells.begin(), cells.end(), pCell) == cells.end())
            cells.push_back(pCell);
    }

    Vector<NpcFurniture::RuntimeCandidate> candidates;
    for (const TESObjectCELL* pCell : cells)
        for (TESObjectREFR* pRef : pCell->GetRefsByFormTypes({FormType::Furniture}))
            if (pRef && pRef->IsTemporary() && pRef->baseForm && pRef->baseForm->formID == aBaseFormId && !pRef->IsDisabled() && !pRef->IsDeleted())
                candidates.push_back({pRef->formID, pRef->position.x, pRef->position.y, pRef->position.z});
    const uint32_t cFormId = NpcFurniture::NearestRuntimeSeat(candidates, acPlacement);
    return cFormId ? Cast<TESObjectREFR>(TESForm::GetById(cFormId)) : nullptr;
}

//! A copy seated or stood on the main thread (CopyWork.h).
struct SeatWork
{
    bool Stand{};
    bool LocalPlayer{};
    uint32_t ServerId{};
    uint32_t CopyFormId{};
    uint32_t CopyHandle{};
    uint32_t FurnitureFormId{};
    uint32_t FurnitureHandle{};
    uint32_t CellFormId{};
    uint8_t State{};
    uint32_t Marker{};
    CopyWork::Stamp Stamp{};
    //! Which of the service's seats and stands this is (NpcFurniture::WorkSerials): only its own receipt is taken.
    uint32_t Serial{};
    std::chrono::steady_clock::time_point QueuedAt{};
};

//! What came of it: every piece of work comes back once, done or not.
struct SeatWorkDone
{
    bool Stand{};
    bool Done{};
    uint32_t ServerId{};
    uint32_t Serial{};
    //! The decision a seat was done under, which is now the one in place.
    uint32_t Ticket{};
    uint32_t CopyHandle{};
    uint32_t FurnitureFormId{};
    uint8_t AskedState{};
    uint32_t AskedMarker{};
    uint8_t ReadState{};
    uint32_t ReadFurnitureFormId{};
    uint32_t ReadMarker{};
};

CopyWorkQueue<SeatWork, SeatWorkDone> s_work;

// The copies this game seated, by handle: written on the main thread where they are seated and stood, read by the character save.
std::mutex s_seatedHereLock;
TiltedPhoques::Set<uint32_t> s_seatedHere;

void NoteSeatedHere(const uint32_t aHandle, const bool aSeated) noexcept
{
    std::scoped_lock lock(s_seatedHereLock);
    if (!aSeated)
    {
        s_seatedHere.erase(aHandle);
        return;
    }
    // Copies gone since are let go of here, not one by one.
    for (auto it = s_seatedHere.begin(); it != s_seatedHere.end();)
        it = TESObjectREFR::GetByHandle(*it) ? std::next(it) : s_seatedHere.erase(it);
    s_seatedHere.insert(aHandle);
}

//! The reference a retained reference names, if it is still the one a piece of work was for, not deleted, with its 3D.
TESObjectREFR* WorkReference(const RetainedReference& acReference, const uint32_t aFormId) noexcept
{
    TESObjectREFR* pReference = acReference.Get();
    return pReference && pReference->formID == aFormId && !pReference->IsDeleted() && pReference->GetNiNode() ? pReference : nullptr;
}

// Where the middle-high process keeps the furniture an actor sits in and its marker: the fields AE 39916 and 39914 read (+0x208 and
// +0x2E8, the same on VR: VRAddressOverrides.h), and that AIProcess's save (AE 1.7.104 0x1406F72C0) writes, the furniture as a reference.
constexpr size_t kFurnitureHandleAt = 0x208;
constexpr size_t kFurnitureMarkerAt = 0x2E8;

uint32_t* MiddleProcessField(Actor* apActor, const size_t aOffset) noexcept
{
    if (!apActor->currentProcess || !apActor->currentProcess->middleProcess)
        return nullptr;
    return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(apActor->currentProcess->middleProcess) + aOffset);
}
} // namespace

bool GameExtension::ReleaseFurnitureSeat(Actor* apActor) noexcept
{
    if (!apActor)
        return true;
    const uint32_t cHandle = HandleOf(apActor);
    {
        std::scoped_lock lock(s_seatedHereLock);
        if (!s_seatedHere.count(cHandle))
            return true;
    }
    if (LoadingScreenUp() || IsProcessExiting() || !CanSeat(apActor))
        return false;
    const auto* pFurniture = CurrentFurniture(apActor);
    if (IsFurniture(pFurniture) && !IsMounted(apActor) && SitSleepState(apActor) != NpcFurniture::kStateNone)
        Stand(apActor);
    if (IsFurniture(CurrentFurniture(apActor)) && SitSleepState(apActor) != NpcFurniture::kStateNone)
        return false;
    NoteSeatedHere(cHandle, false);
    return true;
}

void FurnitureService::RunQueuedWork() noexcept
{
    TiltedPhoques::Vector<SeatWork> work = s_work.TakeWork();
    if (work.empty())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<SeatWork> held;
    for (const SeatWork& cWork : work)
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
        const RetainedReference cCopyReference(cBefore.Loading || cBefore.Exiting ? 0 : cWork.CopyHandle);
        Actor* pCopy = Cast<Actor>(WorkReference(cCopyReference, cWork.CopyFormId));
        const ActorExtension* pExtension = pCopy ? pCopy->GetExtension() : nullptr;
        CopyWork::Found found{};
        found.Loading = cBefore.Loading;
        found.LoadingScreens = cBefore.LoadingScreens;
        found.Actor = pCopy != nullptr;
        found.Copy = cWork.LocalPlayer ? pCopy && pCopy == PlayerCharacter::Get() : pExtension && pExtension->IsRemote();
        found.Live = s_work.Live(cWork.ServerId, cWork.Stamp.Ticket);
        found.InPlace = s_work.InPlace(cWork.ServerId);
        found.WaitedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(cNow - cWork.QueuedAt).count());

        const CopyWork::Verdict cFirst = CopyWork::Check(cWork.Stamp, found, false);
        // Looked at again right before the engine is called, in the same step.
        const CopyWork::Verdict cVerdict = cFirst == CopyWork::Verdict::kDo ? CopyWork::Recheck(cWork.Stamp, cBefore, LoadMomentNow()) : cFirst;
        if (cVerdict == CopyWork::Verdict::kHold)
        {
            held.push_back(cWork);
            continue;
        }
        if (cVerdict == CopyWork::Verdict::kDo && pCopy && GameExtension::Reserved(pCopy->formID))
        {
            s_work.Retire(cWork.ServerId);
            s_work.Place(cWork.ServerId, 0);
            GameExtension::ReleaseFurnitureSeat(pCopy);
            continue;
        }
        if (cWork.LocalPlayer)
        {
            if (cVerdict == CopyWork::Verdict::kDo && CanSeat(pCopy) && !pCopy->IsDead() && !IsMounted(pCopy))
            {
                const RetainedReference cFurniture(cWork.FurnitureHandle);
                const TESObjectREFR* pFurniture = WorkReference(cFurniture, cWork.FurnitureFormId);
                if (pFurniture && SitSleepState(pCopy) != NpcFurniture::kStateNone && CurrentFurniture(pCopy) == pFurniture && FurnitureMarker(pCopy) == cWork.Marker &&
                    s_work.Live(cWork.ServerId, cWork.Stamp.Ticket) && CopyWork::Recheck(cWork.Stamp, cBefore, LoadMomentNow()) == CopyWork::Verdict::kDo)
                    Stand(pCopy);
            }
            continue;
        }

        SeatWorkDone done{};
        done.Stand = cWork.Stand;
        done.ServerId = cWork.ServerId;
        done.Serial = cWork.Serial;
        done.Ticket = cWork.Stamp.Ticket;
        done.CopyHandle = cWork.CopyHandle;
        done.FurnitureFormId = cWork.FurnitureFormId;
        done.AskedState = cWork.State;
        done.AskedMarker = cWork.Marker;
        if (cVerdict == CopyWork::Verdict::kDo)
        {
            if (cWork.Stand)
            {
                const bool cPlayerSeat = cWork.FurnitureFormId != 0;
                const bool cMounted = cPlayerSeat && IsMounted(pCopy);
                // The action replay can have written standing over the seat this game made, with the copy still in its furniture: the
                // state it was seated in is put back, so that the game's own stand has a seat to undo.
                if (!cPlayerSeat && NpcFurniture::IsSettled(cWork.State) && SitSleepState(pCopy) == NpcFurniture::kStateNone && CanSeat(pCopy) &&
                    IsFurniture(CurrentFurniture(pCopy)) && !IsMounted(pCopy))
                    pCopy->actorState.flags1 = NpcFurniture::WithSeat(pCopy->actorState.flags1, static_cast<uint32_t>(cWork.State) << NpcFurniture::kSitSleepShift);
                if (SitSleepState(pCopy) != NpcFurniture::kStateNone && (!cPlayerSeat || (!cMounted && InRecordedSeat(pCopy, cWork.FurnitureFormId, cWork.Marker))))
                    Stand(pCopy);
                done.ReadState = SitSleepState(pCopy);
                done.Done =
                    !cPlayerSeat || cMounted || done.ReadState == NpcFurniture::kStateNone || (CanSeat(pCopy) && !InRecordedSeat(pCopy, cWork.FurnitureFormId, cWork.Marker));
                if (done.Done)
                {
                    s_work.Place(cWork.ServerId, 0);
                    NoteSeatedHere(cWork.CopyHandle, false);
                }
            }
            else
            {
                const RetainedReference cFurnitureReference(cWork.FurnitureHandle);
                TESObjectREFR* pFurniture = WorkReference(cFurnitureReference, cWork.FurnitureFormId);
                // Still standing, and the furniture still there to sit in, at a marker it can have.
                // And again after the furniture's lookup.
                if (PlayerFurniture::MarkerFits(cWork.Marker) && pFurniture && IsFurniture(pFurniture) && !pFurniture->IsDisabled() && CanSeat(pCopy) && !pCopy->IsDead() &&
                    !IsMounted(pCopy) && (!cWork.CellFormId || InFurnitureCell(pFurniture, cWork.CellFormId)) &&
                    // Standing: its bits read standing, or read seated with no furniture held, as the action replay leaves them.
                    NpcFurniture::CopySeatState(SitSleepState(pCopy), CurrentFurniture(pCopy) != nullptr, false, 0) == NpcFurniture::kStateNone &&
                    CopyWork::Recheck(cWork.Stamp, cBefore, LoadMomentNow()) == CopyWork::Verdict::kDo)
                {
                    Seat(pCopy, pFurniture, cWork.State, cWork.Marker);
                    const TESObjectREFR* pNowFurniture = CurrentFurniture(pCopy);
                    done.Done = true;
                    done.ReadState = SitSleepState(pCopy);
                    done.ReadFurnitureFormId = pNowFurniture ? pNowFurniture->formID : 0;
                    done.ReadMarker = FurnitureMarker(pCopy);
                    const bool cSettled = NpcFurniture::IsSettled(done.ReadState);
                    if (cSettled)
                        s_work.Place(cWork.ServerId, cWork.Stamp.Ticket);
                    NoteSeatedHere(cWork.CopyHandle, cSettled);
                }
            }
        }
        s_work.Give(done);
    }
    s_work.Hold(held);
}

FurnitureService::SeatSaveGuard::SeatSaveGuard(Actor* apActor) noexcept
    : m_pActor(apActor)
{
    if (!apActor)
        return;
    bool seatedHere = false;
    {
        std::scoped_lock lock(s_seatedHereLock);
        seatedHere = !s_seatedHere.empty() && s_seatedHere.count(apActor->GetHandle().handle.iBits) != 0;
    }
    const ActorExtension* pExtension = seatedHere ? apActor->GetExtension() : nullptr;
    m_mask.Lift(
        &apActor->actorState.flags1, MiddleProcessField(apActor, kFurnitureHandleAt), MiddleProcessField(apActor, kFurnitureMarkerAt), seatedHere,
        pExtension && pExtension->IsRemote());
}

FurnitureService::SeatSaveGuard::~SeatSaveGuard() noexcept
{
    if (m_mask.Lifted())
        spdlog::debug("Furniture: copy {:X}, seated here, written to the save standing and out of its furniture", m_pActor->formID);
    // Put back however the save is left (NpcFurniture::SeatSaveMask).
}

FurnitureService::FurnitureService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&FurnitureService::OnUpdate>(this);
    m_npcFurnitureConnection = aDispatcher.sink<NotifyNpcFurniture>().connect<&FurnitureService::OnNotifyNpcFurniture>(this);
    m_playerFurnitureConnection = aDispatcher.sink<NotifyPlayerFurniture>().connect<&FurnitureService::OnNotifyPlayerFurniture>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&FurnitureService::OnDisconnected>(this);
    // A copy changing owner, or becoming this game's, withdraws the seat decided for it at once, before the next frame does it.
    m_remoteAddedConnection = m_world.on_construct<RemoteComponent>().connect<&FurnitureService::OnOwnerChanged>(this);
    m_remoteChangedConnection = m_world.on_update<RemoteComponent>().connect<&FurnitureService::OnOwnerChanged>(this);
    m_remoteRemovedConnection = m_world.on_destroy<RemoteComponent>().connect<&FurnitureService::OnOwnerChanged>(this);
}

void FurnitureService::OnOwnerChanged(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    if (auto* pSeat = aRegistry.try_get<CopySeatComponent>(aEntity); pSeat && pSeat->Ticket)
        pSeat->Ticket = s_work.Issue(pSeat->ServerId);
}

void FurnitureService::OnUpdate(const UpdateEvent&) noexcept
{
    const uint64_t cNow = NowMs();
    if (cNow < m_nextSeatRead)
        return;
    m_nextSeatRead = cNow + NpcFurniture::kReadEveryMs;

    PerfScope perfScope("FurnitureService::OnUpdate");
    TakeDoneWork(cNow);
    // This runs on whichever game thread sends the update, and a load can run beside it: nothing of the game's is read while one does.
    // The copies' words are kept and looked at again once it is over (ReadOwnPlayerSeat looks at the load itself, to let its seat go).
    const auto cMoment = LoadMomentNow();
    const bool cLoading = cMoment.Loading || cMoment.Exiting;
    if (!cLoading)
        UpdateCopySeats(cNow);
    // Only once the server has accepted this player: it kicks a connection that sends anything before.
    if (m_transport.IsOnline())
    {
        ReadOwnPlayerSeat(cNow);
        if (!cLoading)
            ReadOwnSeats(cNow);
    }
}

void FurnitureService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    ClearPlayerSeat(false);
}

void FurnitureService::ClearPlayerSeat(const bool aSendExit) noexcept
{
    if (aSendExit && m_playerSeat.Lease)
        SendPlayerSeat(PlayerFurniture::Action::Exit, NpcFurniture::kStateNone, NowMs());
    else if (aSendExit && m_playerSeat.Sequence && !m_playerSeat.Denied)
        SendPlayerSeat(PlayerFurniture::Action::Cancel, NpcFurniture::kStateNone, NowMs());
    if (m_playerSeat.Epoch)
        s_work.Retire(m_playerSeat.Actor);
    m_playerSeat = {};
}

void FurnitureService::SendPlayerSeat(const PlayerFurniture::Action aAction, const uint8_t aState, const uint64_t aNowMs) noexcept
{
    RequestPlayerFurniture message;
    message.Request = {m_playerSeat.Actor, m_playerSeat.Epoch, ++m_playerSequence, aAction, m_playerSeat.Where, m_playerSeat.Lease};
    if (aAction == PlayerFurniture::Action::Cancel)
        message.Request.EnterSequence = m_playerSeat.Sequence;
    message.Cell = m_playerSeat.Cell;
    if (m_transport.Send(message))
    {
        m_playerSeat.Sequence = message.Request.Sequence;
        m_playerSeat.SentState = aState;
    }
    m_playerSeat.SendAt = aNowMs + NpcFurniture::kHeartbeatMs;
}

void FurnitureService::ReadOwnPlayerSeat(const uint64_t aNowMs) noexcept
{
    const auto cMoment = LoadMomentNow();
    if (cMoment.Loading || cMoment.Exiting)
    {
        ClearPlayerSeat(!cMoment.Exiting);
        return;
    }

    Actor* pPlayer = PlayerCharacter::Get();
    const auto cOwner = pPlayer ? Utils::GetLocalOwnershipToken(pPlayer->formID) : std::nullopt;
    if (!cOwner || !cOwner->OwnershipEpoch || pPlayer->IsDead())
    {
        ClearPlayerSeat(cOwner.has_value());
        return;
    }

    if (GameExtension::Reserved(pPlayer->formID))
    {
        ClearPlayerSeat(true);
        return;
    }
    const uint8_t cState = SitSleepState(pPlayer);
    const bool cSettled = NpcFurniture::IsSettled(cState);
    const RetainedReference cFurniture((cSettled || ChoosingSeat(cState)) ? CurrentFurnitureHandle(pPlayer) : 0);
    const TESObjectREFR* pFurniture = cFurniture.Get();
    const TESObjectCELL* pCell = pPlayer->GetParentCellEx();
    PlayerFurniture::Seat where;
    GameId cell{};
    const auto& cMods = m_world.GetModSystem();
    // Furniture placed at run time is named by its base form and where it stands (RuntimeSeat).
    const bool cPlaced = IsFurniture(pFurniture) && pFurniture->IsTemporary();
    const uint32_t cNameId = !IsFurniture(pFurniture) ? 0 : cPlaced ? pFurniture->baseForm->formID : pFurniture->formID;
    if (!IsFurniture(pFurniture) || pFurniture->IsDisabled() || pFurniture->IsDeleted() || IsMounted(pPlayer) || !pCell || !InFurnitureCell(pFurniture, pCell->formID) ||
        !cMods.GetServerModId(pCell->formID, cell) || !cMods.GetServerModId(cNameId, where.Reference) || (where.Marker = FurnitureMarker(pPlayer)) >= PlayerFurniture::kMarkerCount)
    {
        ClearPlayerSeat(true);
        return;
    }
    if (cPlaced)
        where.Placement = RuntimeSeats::At(pFurniture->position.x, pFurniture->position.y, pFurniture->position.z);

    if (m_playerSeat.Actor != cOwner->ServerId || m_playerSeat.Epoch != cOwner->OwnershipEpoch || m_playerSeat.Cell != cell || m_playerSeat.Where != where)
    {
        ClearPlayerSeat(true);
        m_playerSeat = {cOwner->ServerId, cOwner->OwnershipEpoch, cell, where};
    }

    if (m_playerSeat.Denied)
    {
        if (aNowMs >= m_playerSeat.SendAt)
            QueueDeniedSeat(pPlayer, pFurniture, cMoment.LoadingScreens, aNowMs);
        return;
    }
    if (!m_playerSeat.Lease)
    {
        if (aNowMs >= m_playerSeat.SendAt)
            SendPlayerSeat(PlayerFurniture::Action::Enter, NpcFurniture::kStateNone, aNowMs);
        return;
    }
    if (!cSettled)
        return;
    if (m_playerSeat.SentState != cState)
    {
        const auto cAction = cState == NpcFurniture::kStateSleeping ? PlayerFurniture::Action::Sleep : PlayerFurniture::Action::Sit;
        SendPlayerSeat(cAction, cState, aNowMs);
    }
    else if (aNowMs >= m_playerSeat.SendAt)
    {
        SendPlayerSeat(PlayerFurniture::Action::Refresh, cState, aNowMs);
    }
}

void FurnitureService::HandlePlayerSeatReply(const NotifyPlayerFurniture& acMessage) noexcept
{
    if (!m_transport.IsOnline() || acMessage.Actor != m_playerSeat.Actor || acMessage.Epoch != m_playerSeat.Epoch || acMessage.Where != m_playerSeat.Where ||
        acMessage.Cell != m_playerSeat.Cell || acMessage.Sequence != m_playerSeat.Sequence)
        return;

    if (acMessage.Decision == PlayerFurniture::Verdict::Granted)
    {
        m_playerSeat.Lease = acMessage.Lease;
        return;
    }
    if (acMessage.Decision == PlayerFurniture::Verdict::Rejected)
    {
        m_playerSeat.Lease = 0;
        m_playerSeat.Sequence = 0;
        m_playerSeat.SentState = NpcFurniture::kStateNone;
        return;
    }
    if (acMessage.Decision != PlayerFurniture::Verdict::Occupied || m_playerSeat.Denied)
        return;

    m_playerSeat.Denied = true;
    m_playerSeat.SendAt = 0;
    Utils::ShowHudMessage("Someone else is using that spot.");
}

void FurnitureService::QueueDeniedSeat(Actor* apPlayer, const TESObjectREFR* apFurniture, const uint32_t aLoadingScreens, const uint64_t aNowMs) noexcept
{
    SeatWork work{};
    work.LocalPlayer = true;
    work.ServerId = m_playerSeat.Actor;
    work.CopyFormId = apPlayer->formID;
    work.CopyHandle = HandleOf(apPlayer);
    work.FurnitureFormId = apFurniture->formID;
    work.FurnitureHandle = HandleOf(apFurniture);
    work.Marker = m_playerSeat.Where.Marker;
    work.Stamp = {s_work.Issue(work.ServerId), aLoadingScreens, 0};
    work.QueuedAt = std::chrono::steady_clock::now();
    s_work.Push(work);
    m_playerSeat.SendAt = aNowMs + NpcFurniture::kHeartbeatMs;
}

void FurnitureService::OnNotifyPlayerFurniture(const NotifyPlayerFurniture& acMessage) noexcept
{
    if (acMessage.Sequence)
    {
        HandlePlayerSeatReply(acMessage);
        return;
    }
    const auto cEntity = Utils::FindEntityByServerId(acMessage.Actor);
    const auto* pRemote = cEntity ? m_world.try_get<RemoteComponent>(*cEntity) : nullptr;
    if (!pRemote || !m_world.all_of<PlayerComponent>(*cEntity) || pRemote->OwnershipEpoch != acMessage.Epoch || !acMessage.Lease || !acMessage.Cell)
        return;

    const auto* pCurrent = m_world.try_get<CopySeatComponent>(*cEntity);
    const bool cStanding = acMessage.State == PlayerFurniture::SeatState::Standing;
    if (cStanding ? (!pCurrent || pCurrent->Lease != acMessage.Lease) : (pCurrent && pCurrent->Lease > acMessage.Lease))
        return;

    const auto& cRef = acMessage.Where.Reference;
    const NpcFurniture::Seat cSeat{cRef.ModId, cRef.BaseId, acMessage.Where.Marker, static_cast<uint8_t>(acMessage.State), acMessage.Where.Placement};
    ReadCopySeat(*cEntity, acMessage.Actor, acMessage.Epoch, cSeat, NowMs(), acMessage.Lease, acMessage.Cell);
}

// The owner's side: the furniture each NPC this game runs is settled in, sent when it changes and at each heartbeat.
void FurnitureService::ReadOwnSeats(const uint64_t aNowMs) noexcept
{
    const auto& cModSystem = m_world.GetModSystem();
    const auto cView = m_world.view<FormIdComponent, LocalComponent>(entt::exclude<PlayerComponent>);
    for (const entt::entity cEntity : cView)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(cView.get<FormIdComponent>(cEntity).Id));
        if (!pActor || pActor == PlayerCharacter::Get())
            continue;

        if (GameExtension::Reserved(pActor->formID))
        {
            const auto& local = cView.get<LocalComponent>(cEntity);
            m_seatOutbox.Put(local.Id, {}, local.OwnershipEpoch);
            continue;
        }
        const uint8_t cState = SitSleepState(pActor);
        TESObjectREFR* pFurniture = NpcFurniture::IsSettled(cState) ? CurrentFurniture(pActor) : nullptr;
        uint32_t modId = 0;
        uint32_t baseId = 0;
        const bool cIsFurniture = IsFurniture(pFurniture);
        // Asked only of an NPC that reads as seated, as it is a script call.
        const bool cMounted = cIsFurniture && IsMounted(pActor);
        // Furniture placed at run time is named by its base form and where it stands (RuntimeSeat).
        const bool cPlaced = cIsFurniture && pFurniture->IsTemporary();
        const uint32_t cNameId = !cIsFurniture ? 0 : cPlaced ? pFurniture->baseForm->formID : pFurniture->formID;
        const bool cNamed = cIsFurniture && !cMounted && cModSystem.GetServerModId(cNameId, modId, baseId);
        NpcFurniture::Seat cSeat = NpcFurniture::ReadSeat(cState, pFurniture != nullptr, cIsFurniture, cMounted, cNamed, modId, baseId, cNamed ? FurnitureMarker(pActor) : 0);
        if (cPlaced)
            cSeat = NpcFurniture::WithPlacement(cSeat, RuntimeSeats::At(pFurniture->position.x, pFurniture->position.y, pFurniture->position.z));

        const LocalComponent& cLocal = cView.get<LocalComponent>(cEntity);
        m_seatOutbox.Put(cLocal.Id, cSeat, cLocal.OwnershipEpoch);
    }

    const auto cWords = m_seatOutbox.Take(aNowMs);
    for (size_t first = 0; first < cWords.size(); first += kSeatWordsPerMessage)
    {
        RequestNpcFurniture request{};
        const size_t cLast = std::min(cWords.size(), first + kSeatWordsPerMessage);
        request.Words.reserve(cLast - first);
        for (size_t i = first; i < cLast; ++i)
        {
            const NpcFurniture::Word& cWord = cWords[i];
            request.Words.push_back(
                {cWord.NpcId, cWord.Where.FurnitureModId, cWord.Where.FurnitureBaseId, cWord.Where.Marker, cWord.Where.State, cWord.OwnershipEpoch, cWord.Where.Placement});
        }
        m_transport.Send(request);
    }
}

// The copy's side: each copy sits where its owner says until the owner says it got up, or says nothing for 5 s.
void FurnitureService::OnNotifyNpcFurniture(const NotifyNpcFurniture& acMessage) noexcept
{
    const uint64_t cNow = NowMs();
    for (const NpcFurnitureWord& cWord : acMessage.Words)
    {
        const std::optional<entt::entity> cEntity = Utils::FindEntityByServerId(cWord.NpcId);
        const auto* pRemote = cEntity ? m_world.try_get<RemoteComponent>(*cEntity) : nullptr;
        const NpcFurniture::CopyVerdict cVerdict = NpcFurniture::CopyTakes(cEntity.has_value(), pRemote != nullptr, pRemote ? pRemote->OwnershipEpoch : 0, cWord.OwnershipEpoch);
        if (cVerdict == NpcFurniture::CopyVerdict::kEpochMiss)
            ReportEpochMiss("an NPC's furniture", cWord.NpcId, cWord.OwnershipEpoch, pRemote->OwnershipEpoch);
        if (cVerdict != NpcFurniture::CopyVerdict::kTake || m_world.all_of<PlayerComponent>(*cEntity))
            continue;

        const NpcFurniture::Seat cWhere{cWord.FurnitureModId, cWord.FurnitureBaseId, cWord.Marker, cWord.State, cWord.Placement};
        ReadCopySeat(*cEntity, cWord.NpcId, cWord.OwnershipEpoch, cWhere, cNow);
    }
}

void FurnitureService::ReadCopySeat(
    const entt::entity aEntity, const uint32_t aActor, const uint32_t aEpoch, const NpcFurniture::Seat& acWhere, const uint64_t aNowMs, const uint64_t aLease,
    const GameId aCell) noexcept
{
    Actor* pCopy = ActorOf(m_world, aEntity);
    if (!pCopy && !aLease)
        return;

    auto& seat = m_world.get_or_emplace<CopySeatComponent>(aEntity);
    if (!seat.Ticket || seat.OwnershipEpoch != aEpoch || seat.Lease != aLease || !(seat.Where == acWhere))
    {
        seat.ServerId = aActor;
        seat.OwnershipEpoch = aEpoch;
        seat.Lease = aLease;
        seat.Ticket = s_work.Issue(aActor);
    }
    if (!(seat.Where == acWhere))
    {
        seat.RuntimeFormId = 0;
        seat.RuntimeLookedAtMs = 0;
    }
    seat.Where = acWhere;
    seat.FurnitureCell = aCell;
    seat.AtMs = aNowMs;
    // Not while a load runs: the word is kept, and the next round of UpdateCopySeats applies it.
    if (const auto cMoment = LoadMomentNow(); pCopy && !cMoment.Loading && !cMoment.Exiting && !ApplyCopySeat(aEntity, pCopy, aNowMs))
        Forget(aEntity);
}

// Each round: a copy not seated yet (not loaded when the word came) is tried again, and a word not heard again for 5 s goes.
void FurnitureService::UpdateCopySeats(const uint64_t aNowMs) noexcept
{
    Vector<entt::entity> done;
    const auto cView = m_world.view<CopySeatComponent>();
    for (const entt::entity cEntity : cView)
    {
        Actor* pCopy = ActorOf(m_world, cEntity);
        // A copy this game took over runs its own AI again, which gets it up when its package says so.
        const auto& cSeat = cView.get<CopySeatComponent>(cEntity);
        if (!m_world.all_of<RemoteComponent>(cEntity) || (pCopy ? !ApplyCopySeat(cEntity, pCopy, aNowMs) : !cSeat.Lease || NpcFurniture::CopyExpired(cSeat.AtMs, aNowMs)))
            done.push_back(cEntity);
    }

    for (const entt::entity cEntity : done)
        Forget(cEntity);
}

void FurnitureService::Forget(const entt::entity aEntity) noexcept
{
    s_work.Retire(m_world.get<CopySeatComponent>(aEntity).ServerId);
    m_world.remove<CopySeatComponent>(aEntity);
}

// What the main thread did with the seats and stands handed to it.
void FurnitureService::TakeDoneWork(const uint64_t aNowMs) noexcept
{
    for (const SeatWorkDone& cDone : s_work.TakeDone())
    {
        // Only the receipt of the work the copy waits for: one from an earlier decision, a replaced word or an undo held over a load
        // changes nothing here (what the game now reads is read again next round).
        const std::optional<entt::entity> cEntity = Utils::FindEntityByServerId(cDone.ServerId);
        auto* pSeat = cEntity ? m_world.try_get<CopySeatComponent>(*cEntity) : nullptr;
        const NpcFurniture::WorkId cReceipt{cDone.Serial, cDone.Ticket, cDone.CopyHandle};
        if (!NpcFurniture::ReceiptIsFor(pSeat != nullptr, pSeat ? pSeat->WorkOutAtMs : 0, pSeat ? pSeat->Work : NpcFurniture::WorkId{}, cReceipt))
            continue;
        pSeat->WorkOutAtMs = 0;
        if (!cDone.Done)
            continue;

        if (cDone.Stand)
        {
            spdlog::debug("Furniture: copy (server id {:X}) stood; the game now reads state {}", cDone.ServerId, cDone.ReadState);
            pSeat->SeatedInFormId = 0;
            pSeat->SeatedState = 0;
            pSeat->CopyHandle = 0;
            pSeat->PlacedTicket = 0;
            continue;
        }

        // Whatever furniture the game reads back, a copy it settled is this game's to stand again. One settled anywhere but where it
        // was asked to is not tried again at once.
        const NpcFurniture::SeatReadBack cReadBack =
            NpcFurniture::ReadBack(cDone.AskedState, cDone.ReadState, cDone.ReadFurnitureFormId == cDone.FurnitureFormId, cDone.AskedMarker, cDone.ReadMarker);
        if (cReadBack == NpcFurniture::SeatReadBack::kRefused)
        {
            pSeat->RefusedAtMs = aNowMs;
            spdlog::debug(
                "Furniture: the game would not put copy (server id {:X}) in furniture {:X} (marker {}); it reads state {}", cDone.ServerId, cDone.FurnitureFormId,
                cDone.AskedMarker, cDone.ReadState);
            continue;
        }
        pSeat->RefusedAtMs = cReadBack == NpcFurniture::SeatReadBack::kAsAsked ? 0 : aNowMs;
        pSeat->SeatedInFormId = cDone.ReadFurnitureFormId ? cDone.ReadFurnitureFormId : cDone.FurnitureFormId;
        pSeat->SeatedState = cDone.ReadState;
        pSeat->CopyHandle = cDone.CopyHandle;
        pSeat->PlacedTicket = cDone.Ticket;
        pSeat->AskedMarker = cDone.AskedMarker;
        pSeat->GivenMarker = cDone.ReadMarker;
        spdlog::debug(
            "Furniture: copy (server id {:X}) put in furniture {:X} (marker {}); the game now reads state {} in {:X} at marker {}", cDone.ServerId, cDone.FurnitureFormId,
            cDone.AskedMarker, cDone.ReadState, cDone.ReadFurnitureFormId, cDone.ReadMarker);
    }
}

bool FurnitureService::ApplyCopySeat(const entt::entity aEntity, Actor* apCopy, const uint64_t aNowMs) noexcept
{
    auto& seat = m_world.get<CopySeatComponent>(aEntity);
    if (apCopy && GameExtension::Reserved(apCopy->formID))
    {
        GameExtension::ReleaseFurnitureSeat(apCopy);
        return false;
    }

    const auto* pRemote = m_world.try_get<RemoteComponent>(aEntity);
    if (!pRemote)
        return false;

    // A seat or stand is out on the main thread: what it did is read back first.
    if (seat.WorkOutAtMs)
    {
        if (aNowMs - seat.WorkOutAtMs < NpcFurniture::kWorkLostMs)
            return true;
        seat.WorkOutAtMs = 0;
    }

    const bool cExpired = NpcFurniture::CopyExpired(seat.AtMs, aNowMs);
    // When the owner's NPC got up: the copy is given the time its own replayed get-up takes before it is stood.
    if (NpcFurniture::IsSettled(seat.Where.State))
        seat.StandAskedAtMs = 0;
    else if (!seat.StandAskedAtMs)
        seat.StandAskedAtMs = aNowMs;

    // Made again here since this game seated it: the seat this game made was on the actor that went.
    const uint32_t cCopyHandle = HandleOf(apCopy);
    if (seat.CopyHandle && seat.CopyHandle != cCopyHandle)
    {
        seat.SeatedInFormId = 0;
        seat.SeatedState = 0;
        seat.CopyHandle = 0;
        seat.PlacedTicket = 0;
    }

    TESObjectREFR* pFurniture = nullptr;
    if (!cExpired && NpcFurniture::IsSettled(seat.Where.State))
    {
        const uint32_t cFormId = m_world.GetModSystem().GetGameId(seat.Where.FurnitureModId, seat.Where.FurnitureBaseId);
        if (!seat.Where.Placement.Placed)
            pFurniture = cFormId ? Cast<TESObjectREFR>(TESForm::GetById(cFormId)) : nullptr;
        else
        {
            // Placed at run time: the form named is its base. The one found here is kept while it still stands there; when none
            // is, it is looked for again only at each heartbeat, as the search walks the loaded cells' references.
            TESObjectREFR* pKept = seat.RuntimeFormId ? Cast<TESObjectREFR>(TESForm::GetById(seat.RuntimeFormId)) : nullptr;
            if (pKept && (!pKept->IsTemporary() || !pKept->baseForm || pKept->baseForm->formID != cFormId ||
                          !NpcFurniture::RuntimeSeatFits({pKept->formID, pKept->position.x, pKept->position.y, pKept->position.z}, seat.Where.Placement)))
                pKept = nullptr;
            if (!pKept && NpcFurniture::MaySeatAgain(seat.RuntimeLookedAtMs, aNowMs))
            {
                seat.RuntimeLookedAtMs = aNowMs;
                pKept = FindRuntimeFurniture(apCopy, cFormId, seat.Where.Placement);
                if (!pKept)
                    spdlog::debug(
                        "Furniture: no furniture of base {:X} placed at run time near ({}, {}, {}) here for copy {:X}", cFormId, seat.Where.Placement.X, seat.Where.Placement.Y,
                        seat.Where.Placement.Z, apCopy->formID);
            }
            seat.RuntimeFormId = pKept ? pKept->formID : 0;
            pFurniture = pKept;
        }
    }

    // The furniture the copy holds is read whatever its state bits say: the action replay writes its owner's flags over a copy's, so the
    // bits are not always the seat this game made (NpcFurniture::CopySeatState).
    const TESObjectREFR* pHeldFurniture = CurrentFurniture(apCopy);
    const bool cSeatedByUs = seat.SeatedInFormId != 0 && pHeldFurniture && pHeldFurniture->formID == seat.SeatedInFormId;
    const uint8_t cCopyState = NpcFurniture::CopySeatState(SitSleepState(apCopy), pHeldFurniture != nullptr, cSeatedByUs, seat.SeatedState);
    const bool cSameFurniture = pFurniture && pHeldFurniture == pFurniture && cCopyState != NpcFurniture::kStateNone;
    // The same furniture is not the same seat at another marker (two places on one bench).
    const bool cSameSeat = NpcFurniture::SameSeat(
        cSameFurniture, cSameFurniture ? FurnitureMarker(apCopy) : 0, seat.Where.Marker, !seat.Lease && cSeatedByUs && seat.AskedMarker == seat.Where.Marker, seat.GivenMarker);
    // A copy on a horse is the mount's to seat; a seat that is not furniture is never one.
    const uint32_t cFurnitureCell = seat.Lease ? m_world.GetModSystem().GetGameId(seat.FurnitureCell) : 0;
    const bool cReady = IsFurniture(pFurniture) && !pFurniture->IsDisabled() && !pFurniture->IsDeleted() && pFurniture->GetNiNode() && apCopy->GetNiNode() && CanSeat(apCopy) &&
                        !apCopy->IsDead() && !IsMounted(apCopy) && (!seat.Lease || InFurnitureCell(pFurniture, cFurnitureCell));

    const NpcFurniture::CopyStep cStep = NpcFurniture::CopyAction(
        seat.Where.State, cExpired, cCopyState, cSameSeat, cSeatedByUs, cReady, seat.Lease != 0 || NpcFurniture::StandGraceOver(seat.StandAskedAtMs, aNowMs));
    switch (NpcFurniture::CopyActionUnder(pRemote->OwnershipEpoch == seat.OwnershipEpoch, cStep))
    {
    case NpcFurniture::CopyStep::kKeep:
    case NpcFurniture::CopyStep::kWait: return true;
    case NpcFurniture::CopyStep::kSeat:
    {
        if (!NpcFurniture::MaySeatAgain(seat.RefusedAtMs, aNowMs))
            return true;
        // On the main thread at the start of the next frame, if nothing changed by then (RunQueuedWork).
        SeatWork work{};
        work.ServerId = seat.ServerId;
        work.CopyFormId = apCopy->formID;
        work.CopyHandle = cCopyHandle;
        work.FurnitureFormId = pFurniture->formID;
        work.FurnitureHandle = HandleOf(pFurniture);
        work.CellFormId = cFurnitureCell;
        work.State = seat.Where.State;
        work.Marker = seat.Where.Marker;
        work.Stamp = {seat.Ticket, UI::LoadingScreensQueued(), 0};
        work.Serial = m_workSerials.Next();
        work.QueuedAt = std::chrono::steady_clock::now();
        s_work.Push(work);
        seat.Work = {work.Serial, work.Stamp.Ticket, work.CopyHandle};
        seat.WorkOutAtMs = aNowMs;
        return true;
    }
    case NpcFurniture::CopyStep::kStand:
    {
        spdlog::debug(
            "Furniture: copy {:X} taken out of furniture {:X}: {}", apCopy->formID, seat.SeatedInFormId,
            cExpired ? "no word from its owner for 5 s"
                     : (NpcFurniture::IsSettled(seat.Where.State) ? "its owner's NPC moved to another seat" : "its own get-up did not play in 2 s"));
        // An undo of the seat this game put in place: it has no ticket of its own, and is done only while that seat is still the one
        // in place.
        SeatWork work{};
        work.Stand = true;
        work.ServerId = seat.ServerId;
        work.CopyFormId = apCopy->formID;
        work.CopyHandle = cCopyHandle;
        work.State = seat.SeatedState;
        if (seat.Lease)
        {
            work.FurnitureFormId = seat.SeatedInFormId;
            work.Marker = seat.GivenMarker;
        }
        work.Stamp = {0, UI::LoadingScreensQueued(), seat.PlacedTicket};
        work.Serial = m_workSerials.Next();
        work.QueuedAt = std::chrono::steady_clock::now();
        s_work.Push(work);
        seat.Work = {work.Serial, work.Stamp.Ticket, work.CopyHandle};
        seat.WorkOutAtMs = aNowMs;
        if (!seat.Lease)
        {
            seat.SeatedInFormId = 0;
            seat.SeatedState = 0;
        }
        return seat.Lease || (!cExpired && NpcFurniture::IsSettled(seat.Where.State));
    }
    case NpcFurniture::CopyStep::kForget:
    default: return false;
    }
}
