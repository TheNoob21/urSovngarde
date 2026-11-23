#include <TiltedOnlinePCH.h>

#include <StuckArrows.h>

#include <StuckArrow.h>
#include <StuckArrowStore.h>
#include <Messages/NotifyDismember.h>
#include <Messages/NotifyStuckArrow.h>
#include <Messages/RequestDismember.h>
#include <Messages/RequestStuckArrow.h>
#include <Services/TransportService.h>
#include <World.h>
#include <Events/UpdateEvent.h>

#include <Components.h>
#include <Actor.h>
#include <Forms/TESForm.h>
#include <Games/ActorExtension.h>
#include <ExtraData/ExtraAttachedArrows3D.h>
#include <Games/Skyrim/StuckArrowPlacement.h>
#include <NetImmerse/NiTransform.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTree.h>
#include <Components/TESModel.h>
#include <Components/TESContainer.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESAmmo.h>
#include <Forms/TESObjectWEAP.h>
#include <Forms/TESObjectCELL.h>
#include <Projectiles/Projectile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>

namespace
{
std::mutex s_lock;
StuckArrowStore s_store;
StuckArrowTracker s_tracker;
// Counts the arrows sent per actor by server id (game thread).
std::unordered_map<uint32_t, uint32_t> s_sequences;
// Where the next frame's look at actors' arrows starts (game thread).
size_t s_nextWatched = 0;
// The actors read since that look last went round (game thread): the lives of the others are dropped when it does.
std::unordered_set<uint32_t> s_watchedThisRound;

// Actors whose arrows are read each frame, in turn: about a tenth of a second round a full cell.
constexpr size_t kWatchedPerFrame = 16;
// An actor not read for this long, in seconds, is forgotten.
constexpr double kForgetWatched = 10.0;
// More than the engine keeps (iMaxAttachedArrows, 3 by default) is read as none.
constexpr uint32_t kMaxEngineArrows = 32;

// NetImmerse objects are read through raw offsets: the VR NiAVObject is larger than the client's struct, and these are the same on both.
using NiLayout::kCollisionObjectOffset;
using NiLayout::kLocalOffset;
using NiLayout::kNameOffset;
using NiLayout::kParentOffset;
// NiAVObject::flags, bit 0 hidden, as SpawnReveal hides a copy.
#ifdef SKYRIMVR
constexpr uint32_t kFlagsOffset = 0x10C;
#else
constexpr uint32_t kFlagsOffset = 0xF4;
#endif
constexpr uint32_t kHidden = 1u << 0;
// TESAmmo's projectile, the first member of its data (CommonLibSSE-NG TESAmmo::RUNTIME_DATA: 0x110, and 0x100 on VR, which has no TESWeightForm).
#ifdef SKYRIMVR
constexpr uint32_t kAmmoProjectileOffset = 0x100;
#else
constexpr uint32_t kAmmoProjectileOffset = 0x110;
#endif

// Set while this code cuts a copy's head off, so the hook does not send it back.
thread_local bool t_cutting = false;
// Actors the game cut the head off, sent from the next frame: the hook may be called off the game thread (under s_lock).
StuckArrowBeheadings s_beheadings;
std::atomic_bool s_updateDue{false};
uint64_t s_session{};
std::unordered_map<uint32_t, uint64_t> s_nativeLives;
thread_local uint32_t t_inventoryReplacement{};
struct Removal
{
    uint32_t FormId;
    uintptr_t Root;
    uint32_t Projectile;
    int64_t Before;
    int64_t After;
    double Time;
    uint64_t Life;
};
std::vector<Removal> s_removals;
std::vector<StuckArrowTracker::Released> s_releases;

// Each made mesh and its parent have strong references until main-loop retirement.
// No cached raw skeleton pointer is dereferenced after a load or ownership change.
struct MadeNode
{
    NiNode* Object;
    NiNode* Parent;
    double Since;
    uint32_t FormId;
    StuckArrowStore::Arrow Placement;
};
std::unordered_map<uintptr_t, MadeNode> s_made;
std::atomic_bool s_connected{false};

void ReleaseLater(std::vector<StuckArrowTracker::Released> aReleased)
{
    s_releases.insert(s_releases.end(), aReleased.begin(), aReleased.end());
}

#ifdef SKYRIMVR
constexpr size_t kAttachChildSlot = 0x36;
constexpr size_t kDetachChildSlot = 0x39;
#else
constexpr size_t kAttachChildSlot = 0x35;
constexpr size_t kDetachChildSlot = 0x38;
#endif

double Now() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

//! This game's copy of an actor another game runs, by its server id; null when there is none here.
Actor* CopyOf(const uint32_t aServerId) noexcept
{
    auto view = World::Get().view<FormIdComponent, RemoteComponent>();
    for (const auto cEntity : view)
    {
        if (view.get<RemoteComponent>(cEntity).Id == aServerId)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id));
    }
    return nullptr;
}

