#include <TiltedOnlinePCH.h>

#include <Services/Generic/NpcEquipmentCheck.h>

#include <Services/NpcEquipmentQueue.h>
#include <World.h>
#include <Components.h>
#include <Services/TransportService.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Actor.h>
#include <EquipManager.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESBoundObject.h>
#include <Games/ActorExtension.h>
#include <Games/Overrides.h>
#include <Services/Generic/WornSetRules.h>

namespace
{
// The first check after a change, and the second after the first.
constexpr auto kFirstCheckAfter = std::chrono::seconds(1);
constexpr auto kSecondCheckAfter = std::chrono::seconds(2);
constexpr uint32_t kChecksPerChange = 2;

// The checks waiting, oldest first.
TiltedPhoques::Vector<NpcEquipmentQueue::PendingCheck> s_pending;

// For each remote copy, by form id: the armor its owner was last known to wear, from its spawn data and then each snapshot applied.
// A piece in it that the owner no longer wears comes off the copy (WornSetRules::Compare).
TiltedPhoques::Map<uint32_t, std::vector<uint32_t>> s_ownerWorn;
// Copies come and go. Past this many, all is forgotten at once; a copy then takes off only replaced pieces until its next snapshot.
constexpr size_t kMaxOwnerWorn = 1024;

/**
 * List the armor an equipment list has worn that this game can name, with the slots each covers.
 *
 * @param acEquipment The equipment list, as the owner's game sent it.
 * @param appAmmo Set to the worn ammo this game can name, when not null.
 * @return The armor pieces.
 */
std::vector<WornSetRules::Piece> NamedWornArmor(const Inventory& acEquipment, TESForm** appAmmo = nullptr) noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    std::vector<WornSetRules::Piece> pieces;
    for (const auto& entry : acEquipment.Entries)
    {
        if (!entry.IsWorn())
            continue;
        TESForm* pForm = TESForm::GetById(modSystem.GetGameId(entry.BaseId));
        if (!pForm)
            continue;
        if (const auto* pArmor = Cast<TESObjectARMO>(pForm); pArmor && pForm->formType == FormType::Armor)
            pieces.push_back({pForm->formID, pArmor->slotType});
        else if (appAmmo && pForm->formType == FormType::Ammo)
            *appAmmo = pForm;
    }
    return pieces;
}

/**
 * Remember the armor a copy's owner wears now.
 *
 * @param aFormId The copy's form id.
 * @param acPieces The armor its owner wears that this game can name.
 */
void RememberOwnerWorn(const uint32_t aFormId, const std::vector<WornSetRules::Piece>& acPieces, const bool aAddOnly = false) noexcept
{
    if (s_ownerWorn.size() >= kMaxOwnerWorn && s_ownerWorn.find(aFormId) == s_ownerWorn.end())
        s_ownerWorn.clear();
    std::vector<uint32_t>& ids = s_ownerWorn[aFormId];
    if (!aAddOnly)
        ids.clear();
    for (const auto& cPiece : acPieces)
        if (std::find(ids.begin(), ids.end(), cPiece.Id) == ids.end())
            ids.push_back(cPiece.Id);
}

/**
 * Find the shield an equipment list has worn.
 *
 * @param acInventory The equipment list.
 * @return The shield, or null when none is worn or it is not loaded in this game.
 */
TESObjectARMO* WornShield(const Inventory& acInventory) noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    for (const auto& entry : acInventory.Entries)
    {
        if (!entry.IsWorn())
            continue;
        auto* pArmor = Cast<TESObjectARMO>(TESForm::GetById(modSystem.GetGameId(entry.BaseId)));
        if (pArmor && pArmor->IsShield())
            return pArmor;
    }
    return nullptr;
}

/**
 * Make sure a copy holds an item before it is equipped: equipping an item it lacks does nothing, and an inventory change may not have reached this game.
 *
 * @param apActor The copy.
 * @param apForm The item.
 * @return True when the copy holds at least one.
 */
bool EnsureHeld(Actor* apActor, TESForm* apForm) noexcept
{
    if (apActor->GetItemCountInInventory(apForm) > 0)
        return true;
    if (auto* pObject = Cast<TESBoundObject>(apForm))
    {
        ScopedInventoryOverride _;
        apActor->AddObjectToContainer(pObject, nullptr, 1, nullptr);
    }
    return apActor->GetItemCountInInventory(apForm) > 0;
}
} // namespace

