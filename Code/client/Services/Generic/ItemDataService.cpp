#include <TiltedOnlinePCH.h>

#include <Services/ItemDataService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/RequestItemDataChange.h>
#include <Messages/NotifyItemDataChange.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Events/InventoryChangeEvent.h>

#include <Games/Overrides.h>
#include <ItemExtraData.h>
#include <PlayerCharacter.h>
#include <Components.h>
#include <World.h>

using namespace std::chrono_literals;

namespace
{
constexpr size_t kMaxDeltas = 1024;

// What can change where it lies: weapons and armour (temper, charge, poison, name) and soul gems (a soul caught).
bool CanChangeInPlace(TESForm& aForm) noexcept
{
    return aForm.formType == FormType::Weapon || aForm.formType == FormType::Armor || aForm.formType == FormType::SoulGem;
}
} // namespace

ItemDataService::ItemDataService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ItemDataService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ItemDataService::OnDisconnected>(this);
    m_notifyConnection = aDispatcher.sink<NotifyItemDataChange>().connect<&ItemDataService::OnNotifyItemDataChange>(this);
    m_inventoryConnection = aDispatcher.sink<InventoryChangeEvent>().connect<&ItemDataService::OnInventoryChange>(this);
    m_inventoryNotifyConnection = aDispatcher.sink<NotifyInventoryChanges>().connect<&ItemDataService::OnNotifyInventoryChanges>(this);
}

void ItemDataService::OnInventoryChange(const InventoryChangeEvent& acEvent) noexcept
{
    // Past the cap a reading cannot be told from what came and went; it is then left out as changed counts are.
    if (acEvent.FormId == 0x14 && m_hasLast && m_deltas.size() < kMaxDeltas)
        m_deltas.push_back(acEvent.Item);
}

void ItemDataService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (m_hasLast && acMessage.ServerId == m_lastServerId && m_deltas.size() < kMaxDeltas)
        m_deltas.push_back(acMessage.Item);
}

void ItemDataService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < m_nextRead)
        return;
    m_nextRead = cNow + 1s;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == 0x14; });
    if (it == view.end())
        return;
    const auto& cLocal = view.get<LocalComponent>(*it);

    Inventory now = pPlayer->GetInventory(&CanChangeInPlace);

    // A new character on the server starts from what it has now.
    if (!m_hasLast || m_lastServerId != cLocal.Id)
    {
        m_last = std::move(now);
        m_lastServerId = cLocal.Id;
        m_hasLast = true;
        m_deltas.clear();
        return;
    }

    Vector<ItemDataChange> changes = ItemDataChanges::Find(ItemDataChanges::Expect(m_last, m_deltas), now);
    m_last = std::move(now);
    m_deltas.clear();
    if (changes.empty())
        return;

    RequestItemDataChange request{};
    request.ServerId = cLocal.Id;
    request.OwnershipEpoch = cLocal.OwnershipEpoch;
    for (ItemDataChange& change : changes)
    {
        if (!ItemDataChanges::IsSyncable(change.Old) || !ItemDataChanges::IsSyncable(change.New))
            continue;
        if (request.Changes.size() == ItemDataChanges::kMaxChanges)
        {
            m_transport.Send(request);
            request.Changes.clear();
        }
        spdlog::debug("ItemData: {:X}:{:X} x{} changed where it lies", change.Old.BaseId.ModId, change.Old.BaseId.BaseId, change.Old.Count);
        request.Changes.push_back(std::move(change));
    }
    if (!request.Changes.empty())
        m_transport.Send(request);
}

void ItemDataService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_last = {};
    m_lastServerId = 0;
    m_hasLast = false;
    m_deltas.clear();
}

void ItemDataService::OnNotifyItemDataChange(const NotifyItemDataChange& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(
        view.begin(), view.end(),
        [view, &acMessage](const entt::entity aEntity)
        {
            const auto& cRemote = view.get<RemoteComponent>(aEntity);
            return cRemote.Id == acMessage.ServerId && cRemote.OwnershipEpoch == acMessage.OwnershipEpoch;
        });
    if (it == view.end())
        return;

    // Not loaded here: the copy gets its items with its next spawn, which the server keeps up to date.
    auto* pCopy = Cast<TESObjectREFR>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pCopy)
        return;

    for (const ItemDataChange& change : acMessage.Changes)
    {
        const int32_t cChanged = ItemExtraData::Apply(pCopy, change);
        if (cChanged < change.Old.Count)
            spdlog::debug("ItemData: copy {:X} changed {} of {} {:X}:{:X}", pCopy->formID, cChanged, change.Old.Count, change.Old.BaseId.ModId, change.Old.BaseId.BaseId);
    }
}