template <class T> T& At(void* apObject, const uint32_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

using NiTree::AsNode;

void* FindBone(void* apRoot, const char* apName) noexcept
{
    void* pFound = nullptr;
    NiTree::VisitTree(
        apRoot, 4096,
        [&](void* apObject, void* apNode)
        {
            const char* pName = At<const char*>(apObject, kNameOffset);
            if (!apNode || !pName || _stricmp(pName, apName) != 0)
                return true;
            pFound = apNode;
            return false;
        });
    return pFound;
}

void Attach(void* apParent, void* apObject) noexcept
{
    auto** ppVtable = *static_cast<void***>(apParent);
    reinterpret_cast<void(__fastcall*)(void*, void*, bool)>(ppVtable[kAttachChildSlot])(apParent, apObject, false);
}

void Detach(void* apParent, void* apObject) noexcept
{
    auto** ppVtable = *static_cast<void***>(apParent);
    reinterpret_cast<void(__fastcall*)(void*, void*)>(ppVtable[kDetachChildSlot])(apParent, apObject);
}

bool Place(void* apArrow, const StuckArrowStore::Arrow& acArrow) noexcept;

bool PlaceOn(void* apParent, void* apArrow, const StuckArrowStore::Arrow& acArrow) noexcept
{
    if (!Place(apArrow, acArrow))
        return false;
    if (At<void*>(apArrow, kParentOffset) != apParent)
    {
        auto* pObject = static_cast<NiAVObject*>(apArrow);
        pObject->IncRef();
        if (void* pPrevious = At<void*>(apArrow, kParentOffset))
            Detach(pPrevious, apArrow);
        Attach(apParent, apArrow);
        pObject->DecRef();
    }
    return At<void*>(apArrow, kParentOffset) == apParent;
}

void DrainReleases() noexcept
{
    std::vector<StuckArrowTracker::Released> released;
    {
        std::scoped_lock lock(s_lock);
        released.swap(s_releases);
    }
    for (const auto& cReleased : released)
    {
        const auto it = s_made.find(cReleased.Object);
        if (it == s_made.end())
            continue;
        const MadeNode node = it->second;
        if (At<void*>(node.Object, kParentOffset) == node.Parent)
            Detach(node.Parent, node.Object);
        node.Object->DecRef();
        node.Parent->DecRef();
        s_made.erase(it);
    }
}

// The model database and deep-copy call return one held reference.
NiNode* MakeArrow(TESForm* apProjectile) noexcept
{
    const TESModel* pModel = apProjectile ? Cast<TESModel>(apProjectile) : nullptr;
    const char* pPath = pModel ? pModel->name.AsAscii() : nullptr;
    if (!pPath || !*pPath)
        return nullptr;
    struct ModelArgs
    {
        uint32_t LodMult{0};
        uint32_t TexLoadLevel{3};
        bool Unk8{true}, Unk9{false}, UnkA{true}, PostProcess{true};
    };
    static_assert(sizeof(ModelArgs) == 0xC);
    using TDemand = uint32_t(const char*, NiNode*&, const ModelArgs&);
    using TCopy = void(NiObject*, NiObject*&);
    POINTER_SKYRIMSE(TDemand, s_demand, 75782, 74040);
    POINTER_SKYRIMSE(TCopy, s_copy, 70191, 68839);
    if (!s_demand.Get() || !s_copy.Get())
        return nullptr;
    NiNode* pShared = nullptr;
    const auto error = s_demand.Get()(pPath, pShared, ModelArgs{});
    if (error || !pShared)
    {
        if (pShared)
            pShared->DecRef();
        return nullptr;
    }
    NiObject* pCopy = nullptr;
    s_copy.Get()(pShared, pCopy);
    pShared->DecRef();
    if (pCopy && !AsNode(pCopy))
    {
        pCopy->DecRef();
        return nullptr;
    }
    // A stuck visual must not introduce a collision body or inventory entry.
    if (pCopy)
    {
        NiTree::VisitTree(
            pCopy, 4096,
            [](void* apObject)
            {
                if (auto*& pCollision = At<NiRefObject*>(apObject, kCollisionObjectOffset))
                {
                    pCollision->DecRef();
                    pCollision = nullptr;
                }
            });
    }
    return static_cast<NiNode*>(pCopy);
}

glm::mat3 ToGlm(const NiMatrix3& acMatrix) noexcept
{
    glm::mat3 result{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result[c][r] = acMatrix.data[r][c];
    return result;
}

NiMatrix3 FromGlm(const glm::mat3& acMatrix) noexcept
{
    NiMatrix3 result{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.data[r][c] = acMatrix[c][r];
    return result;
}

bool IsFinite(const glm::vec3& acValue) noexcept
{
    return std::isfinite(acValue.x) && std::isfinite(acValue.y) && std::isfinite(acValue.z);
}

bool IsFinite(const glm::mat3& acValue) noexcept
{
    return IsFinite(acValue[0]) && IsFinite(acValue[1]) && IsFinite(acValue[2]);
}

//! An arrow the engine lists stuck in an actor.
struct EngineArrow
{
    void* pObject;
    uint64_t Stamp;
    TESForm* pSource;
};

//! The actor's stuck arrows as the engine lists them now (ExtraAttachedArrows3D).
std::vector<EngineArrow> ArrowsIn(Actor* apActor) noexcept
{
    std::vector<EngineArrow> arrows;
    const auto* pExtra = static_cast<const ExtraAttachedArrows3D*>(apActor->extraData.GetByType(ExtraDataType::AttachedArrows3D));
    if (!pExtra || !pExtra->data.data || pExtra->data.length > kMaxEngineArrows)
        return arrows;
    for (uint32_t i = 0; i < pExtra->data.length; ++i)
    {
        const auto& cItem = pExtra->data.data[i];
        if (cItem.arrow3D.object)
            arrows.push_back({cItem.arrow3D.object, cItem.timeStamp, cItem.source});
    }
    return arrows;
}

std::vector<StuckArrowTracker::Entry> EntriesOf(const std::vector<EngineArrow>& acArrows) noexcept
{
    std::vector<StuckArrowTracker::Entry> entries;
    entries.reserve(acArrows.size());
    for (const EngineArrow& arrow : acArrows)
        entries.push_back({reinterpret_cast<uintptr_t>(arrow.pObject), arrow.Stamp, arrow.pSource ? arrow.pSource->formID : 0});
    return entries;
}

//! The engine's arrow read now that is the one kept; null when it is no longer listed.
const EngineArrow* Listed(const std::vector<EngineArrow>& acArrows, const StuckArrowTracker::Entry& acEntry) noexcept
{
    const auto cArrow = std::find_if(
        acArrows.begin(), acArrows.end(),
        [&acEntry](const EngineArrow& acArrow) { return reinterpret_cast<uintptr_t>(acArrow.pObject) == acEntry.Object && acArrow.Stamp == acEntry.Stamp; });
    return cArrow == acArrows.end() ? nullptr : &*cArrow;
}

//! The name of the node the arrow hangs under: the bone it struck. Null when it hangs under nothing.
const char* BoneOf(void* apArrow) noexcept
{
    void* pParent = At<void*>(apArrow, kParentOffset);
    return pParent ? At<const char*>(pParent, kNameOffset) : nullptr;
}

//! An arrow stuck in an actor this game runs, sent with where it sits on its bone.
void SendArrow(const uint32_t aServerId, const uint32_t aFormId, const EngineArrow& acArrow, const uint32_t aSequence = 0, const bool aRemoved = false) noexcept
{
    const char* pBone = BoneOf(acArrow.pObject);
    if (!pBone || !*pBone)
        return;

    // The arrow's local is already in the space of its bone.
    const NiTransform& cLocal = At<NiTransform>(acArrow.pObject, kLocalOffset);
    const glm::vec3 cPosition{cLocal.translate.x, cLocal.translate.y, cLocal.translate.z};
    const glm::mat3 cRotation = ToGlm(cLocal.rotate);
    if (!IsFinite(cPosition) || !IsFinite(cRotation) || !std::isfinite(cLocal.scale))
        return;

    uint32_t ammoModId = 0;
    uint32_t ammoBaseId = 0;
    if (acArrow.pSource)
        World::Get().GetModSystem().GetServerModId(acArrow.pSource->formID, ammoModId, ammoBaseId);

    const uint32_t cSequence = aSequence ? aSequence : ++s_sequences[aServerId];
    spdlog::debug("Stuck arrow: sent the arrow in {} of actor {:X} ({:X}) (sequence {})", pBone, aServerId, aFormId, cSequence);
    StuckArrows::SendStuckArrow(aServerId, ammoModId, ammoBaseId, pBone, cPosition, StuckArrowPlacement::ToQuaternion(cRotation), cLocal.scale, cSequence, aRemoved);
    std::scoped_lock lock(s_lock);
    s_tracker.Sent(aFormId, {reinterpret_cast<uintptr_t>(acArrow.pObject), acArrow.Stamp, acArrow.pSource ? acArrow.pSource->formID : 0}, cSequence, aRemoved);
}

// The native inventory's base count plus the first change entry, as RemoveItem reads it.
// Same rule as TESObjectREFR.cpp::BaseCount/FindEntry; no ExtraContainerChanges list is required.
// Do not use GetItemCountInInventory here: it dereferences the changes list, and its inherited
// TESContainer::GetItemCount wrapper does not forward the item argument.
const ExtraContainerChanges::Data* ExistingChangesOf(Actor* apActor) noexcept
{
    // The project's GetContainerChanges wrapper resolves SE 15802 / AE 16040,
    // CommonLibSSE-NG's MakeInventoryChanges. Observation must not initialize inventory.
    const auto* pExtra = static_cast<const ExtraContainerChanges*>(apActor->extraData.GetByType(ExtraDataType::ContainerChanges));
    return pExtra ? pExtra->data : nullptr;
}

int64_t ItemCountOf(Actor* apActor, TESForm* apItem) noexcept
{
    int64_t base = 0;
    if (const TESContainer* pContainer = apActor->GetContainer(); pContainer && pContainer->entries)
        for (uint32_t i = 0; i < pContainer->count; ++i)
            if (const auto* pEntry = pContainer->entries[i]; pEntry && pEntry->form == apItem)
                base += static_cast<int32_t>(pEntry->count);
    if (base < 0)
        base = -base;
    const auto* pChanges = ExistingChangesOf(apActor);
    if (pChanges && pChanges->entries)
        for (const auto* pEntry : *pChanges->entries)
            if (pEntry && pEntry->form == apItem)
                return std::max<int64_t>(0, base + pEntry->count);
    return base;
}

//! Native ammo totals by projectile, including base-only inventories and several ammo forms sharing one projectile.
std::unordered_map<uint32_t, int64_t> AmmoCountsOf(Actor* apActor) noexcept
{
    std::unordered_set<TESForm*> forms;
    if (const TESContainer* pContainer = apActor->GetContainer(); pContainer && pContainer->entries)
        for (uint32_t i = 0; i < pContainer->count; ++i)
            if (const auto* pEntry = pContainer->entries[i]; pEntry && pEntry->form && pEntry->form->formType == FormType::Ammo)
                forms.insert(pEntry->form);
    const auto* pChanges = ExistingChangesOf(apActor);
    if (pChanges && pChanges->entries)
        for (const auto* pEntry : *pChanges->entries)
            if (pEntry && pEntry->form && pEntry->form->formType == FormType::Ammo)
                forms.insert(pEntry->form);
    std::unordered_map<uint32_t, int64_t> counts;
    for (TESForm* pAmmo : forms)
        if (const TESForm* pProjectile = At<TESForm*>(pAmmo, kAmmoProjectileOffset))
            counts[pProjectile->formID] += ItemCountOf(apActor, pAmmo);
    return counts;
}

//! Reads actual attachments. The owner validates inventory removal and tells the copies the exact removed shot sequence.
//! Copies retain that decision even when their replicated inventory was already empty before their first read.
//! The arrows returned stay as read only until the engine runs again: they are used within the frame, and read again the next.
std::vector<EngineArrow> ReadArrows(Actor* apActor, const StuckArrowTracker::Life& acLife, const double aNow, std::vector<StuckArrowTracker::Entry>* apFresh = nullptr) noexcept
{
    auto arrows = ArrowsIn(apActor);
    const bool cDead = apActor->IsDead();
    StuckArrowTracker::Read read;
    bool hasArrows = false;
    {
        std::scoped_lock lock(s_lock);
        read = s_tracker.Update(acLife, EntriesOf(arrows), aNow);
        ReleaseLater(std::move(read.LetGo));
        for (auto it = s_removals.begin(); it != s_removals.end();)
        {
            if (it->FormId == acLife.FormId)
            {
                if (acLife.Local && it->Root == acLife.Root && it->Life == s_nativeLives[it->FormId])
                    s_tracker.Removed(acLife.FormId, it->Projectile, it->Before, it->After, it->Time);
                it = s_removals.erase(it);
            }
            else if (aNow - it->Time > kForgetWatched)
                it = s_removals.erase(it);
            else
                ++it;
        }
        hasArrows = !s_tracker.ArrowsOf(acLife.FormId).empty();
    }
    if (apFresh)
        *apFresh = std::move(read.Fresh);
    for (const auto& [madeObject, native] : read.Replayed)
    {
        const auto made = s_made.find(madeObject);
        const EngineArrow* pNative = Listed(arrows, native);
        if (made != s_made.end() && pNative)
            PlaceOn(made->second.Parent, pNative->pObject, made->second.Placement);
    }
    if (!cDead && !hasArrows)
        return arrows;

    const auto cCounts = AmmoCountsOf(apActor);
    std::vector<StuckArrowTracker::Entry> taken;
    std::vector<uintptr_t> hidden;
    {
        std::scoped_lock lock(s_lock);
        // The owner charges ammunition removals to exact shot sequences. A copy uses those
        // tombstones, so inventory replication arriving in another order cannot hide a second shot.
        taken = s_tracker.Loot(acLife.FormId, cCounts, cDead && acLife.Local, aNow);
        hidden = s_tracker.HiddenOf(acLife.FormId);
    }
    if (!taken.empty())
        spdlog::debug("Stuck arrow: {} of the arrows in actor {:X} ({:X}) hidden, as their ammunition was taken from it", taken.size(), acLife.ServerId, acLife.FormId);

    // Set again on each read. Every one kept is in the list just read: one the engine dropped is no longer kept.
    for (const uintptr_t cObject : hidden)
        At<uint32_t>(reinterpret_cast<void*>(cObject), kFlagsOffset) |= kHidden;
    return arrows;
}

//! Reads the next actors' stuck arrows: new ones in actors this game runs are sent, and the copies' are noted for the arrows told.
void WatchArrows(const double aNow) noexcept
{
    auto& world = World::Get();
    // Walked in place by position: objects (containers, doors) carry a form id too, and are passed over.
    const auto& formIds = world.storage<FormIdComponent>();
    const auto cSkip = [&world, &formIds](const size_t aAt)
    {
        const entt::entity cEntity = formIds.at(aAt);
        return cEntity == entt::null || world.all_of<ObjectComponent>(cEntity);
    };
    const auto cWatch = [&world, &formIds, aNow](const size_t aAt)
    {
        const entt::entity cEntity = formIds.at(aAt);
        const uint32_t cFormId = formIds.get(cEntity).Id;
        s_watchedThisRound.insert(cFormId);
        const auto* pLocal = world.try_get<LocalComponent>(cEntity);
        const auto* pRemote = pLocal ? nullptr : world.try_get<RemoteComponent>(cEntity);
        Actor* pActor = pLocal || pRemote ? Cast<Actor>(TESForm::GetById(cFormId)) : nullptr;
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        // Only actors synced with the other games are touched. One no longer synced, or whose 3D is gone, starts afresh.
        if (!pRoot)
        {
            std::scoped_lock lock(s_lock);
            ReleaseLater(s_tracker.Retire(cFormId));
            ++s_nativeLives[cFormId];
            return;
        }

        const StuckArrowTracker::Life cLife{cFormId, pLocal ? pLocal->Id : pRemote->Id, reinterpret_cast<uintptr_t>(pRoot), pLocal != nullptr};
        std::vector<StuckArrowTracker::Entry> fresh;
        const auto cArrows = ReadArrows(pActor, cLife, aNow, &fresh);
        if (!pLocal)
            return;

        for (const auto& cEntry : fresh)
            if (const EngineArrow* pArrow = Listed(cArrows, cEntry))
            {
                bool removed = false;
                {
                    std::scoped_lock lock(s_lock);
                    const auto* pTracked = s_tracker.Find(cFormId, cEntry);
                    removed = pTracked && pTracked->Hidden;
                }
                SendArrow(pLocal->Id, cFormId, *pArrow, 0, removed);
            }
        std::vector<StuckArrowTracker::Arrow> tracked;
        {
            std::scoped_lock lock(s_lock);
            tracked = s_tracker.ArrowsOf(cFormId);
        }
        for (const auto& arrow : tracked)
            if (arrow.Hidden && arrow.Sequence && !arrow.SentHidden)
                if (const EngineArrow* pArrow = Listed(cArrows, arrow.Id))
                    SendArrow(pLocal->Id, cFormId, *pArrow, arrow.Sequence, true);
    };

    const StuckArrowWatch::Step cStep = StuckArrowWatch::Next(formIds.size(), s_nextWatched, kWatchedPerFrame, cSkip, cWatch);
    s_nextWatched = cStep.Next;
    if (cStep.RoundDone)
    {
        std::scoped_lock lock(s_lock);
        ReleaseLater(s_tracker.Prune(aNow, kForgetWatched));
        // An actor no longer here needs no life, unless a removal noted in it still waits.
        std::erase_if(
            s_nativeLives,
            [](const auto& acLife)
            {
                return !s_watchedThisRound.contains(acLife.first) &&
                       std::none_of(s_removals.begin(), s_removals.end(), [&acLife](const Removal& acRemoval) { return acRemoval.FormId == acLife.first; });
            });
        s_watchedThisRound.clear();
    }
}

//! The arrow told, laid on its bone as the owner's game has it. The arrow already hangs under that bone, so only its local is written.
bool Place(void* apArrow, const StuckArrowStore::Arrow& acArrow) noexcept
{
    if (!IsFinite(acArrow.Position))
        return false;

    glm::quat rotation = acArrow.Rotation;
    const bool cFiniteRotation = std::isfinite(rotation.w) && std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z);
    if (!cFiniteRotation || glm::length(rotation) < 1e-4f)
        rotation = glm::quat{1.f, 0.f, 0.f, 0.f};
    const float cScale = std::isfinite(acArrow.Scale) && acArrow.Scale > 0.01f && acArrow.Scale < 100.f ? acArrow.Scale : 1.f;

    NiTransform& local = At<NiTransform>(apArrow, kLocalOffset);
    local.rotate = FromGlm(StuckArrowPlacement::ToMatrix(rotation));
    local.translate.x = acArrow.Position.x;
    local.translate.y = acArrow.Position.y;
    local.translate.z = acArrow.Position.z;
    local.scale = cScale;
    return true;
}

//! Correct native replays on any bone; a missed replay receives its own visual mesh after a short grace period.
void PlaceArrows(const double aNow) noexcept
{
    std::vector<std::pair<uint32_t, StuckArrowStore::Arrow>> waiting;
    {
        std::scoped_lock lock(s_lock);
        waiting = s_store.WaitingArrows();
    }
    for (const auto& [serverId, arrow] : waiting)
    {
        Actor* pActor = CopyOf(serverId);
        if (pActor)
        {
            // Told for a life the game has since ended by itself: never placed on the next one.
            std::scoped_lock lock(s_lock);
            if (s_store.IsOfPastLife(pActor->formID, arrow.Epoch))
            {
                s_store.ArrowDone(serverId, arrow.Sequence);
                continue;
            }
        }
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        void* pBone = pRoot ? FindBone(pRoot, arrow.Bone.c_str()) : nullptr;
        const uint32_t cProjectileId = World::Get().GetModSystem().GetGameId(GameId(arrow.AmmoModId, arrow.AmmoBaseId));
        TESForm* pProjectile = cProjectileId ? TESForm::GetById(cProjectileId) : nullptr;
        bool placed = false;
        if (pBone && pProjectile && pProjectile->formType == FormType::Projectile)
        {
            const auto cArrows = ReadArrows(pActor, {pActor->formID, serverId, reinterpret_cast<uintptr_t>(pRoot), false}, aNow);
            std::vector<StuckArrowTracker::Arrow> tracked;
            {
                std::scoped_lock lock(s_lock);
                tracked = s_tracker.ArrowsOf(pActor->formID);
            }
            const EngineArrow* pMatch = nullptr;
            StuckArrowTracker::Entry match{};
            bool sameBone = false;
            if (arrow.Removed)
            {
                std::scoped_lock lock(s_lock);
                placed = s_tracker.HideSequence(pActor->formID, arrow.Sequence);
            }
            for (const auto& cTracked : tracked)
            {
                if (cTracked.Made || cTracked.Claimed || cTracked.Id.Projectile != cProjectileId ||
                    (cTracked.FirstSeen != StuckArrowTracker::kBeforeWatched && cTracked.FirstSeen < arrow.Received - StuckArrowStore::kArrowWait))
                    continue;
                const EngineArrow* pCandidate = Listed(cArrows, cTracked.Id);
                const char* pCandidateBone = pCandidate ? BoneOf(pCandidate->pObject) : nullptr;
                if (!pCandidateBone)
                    continue;
                const bool cSame = _stricmp(pCandidateBone, arrow.Bone.c_str()) == 0;
                if (!pMatch || (cSame && !sameBone) || (cSame == sameBone && pCandidate->Stamp > pMatch->Stamp))
                {
                    pMatch = pCandidate;
                    match = cTracked.Id;
                    sameBone = cSame;
                }
            }
            if (pMatch)
            {
                placed = placed || arrow.Removed || PlaceOn(pBone, pMatch->pObject, arrow);
                if (placed)
                {
                    std::scoped_lock lock(s_lock);
                    if (!arrow.Removed || !s_tracker.HideSequence(pActor->formID, arrow.Sequence))
                    {
                        s_tracker.Claim(pActor->formID, match, arrow.Sequence);
                        if (arrow.Removed)
                            s_tracker.HideSequence(pActor->formID, arrow.Sequence);
                    }
                }
            }
            // Keep the normal replay when available; make a missing one within the one-second placement contract.
            else if (!arrow.Removed && aNow - arrow.Received >= 0.2)
            {
                if (NiNode* pMade = MakeArrow(pProjectile))
                {
                    placed = PlaceOn(pBone, pMade, arrow);
                    if (placed)
                    {
                        auto* pParent = static_cast<NiNode*>(pBone);
                        pParent->IncRef();
                        const uintptr_t cObject = reinterpret_cast<uintptr_t>(pMade);
                        s_made.emplace(cObject, MadeNode{pMade, pParent, aNow, pActor->formID, arrow});
                        {
                            std::scoped_lock lock(s_lock);
                            ReleaseLater(s_tracker.AddMade(pActor->formID, {cObject, 0, cProjectileId}, aNow, arrow.Sequence));
                        }
                        // Applies a removal received before this missing arrow was made, without another inventory mutation.
                        ReadArrows(pActor, {pActor->formID, serverId, reinterpret_cast<uintptr_t>(pRoot), false}, aNow);
                    }
                    else
                        pMade->DecRef();
                }
            }
            if (placed && arrow.Removed)
                ReadArrows(pActor, {pActor->formID, serverId, reinterpret_cast<uintptr_t>(pRoot), false}, aNow);
        }
        if (placed || aNow - arrow.Received > StuckArrowStore::kArrowWait)
        {
            spdlog::debug("Stuck arrow: {} arrow on {} of actor {:X} (sequence {})", placed ? "placed" : "could not place", arrow.Bone, serverId, arrow.Sequence);
            std::scoped_lock lock(s_lock);
            s_store.ArrowDone(serverId, arrow.Sequence);
        }
    }
}

//! Heads the game cut off actors this game runs, sent to the other games if the actors are still as they were then (game thread).
void SendDecapitated() noexcept
{
    std::vector<StuckArrowBeheadings::Beheading> beheadings;
    {
        std::scoped_lock lock(s_lock);
        beheadings = s_beheadings.Take();
    }

    auto view = World::Get().view<FormIdComponent, LocalComponent>();
    for (const auto& cBeheading : beheadings)
    {
        StuckArrowBeheadings::Current current{};
        if (const auto cEntity = Utils::FindByFormId(view, cBeheading.FormId))
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(cBeheading.FormId));
            const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
            const auto& cLocal = view.get<LocalComponent>(*cEntity);
            current.Local = pExtension && pExtension->IsLocal();
            current.ServerId = cLocal.Id;
            current.OwnershipEpoch = cLocal.OwnershipEpoch;
            current.Headless = current.Local && pActor->IsLimbGone(StuckArrowStore::kHeadLimb);
            current.Dead = current.Local && pActor->IsDead();
        }

        bool isCurrent = false;
        {
            std::scoped_lock lock(s_lock);
            isCurrent = s_beheadings.IsCurrent(cBeheading, current);
        }
        if (!isCurrent)
        {
            spdlog::debug("Dismember: actor {:X} ({:X}) changed hands, came back or has its head again since it lost it; not sent", cBeheading.ServerId, cBeheading.FormId);
            continue;
        }

        spdlog::debug("Dismember: actor {:X} ({:X}) lost its head here", cBeheading.ServerId, cBeheading.FormId);
        StuckArrows::SendDismember(cBeheading.ServerId, StuckArrowStore::kHeadBit, static_cast<uint8_t>(StuckArrowStore::kHeadLimb));
    }

    // Those run here as of this frame, for the beheadings the next one sends.
    std::unordered_map<uint32_t, StuckArrowBeheadings::Owner> owned;
    for (const auto cEntity : view)
    {
        const auto& cLocal = view.get<LocalComponent>(cEntity);
        owned[view.get<FormIdComponent>(cEntity).Id] = {cLocal.Id, cLocal.OwnershipEpoch};
    }
    std::scoped_lock lock(s_lock);
    s_beheadings.SetOwned(std::move(owned));
}