namespace NpcEquipmentCheck
{
/**
 * Check whether two equipment lists hold the same items, worn in the same hands, with the same spells.
 * Charges and other extra data are not compared.
 *
 * @param acLhs One equipment list.
 * @param acRhs The other equipment list.
 * @return True when both list the same items in the same order, worn the same way, with the same magic equipment.
 */
bool HasSameWornItems(const Inventory& acLhs, const Inventory& acRhs) noexcept
{
    if (acLhs.Entries.size() != acRhs.Entries.size() || !(acLhs.CurrentMagicEquipment == acRhs.CurrentMagicEquipment))
        return false;

    for (size_t i = 0; i < acLhs.Entries.size(); ++i)
    {
        const auto& lhs = acLhs.Entries[i];
        const auto& rhs = acRhs.Entries[i];
        if (lhs.BaseId != rhs.BaseId || lhs.ExtraWorn != rhs.ExtraWorn || lhs.ExtraWornLeft != rhs.ExtraWornLeft)
            return false;
    }

    return true;
}

/**
 * Check whether an equipment list has an item worn, in either hand or as armor.
 *
 * @param acInventory The equipment list.
 * @param acItemId The item, as the server names it.
 * @return True when the list has that item worn.
 */
bool IsWorn(const Inventory& acInventory, const GameId& acItemId) noexcept
{
    return std::any_of(acInventory.Entries.begin(), acInventory.Entries.end(), [&acItemId](const Inventory::Entry& acEntry) { return acEntry.BaseId == acItemId && acEntry.IsWorn(); });
}

/**
 * Queue two checks of what a local NPC wears after one of its equipment changes is sent: 1 s and 3 s later.
 * A difference from what was sent goes out as an equipment snapshot.
 * A check already waiting for that NPC is merged into the new one, so a change the game undid at once is not lost (NpcEquipmentQueue::Queue).
 *
 * @param aFormId The NPC's form id in this game.
 * @param aServerId The NPC's id on the server.
 * @param aOwnershipEpoch The ownership epoch the change was sent with.
 * @param acSent The equipment sent with the change.
 * @param aUndone True when the change was already undone when it was sent, so the first check sends what the NPC wears whatever it finds.
 */
void Note(const uint32_t aFormId, const uint32_t aServerId, const uint32_t aOwnershipEpoch, const Inventory& acSent, const bool aUndone) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    NpcEquipmentQueue::Queue(s_pending, {aFormId, aServerId, aOwnershipEpoch, acSent, cNow + kFirstCheckAfter, kChecksPerChange, cNow, aUndone});
}

/**
 * Run the checks that are due, and send an equipment snapshot for each NPC that wears other items than were sent or had a change undone.
 * A check is dropped when its NPC is no longer this game's at the same epoch, and all are dropped while not connected.
 * Called every frame.
 *
 * @param aWorld The world, for the NPCs this game owns.
 * @param aTransport The connection the snapshots are sent through.
 */
void Run(World& aWorld, TransportService& aTransport) noexcept
{
    if (s_pending.empty())
        return;
    if (!aTransport.IsConnected())
    {
        s_pending.clear();
        return;
    }

    const auto cNow = std::chrono::steady_clock::now();
    auto view = aWorld.view<FormIdComponent, LocalComponent>();
    for (auto it = s_pending.begin(); it != s_pending.end();)
    {
        if (cNow < it->Due)
        {
            ++it;
            continue;
        }

        // Still this game's at the same epoch: otherwise its new owner sends what it wears.
        const bool cStillOurs = std::any_of(view.begin(), view.end(), [&view, &it](const entt::entity aEntity)
        {
            const auto& local = view.get<LocalComponent>(aEntity);
            return view.get<FormIdComponent>(aEntity).Id == it->FormId && local.Id == it->ServerId && local.OwnershipEpoch == it->OwnershipEpoch;
        });
        Actor* pActor = cStillOurs ? Cast<Actor>(TESForm::GetById(it->FormId)) : nullptr;
        if (!pActor || !pActor->GetNiNode() || pActor->GetExtension()->IsRemote())
        {
            it = s_pending.erase(it);
            continue;
        }

        Inventory worn = pActor->GetEquipment();
        if (it->Undone || !HasSameWornItems(worn, it->Sent))
        {
            const TESObjectARMO* pSentShield = WornShield(it->Sent);
            const TESObjectARMO* pWornShield = WornShield(worn);
            spdlog::info("Equipment sync: NPC {:X} {} {} ms after its last change was sent ({} worn entries then, {} now; shield {:X} then, {:X} now); "
                         "its equipment sent again",
                         it->FormId, it->Undone ? "had that change undone at once" : "wears other items", std::chrono::duration_cast<std::chrono::milliseconds>(cNow - it->SentAt).count(),
                         it->Sent.Entries.size(), worn.Entries.size(), pSentShield ? pSentShield->formID : 0, pWornShield ? pWornShield->formID : 0);
            it->Undone = false;

            // No item: the others apply it as a snapshot (InventoryService::OnNotifyEquipmentChanges).
            RequestEquipmentChanges request;
            request.ServerId = it->ServerId;
            request.OwnershipEpoch = it->OwnershipEpoch;
            request.CurrentInventory = worn;
            aTransport.Send(request);
            it->Sent = std::move(worn);
        }

        if (--it->ChecksLeft == 0)
        {
            it = s_pending.erase(it);
            continue;
        }
        it->Due = cNow + kSecondCheckAfter;
        ++it;
    }
}

