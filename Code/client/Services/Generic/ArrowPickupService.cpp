#include <TiltedOnlinePCH.h>

#include <Services/ArrowPickupService.h>
#include <Services/TransportService.h>

#include <Events/ArrowPickupEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestArrowPickup.h>
#include <Messages/NotifyArrowPickup.h>

#include <ArrowPickup.h>
#include <World.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
constexpr size_t cRemovedKept = 16;
// Other players' pick-ups waiting for their second read; more at once are dropped, the oldest first.
constexpr size_t cPendingKept = 8;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsExterior(const TESObjectCELL* apCell) noexcept
{
    // Bit 0 of the cell's flags is "interior".
    return apCell && apCell->worldspace && (apCell->cellFlags & 1) == 0;
}
} // namespace

ArrowPickupService::ArrowPickupService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_eventConnection = aDispatcher.sink<ArrowPickupEvent>().connect<&ArrowPickupService::OnArrowPickupEvent>(this);
    m_notifyConnection = aDispatcher.sink<NotifyArrowPickup>().connect<&ArrowPickupService::OnNotifyArrowPickup>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ArrowPickupService::OnUpdate>(this);
}

TiltedPhoques::Vector<TESObjectCELL*> ArrowPickupService::CellsAround(TESObjectCELL* apCell, const float aX, const float aY) noexcept
{
    TiltedPhoques::Vector<TESObjectCELL*> cells;
    if (!apCell)
        return cells;
    if (!IsExterior(apCell))
    {
        cells.push_back(apCell);
        return cells;
    }
    const int32_t cGridX = ArrowPickup::GridOf(aX);
    const int32_t cGridY = ArrowPickup::GridOf(aY);
    for (int32_t dx = -1; dx <= 1; ++dx)
        for (int32_t dy = -1; dy <= 1; ++dy)
        {
            TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(cGridX + dx, cGridY + dy, apCell->worldspace, false);
            if (pCell && pCell->IsAttached() && std::find(cells.begin(), cells.end(), pCell) == cells.end())
                cells.push_back(pCell);
        }
    // The arrow's own cell, even when the grid read from the place disagrees with it.
    if (apCell->IsAttached() && std::find(cells.begin(), cells.end(), apCell) == cells.end())
        cells.push_back(apCell);
    return cells;
}

bool ArrowPickupService::IsWithActor(const TiltedPhoques::Vector<TESObjectCELL*>& acCells, const float aX, const float aY, const float aZ) noexcept
{
    for (const TESObjectCELL* pCell : acCells)
        for (TESObjectREFR* pRef : pCell->GetRefsByFormTypes({FormType::Npc}))
        {
            Actor* pActor = Cast<Actor>(pRef);
            if (!pActor)
                continue;
            const ActorExtension* pExtension = pActor->GetExtension();
            const bool cPlayer = pActor == PlayerCharacter::Get() || (pExtension && pExtension->IsPlayer());
            if (ArrowPickup::KeptForActor(aX, aY, aZ, pActor->position.x, pActor->position.y, pActor->position.z, cPlayer))
                return true;
        }
    return false;
}

void ArrowPickupService::OnArrowPickupEvent(const ArrowPickupEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // Never a copy this game removed for another player's pick-up (removal does not activate, but this keeps it so).
    if (std::find(m_removedHere.begin(), m_removedHere.end(), acEvent.RefId) != m_removedHere.end())
        return;

    // Only once the arrow really left the world: the game moved it into the inventory.
    if (TESForm* pStill = TESForm::GetById(acEvent.RefId))
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(pStill);
        if (pRef && !pRef->IsDisabled() && !pRef->IsDeleted() && pRef->GetNiNode())
        {
            spdlog::debug("ArrowPickup: arrow {:X} was activated but is still in the world; not sent", acEvent.RefId);
            return;
        }
    }

    // An arrow with an actor or a body is StuckArrows' to share, never this.
    TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(acEvent.CellId));
    if (pCell && IsWithActor(CellsAround(pCell, acEvent.X, acEvent.Y), acEvent.X, acEvent.Y, acEvent.Z))
    {
        spdlog::debug("ArrowPickup: arrow {:X} was with an actor or a body; not sent", acEvent.RefId);
        return;
    }

    RequestArrowPickup request{};
    // A projectile or a cell a mod made at run time has an id only this game knows: the other games could not match it.
    if (!m_world.GetModSystem().GetServerModId(acEvent.ProjectileBaseId, request.ProjectileBaseId) || !m_world.GetModSystem().GetServerModId(acEvent.CellId, request.CellId))
    {
        spdlog::debug("ArrowPickup: arrow {:X} taken in cell {:X} not sent: no id the other games know", acEvent.ProjectileBaseId, acEvent.CellId);
        return;
    }
    request.X = acEvent.X;
    request.Y = acEvent.Y;
    request.Z = acEvent.Z;
    m_transport.Send(request);
    spdlog::debug("ArrowPickup: arrow {:X} taken at ({:.0f}, {:.0f}, {:.0f}); sent so the other games remove theirs", acEvent.ProjectileBaseId, acEvent.X, acEvent.Y, acEvent.Z);
}