//! Heads other games cut off, cut off the copies here once they are dead and have their 3D.
void CutOffLimbs() noexcept
{
    const double cNow = Now();
    std::vector<std::pair<uint32_t, uint16_t>> toCut;
    std::vector<uint32_t> dropped;
    {
        std::scoped_lock lock(s_lock);
        if (!s_store.HasLimbsToCutOff())
            return;
        dropped = s_store.DropStaleLimbs(cNow);
        toCut = s_store.LimbsToCutOff();
    }

    for (const uint32_t cServerId : dropped)
        spdlog::debug("Dismember: actor {:X} keeps its head here: its copy was not dead and loaded within {} s", cServerId, StuckArrowStore::kLimbWait);

    for (const auto& [serverId, limbs] : toCut)
    {
        // Only the head for now: other limbs come from mods that may not use the engine's own storage.
        if (!(limbs & StuckArrowStore::kHeadBit))
        {
            std::scoped_lock lock(s_lock);
            s_store.LimbsCutOff(serverId);
            continue;
        }

        Actor* pActor = CopyOf(serverId);
        if (!pActor || !pActor->GetNiNode() || !pActor->IsDead())
            continue;
        {
            std::scoped_lock lock(s_lock);
            if (s_store.LimbsOfPastLife(serverId, pActor->formID))
            {
                s_store.DiscardLimbs(serverId);
                continue;
            }
        }
        const ActorExtension* pExtension = pActor->GetExtension();
        if (!pExtension || !pExtension->IsRemote())
            continue;

        if (!pActor->IsLimbGone(StuckArrowStore::kHeadLimb))
        {
            t_cutting = true;
            pActor->Decapitate();
            t_cutting = false;
            spdlog::debug("Dismember: cut the head off actor {:X} ({:X}) as its owner's game did", serverId, pActor->formID);
        }

        std::scoped_lock lock(s_lock);
        s_store.LimbsCutOff(serverId);
    }
}

} // namespace