/**
 * Remember what a remote copy's owner wears, from the copy's spawn or assignment data, so its next snapshot can take off what the owner took off.
 *
 * @param apActor The copy. Nothing is kept for an actor this game owns.
 * @param acOwnerInventory The inventory its owner's game sent, with the worn flags.
 */
void NoteOwnerEquipment(Actor* apActor, const Inventory& acOwnerInventory, const bool aAddOnly) noexcept
{
    if (!apActor || !apActor->GetExtension() || !apActor->GetExtension()->IsRemote())
        return;
    RememberOwnerWorn(apActor->formID, NamedWornArmor(acOwnerInventory), aAddOnly);
}

/**
 * Make an NPC's copy wear every armor piece its owner wears (shield, rings, amulet, helmet and the rest) and its ammo, from an equipment snapshot.
 * A piece of the copy comes off when a piece its owner wears takes its slot, or when its owner wore it before (spawn data or an earlier snapshot) and no longer does.
 * A piece its owner was never known to wear stays when nothing takes its slot: its owner may wear one there that this game cannot name.
 * The shield is the exception, as upstream's rule has it: a snapshot with no shield takes the copy's shield off.
 * A piece the copy lacks is added to its inventory first; when that fails, the copy keeps its own.
 *
 * @param apActor The copy.
 * @param acOwnerEquipment The equipment its owner's game sent.
 */
void ApplyWornItems(Actor* apActor, const Inventory& acOwnerEquipment) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    // What the owner wears that this game can name: armor of every slot (shield, rings, amulet, helmet) and the ammo.
    // A piece from a plugin the server does not know never reaches the snapshot, so a piece is not taken off only for being absent from it:
    // its owner must have worn it before (s_ownerWorn), or a piece put on must take its slot.
    TESForm* pWantedAmmo = nullptr;
    const std::vector<WornSetRules::Piece> wanted = NamedWornArmor(acOwnerEquipment, &pWantedAmmo);

    std::vector<WornSetRules::Piece> worn;
    for (const auto& entry : apActor->GetWornArmor().Entries)
    {
        if (const auto* pArmor = Cast<TESObjectARMO>(TESForm::GetById(modSystem.GetGameId(entry.BaseId))))
            worn.push_back({pArmor->formID, pArmor->slotType});
    }

    static const std::vector<uint32_t> s_none;
    const auto cLast = s_ownerWorn.find(apActor->formID);
    const std::vector<uint32_t> cLastWanted = cLast != s_ownerWorn.end() ? cLast->second : s_none;
    RememberOwnerWorn(apActor->formID, wanted);

    auto* pEquipManager = EquipManager::Get();
    const auto cChanges = WornSetRules::Compare(wanted, worn, cLastWanted);
    for (const uint32_t cId : cChanges.Remove)
    {
        if (TESForm* pArmor = TESForm::GetById(cId))
        {
            pEquipManager->UnEquip(apActor, pArmor, nullptr, 1, nullptr, false, true, false, false, nullptr);
            spdlog::debug("Equipment sync: remote actor {:X} takes off {:X}, which its owner no longer wears", apActor->formID, cId);
        }
    }
    // The shield follows its owner's even with nothing to replace it, as upstream's ApplyShield did.
    if (!WornShield(acOwnerEquipment))
    {
        if (TESObjectARMO* pShield = WornShield(apActor->GetWornArmor());
            pShield && std::find(cChanges.Remove.begin(), cChanges.Remove.end(), pShield->formID) == cChanges.Remove.end())
        {
            pEquipManager->UnEquip(apActor, pShield, nullptr, 1, nullptr, false, true, false, false, nullptr);
            spdlog::debug("Equipment sync: remote actor {:X} takes off shield {:X}, as its owner wears none", apActor->formID, pShield->formID);
        }
    }
    for (const uint32_t cId : cChanges.Add)
    {
        TESForm* pArmor = TESForm::GetById(cId);
        if (pArmor && EnsureHeld(apActor, pArmor))
        {
            pEquipManager->Equip(apActor, pArmor, nullptr, 1, nullptr, false, true, false, false);
            spdlog::debug("Equipment sync: remote actor {:X} puts on {:X}, which its owner wears", apActor->formID, cId);
        }
        else
            spdlog::debug("Equipment sync: remote actor {:X} could not be given {:X}; its own left as it is", apActor->formID, cId);
    }

    // The ammo its owner wears. An owner with none that this game can name leaves the copy's ammo as it is.
    if (pWantedAmmo && pWantedAmmo != apActor->GetEquippedAmmo() && EnsureHeld(apActor, pWantedAmmo))
    {
        pEquipManager->Equip(apActor, pWantedAmmo, nullptr, apActor->GetItemCountInInventory(pWantedAmmo), nullptr, false, true, false, false);
        spdlog::debug("Equipment sync: remote actor {:X} equips ammo {:X} like its owner", apActor->formID, pWantedAmmo->formID);
    }
}
} // namespace NpcEquipmentCheck