void ArrowPickupService::OnNotifyArrowPickup(const NotifyArrowPickup& acMessage) noexcept
{
    if (!ArrowPickup::IsPlace(acMessage.X, acMessage.Y, acMessage.Z))
        return;

    const uint32_t cProjectileId = m_world.GetModSystem().GetGameId(acMessage.ProjectileBaseId);
    const uint32_t cCellId = m_world.GetModSystem().GetGameId(acMessage.CellId);
    TESObjectCELL* pCell = cCellId ? Cast<TESObjectCELL>(TESForm::GetById(cCellId)) : nullptr;
    if (!cProjectileId || !pCell)
    {
        spdlog::debug(
            "ArrowPickup: another player's arrow {:X}:{:X} taken in a cell not here; nothing to remove", acMessage.ProjectileBaseId.ModId, acMessage.ProjectileBaseId.BaseId);
        return;
    }

    // This game's references that may be its copy: indoors in that cell, outdoors in the worldspace's cells around the place.
    // Each is read now and again a moment later (OnUpdate): only one that did not move in between, landed or stuck, is taken.
    const TiltedPhoques::Vector<TESObjectCELL*> cCells = CellsAround(pCell, acMessage.X, acMessage.Y);
    Pending pending{};
    pending.FirstReadMs = NowMs();
    pending.ProjectileId = cProjectileId;
    for (const TESObjectCELL* pSearched : cCells)
    {
        for (TESObjectREFR* pRef : pSearched->GetRefsByFormTypes({FormType::Projectile}))
        {
            if (!pRef || !pRef->baseForm || pRef->IsDisabled() || pRef->IsDeleted())
                continue;
            const float cX = pRef->position.x - acMessage.X;
            const float cY = pRef->position.y - acMessage.Y;
            const float cZ = pRef->position.z - acMessage.Z;
            ArrowPickup::Candidate candidate{};
            candidate.Distance = std::sqrt(cX * cX + cY * cY + cZ * cZ);
            candidate.SameProjectile = pRef->baseForm->formID == cProjectileId;
            candidate.Temporary = pRef->IsTemporary();
            // At rest is only known at the second read; the rest is weighed now.
            candidate.AtRest = true;
            if (!ArrowPickup::Matches(candidate))
                continue;
            // Only weighed for one that could be taken: it reads every actor around.
            candidate.OnActor = IsWithActor(cCells, pRef->position.x, pRef->position.y, pRef->position.z);
            if (candidate.OnActor)
                continue;
            pending.Refs.push_back({pRef->formID, pRef->position.x, pRef->position.y, pRef->position.z, candidate});
        }
    }

    if (pending.Refs.empty())
    {
        spdlog::debug("ArrowPickup: another player took arrow {:X}; no copy of it within {:.0f} units here ({} cells)", cProjectileId, ArrowPickup::kMatchUnits, cCells.size());
        return;
    }
    if (m_pending.size() >= cPendingKept)
        m_pending.erase(m_pending.begin());
    m_pending.push_back(std::move(pending));
}

void ArrowPickupService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_pending.empty())
        return;
    const uint64_t cNow = NowMs();
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (!ArrowPickup::RestReadDue(it->FirstReadMs, cNow))
        {
            ++it;
            continue;
        }
        Decide(*it);
        it = m_pending.erase(it);
    }
}

void ArrowPickupService::Decide(Pending& aPending) noexcept
{
    TiltedPhoques::Vector<TESObjectREFR*> refs;
    TiltedPhoques::Vector<ArrowPickup::Candidate> candidates;
    for (PendingRef& entry : aPending.Refs)
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(entry.FormId));
        if (!pRef || pRef->IsDisabled() || pRef->IsDeleted())
            continue;
        entry.Candidate.AtRest = ArrowPickup::IsAtRest(entry.X, entry.Y, entry.Z, pRef->position.x, pRef->position.y, pRef->position.z);
        refs.push_back(pRef);
        candidates.push_back(entry.Candidate);
    }

    const size_t cPick = ArrowPickup::PickNearest(candidates.data(), candidates.size());
    if (cPick >= refs.size())
    {
        spdlog::debug("ArrowPickup: another player took arrow {:X}; none of this game's {} copies near it had landed", aPending.ProjectileId, aPending.Refs.size());
        return;
    }

    TESObjectREFR* pCopy = refs[cPick];
    if (m_removedHere.size() >= cRemovedKept)
        m_removedHere.erase(m_removedHere.begin());
    m_removedHere.push_back(pCopy->formID);
    spdlog::debug(
        "ArrowPickup: another player took arrow {:X}; this game's copy {:X}, {:.1f} units away, removed", aPending.ProjectileId, pCopy->formID, candidates[cPick].Distance);
    pCopy->Disable();
    pCopy->Delete();
}