void StuckArrows::OnNotifyStuckArrow(const NotifyStuckArrow& acMessage) noexcept
{
    StuckArrowStore::Arrow arrow{};
    arrow.AmmoModId = acMessage.Ammo.ModId;
    arrow.AmmoBaseId = acMessage.Ammo.BaseId;
    arrow.Bone.assign(acMessage.Bone.data(), acMessage.Bone.size());
    arrow.Position = acMessage.Position;
    arrow.Rotation = acMessage.Rotation;
    arrow.Scale = acMessage.Scale;
    arrow.Sequence = acMessage.Sequence;
    arrow.Removed = acMessage.Removed;
    arrow.Received = Now();
    {
        std::scoped_lock lock(s_lock);
        s_store.AddArrow(acMessage.ServerId, std::move(arrow));
    }
    spdlog::debug(
        "Stuck arrow: actor {:X} has an arrow {:X}:{:X} in {} (sequence {})", acMessage.ServerId, acMessage.Ammo.ModId, acMessage.Ammo.BaseId, acMessage.Bone.c_str(),
        acMessage.Sequence);
}

void StuckArrows::OnNotifyDismember(const NotifyDismember& acMessage) noexcept
{
    {
        std::scoped_lock lock(s_lock);
        s_store.AddLimbs(acMessage.ServerId, acMessage.Limbs, Now());
    }
    spdlog::debug("Dismember: actor {:X} lost limbs {:X} (last hit limb {})", acMessage.ServerId, acMessage.Limbs, acMessage.LastHitLimb);
}

void StuckArrows::OnSpawnLimbs(const uint32_t aServerId, const uint16_t aLimbs) noexcept
{
    std::scoped_lock lock(s_lock);
    s_store.SetLimbs(aServerId, aLimbs, Now());
}

void StuckArrows::OnRevived(const uint32_t aServerId) noexcept
{
    std::scoped_lock lock(s_lock);
    s_store.Forget(aServerId);
    s_beheadings.NewLife(aServerId);
    ReleaseLater(s_tracker.RetireServer(aServerId));
}

void StuckArrows::OnRetired(const uint32_t aServerId, const uint32_t aFormId) noexcept
{
    {
        std::scoped_lock lock(s_lock);
        s_store.Forget(aServerId);
        ReleaseLater(s_tracker.RetireServer(aServerId));
        s_beheadings.NewLife(aServerId);
        if (aFormId)
        {
            ++s_nativeLives[aFormId];
            std::erase_if(s_removals, [aFormId](const Removal& acRemoval) { return acRemoval.FormId == aFormId; });
        }
    }
    s_sequences.erase(aServerId);
}

uint16_t StuckArrows::LimbsOf(const uint32_t aServerId) noexcept
{
    std::scoped_lock lock(s_lock);
    return s_store.Limbs(aServerId);
}

uint32_t StuckArrows::PendingArrowCount(const uint32_t aServerId) noexcept
{
    std::scoped_lock lock(s_lock);
    const auto* pArrows = s_store.Arrows(aServerId);
    return pArrows ? static_cast<uint32_t>(pArrows->size()) : 0;
}

void StuckArrows::SendStuckArrow(
    const uint32_t aServerId, const uint32_t aAmmoModId, const uint32_t aAmmoBaseId, const char* apBone, const glm::vec3& acPosition, const glm::quat& acRotation,
    const float aScale, const uint32_t aSequence, const bool aRemoved) noexcept
{
    if (!apBone)
        return;

    RequestStuckArrow request{};
    request.ServerId = aServerId;
    request.Ammo = GameId(aAmmoModId, aAmmoBaseId);
    request.Bone = apBone;
    // The receiver drops a longer name, so a longer one is not sent.
    if (request.Bone.size() > StuckArrow::kMaxBoneNameLength)
        return;
    request.Position = acPosition;
    request.Rotation = acRotation;
    request.Scale = aScale;
    request.Sequence = aSequence;
    request.Removed = aRemoved;
    World::Get().GetTransport().Send(request);
}

void StuckArrows::SendDismember(const uint32_t aServerId, const uint16_t aLimbs, const uint8_t aLastHitLimb) noexcept
{
    if (!aLimbs)
        return;

    RequestDismember request{};
    request.ServerId = aServerId;
    request.Limbs = aLimbs;
    request.LastHitLimb = aLastHitLimb;
    World::Get().GetTransport().Send(request);
}

void StuckArrows::OnDecapitated(Actor* apActor) noexcept
{
    if (t_cutting || !apActor)
        return;

    // Who runs the actor is taken from the last frame, and asked again on the game thread, where the entities are.
    std::scoped_lock lock(s_lock);
    s_beheadings.Push(apActor->formID);
}

void StuckArrows::OnNativeRevival(const uint32_t aFormId) noexcept
{
    if (!s_connected.load())
        return;
    std::scoped_lock lock(s_lock);
    s_beheadings.NewLifeOfForm(aFormId);
    ++s_nativeLives[aFormId];
    s_store.NativeRevival(aFormId);
    ReleaseLater(s_tracker.Retire(aFormId));
    std::erase_if(s_removals, [aFormId](const Removal& acRemoval) { return acRemoval.FormId == aFormId; });
}

StuckArrows::InventoryReplacement::InventoryReplacement() noexcept
{
    ++t_inventoryReplacement;
}
StuckArrows::InventoryReplacement::~InventoryReplacement()
{
    --t_inventoryReplacement;
}

StuckArrows::AmmoRemoval::AmmoRemoval(TESObjectREFR* apOwner, TESForm* apAmmo) noexcept
{
    if (!s_connected.load() || t_inventoryReplacement || !apOwner || !apAmmo || apAmmo->formType != FormType::Ammo)
        return;
    Actor* pActor = Cast<Actor>(apOwner);
    TESForm* pProjectile = At<TESForm*>(apAmmo, kAmmoProjectileOffset);
    if (!pActor || !pActor->IsDead() || !pProjectile)
        return;
    Owner = pActor;
    Ammo = apAmmo;
    Root = reinterpret_cast<uintptr_t>(pActor->GetNiNode());
    Projectile = pProjectile->formID;
    const auto counts = AmmoCountsOf(pActor);
    const auto before = counts.find(Projectile);
    Before = before == counts.end() ? 0 : before->second;
    std::scoped_lock lock(s_lock);
    Session = s_session;
    Life = s_nativeLives[pActor->formID];
}

StuckArrows::AmmoRemoval::~AmmoRemoval()
{
    if (!Owner || !Root)
        return;
    const auto counts = AmmoCountsOf(Owner);
    const auto after = counts.find(Projectile);
    const int64_t cAfter = after == counts.end() ? 0 : after->second;
    if (Before <= cAfter || reinterpret_cast<uintptr_t>(Owner->GetNiNode()) != Root)
        return;
    std::scoped_lock lock(s_lock);
    if (Session == s_session && Life == s_nativeLives[Owner->formID] && s_removals.size() < 512)
        s_removals.push_back({Owner->formID, Root, Projectile, Before, cAfter, Now(), Life});
}

void StuckArrows::OnUpdate(const UpdateEvent&) noexcept
{
    s_connected.store(World::Get().GetTransport().IsConnected());
    s_updateDue.store(true);
}

void StuckArrows::OnMainLoop() noexcept
{
    if (!entt::locator<World>::has_value())
        return;
    static const BSFixedString s_loading("Loading Menu");
    static const BSFixedString s_main("Main Menu");
    const UI* pUI = UI::Get();
    if (!pUI || pUI->GetMenuOpen(s_loading) || pUI->GetMenuOpen(s_main))
        return;
    DrainReleases();
    if (!s_updateDue.exchange(false))
        return;
    // Playing alone, nothing is done to any actor.
    if (!World::Get().GetTransport().IsConnected())
        return;

    SendDecapitated();
    CutOffLimbs();

    const double cNow = Now();
    WatchArrows(cNow);
    PlaceArrows(cNow);
    {
        std::scoped_lock lock(s_lock);
        ReleaseLater(s_tracker.ExpireMade(cNow, 30.0));
    }
    DrainReleases();
}

void StuckArrows::Reset() noexcept
{
    std::scoped_lock lock(s_lock);
    s_store.Clear();
    ReleaseLater(s_tracker.Clear());
    s_sequences.clear();
    s_nextWatched = 0;
    s_watchedThisRound.clear();
    s_beheadings.Clear();
    ++s_session;
    s_connected.store(false);
    s_removals.clear();
    s_nativeLives.clear();
}
