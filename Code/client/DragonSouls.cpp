#include <TiltedOnlinePCH.h>

#include <DragonSouls.h>

#include <DragonSoul.h>
#include <Components.h>
#include <Messages/NotifyDragonSoul.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <Forms/TESFaction.h>
#include <Forms/TESWorldSpace.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <Magic/MagicCaster.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Utils.h>
#include <World.h>

#include <atomic>
#include <cmath>
#include <mutex>

namespace
{
constexpr const char* kPlugin = "urSovngardeCoopMenu.esp";
// SPEL urSovngardeDragonSoulSPEL, self, its effect running urSovngardeDragonSoulEffect (Tools/Esp/make_coop_menu_esp.py).
constexpr uint32_t kSpellId = 0x809;
// The Dragonborn story gives some souls to Miraak and plays its own death in Apocrypha: dragons there keep the game's own absorb.
constexpr uint32_t kSolstheimWorld = 0x800;
constexpr uint32_t kApocryphaWorld = 0x1C0B2;

// Dragons left to the game's own absorb here, and those the game never absorbs by their own record.
TiltedPhoques::Set<uint32_t> s_leftToGame;
TiltedPhoques::Set<uint32_t> s_neverAbsorbed;
// Dragons this game put out of the game's own absorb, and the bodies whose soul this player had.
TiltedPhoques::Set<uint32_t> s_keptOut;
TiltedPhoques::Set<uint32_t> s_soulHad;
// Server ids whose soul this player was given, and the dragons loaded here waiting for this player to come near.
TiltedPhoques::Set<uint32_t> s_given;
TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> s_waiting;

// Read by the effect script's thread.
std::mutex s_lock;
TiltedPhoques::Vector<int32_t> s_souls;

// The co-op menu plugin's soul spell, or null when the plugin is not loaded here. The load order is fixed for the session,
// so a spell once found is kept; the game may also kill an actor off the main thread, hence the atomic.
MagicItem* SoulSpell() noexcept
{
    static std::atomic<MagicItem*> s_pSpell{nullptr};
    MagicItem* pSpell = s_pSpell.load(std::memory_order_acquire);
    if (!pSpell)
    {
        const ModManager* pManager = ModManager::Get();
        const Mod* pMod = pManager ? pManager->GetByName(kPlugin) : nullptr;
        pSpell = pMod && pMod->IsLoaded() ? Cast<MagicItem>(TESForm::GetById(pMod->GetFormId(kSpellId))) : nullptr;
        if (pSpell)
            s_pSpell.store(pSpell, std::memory_order_release);
    }
    return pSpell;
}

bool Active() noexcept
{
    const bool cServerGives = World::Get().GetServerSettings().DragonSouls;
    const bool cAccepted = World::Get().GetTransport().IsOnline();
    const bool cCanTake = SoulSpell() != nullptr;
    // Without the plugin no soul from the server could be taken: the game's own absorb stays, rather than no soul at all.
    static std::atomic<bool> s_missingLogged{false};
    if (cServerGives && cAccepted && !cCanTake && !s_missingLogged.exchange(true))
        spdlog::warn("Dragon soul: {} is not loaded here; dragons keep the game's own absorb, and no shared soul is given", kPlugin);
    return DragonSoul::KeepsDragonsOut(cServerGives, cAccepted, cCanTake);
}

bool InDragonbornWorld(const Actor* apActor) noexcept
{
    const ModManager* pManager = ModManager::Get();
    const Mod* pDragonborn = pManager ? pManager->GetByName("Dragonborn.esm") : nullptr;
    const TESWorldSpace* pWorld = apActor->GetWorldSpace();
    if (!pDragonborn || !pDragonborn->IsLoaded() || !pWorld)
        return false;
    return pWorld->formID == pDragonborn->GetFormId(kSolstheimWorld) || pWorld->formID == pDragonborn->GetFormId(kApocryphaWorld);
}

// In MQNoDragonAbsorb by its own record: a dragon the game never absorbs. Only the base counts, as every game adds the faction itself.
bool NeverAbsorbedByRecord(const Actor* apActor) noexcept
{
    const auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    if (!pNpc)
        return false;
    const auto& cFactions = pNpc->actorData.factions;
    for (auto i = 0u; i < cFactions.length; ++i)
        if (cFactions[i].faction && cFactions[i].faction->formID == DragonSoul::kNoDragonAbsorbFaction)
            return cFactions[i].rank >= 0;
    return false;
}

// In MQNoDragonAbsorb now: added by this game, or by its owner's and copied here.
bool KeptOutNow(const Actor* apActor) noexcept
{
    if (const auto* pChanges = Cast<ExtraFactionChanges>(apActor->extraData.GetByType(ExtraDataType::Faction)))
        for (auto i = 0u; i < pChanges->entries.length; ++i)
            if (pChanges->entries[i].faction && pChanges->entries[i].faction->formID == DragonSoul::kNoDragonAbsorbFaction)
                return pChanges->entries[i].rank >= 0;
    return false;
}

float DistanceToPlayer(const Actor* apActor) noexcept
{
    const Actor* pPlayer = PlayerCharacter::Get();
    const float cX = apActor->position.x - pPlayer->position.x;
    const float cY = apActor->position.y - pPlayer->position.y;
    const float cZ = apActor->position.z - pPlayer->position.z;
    return std::sqrt(cX * cX + cY * cY + cZ * cZ);
}

Actor* ByServerId(uint32_t aServerId) noexcept
{
    const auto cEntity = Utils::FindEntityByServerId(aServerId);
    const auto* pFormId = cEntity ? World::Get().try_get<FormIdComponent>(*cEntity) : nullptr;
    return pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;
}

// The effect script takes the souls queued, as the game's own absorb or the soul alone.
void Give(int32_t aSoul) noexcept
{
    if (!Active())
        return;

    MagicItem* pSpell = SoulSpell();
    Actor* pPlayer = PlayerCharacter::Get();
    MagicCaster* pCaster = pSpell && pPlayer ? pPlayer->GetMagicCaster(MagicSystem::INSTANT) : nullptr;
    if (!pCaster)
    {
        spdlog::debug("Dragon soul: no {} here; the soul is not given", kPlugin);
        return;
    }
    {
        std::scoped_lock _(s_lock);
        s_souls.push_back(aSoul);
    }
    pCaster->CastSpellImmediate(pSpell, false, pPlayer, 1.0f, false, 0.0f);
}
} // namespace

void DragonSouls::Hold(Actor* apDragon) noexcept
{
    // Actor::Kill can also run offline. Only a live co-op connection suppresses
    // the game's own absorb; Reset must remain effective after disconnect/load.
    if (!Active())
        return;

    // Checked again each time: a temporary dragon's form id is given to the next one once it is gone.
    if (!apDragon || !apDragon->IsDragon() || s_leftToGame.contains(apDragon->formID) || s_neverAbsorbed.contains(apDragon->formID) || KeptOutNow(apDragon))
        return;

    if (InDragonbornWorld(apDragon))
    {
        s_leftToGame.insert(apDragon->formID);
        spdlog::debug("Dragon soul: dragon {:X} is in the Dragonborn's lands; its soul is left to the game's own story", apDragon->formID);
        return;
    }
    if (NeverAbsorbedByRecord(apDragon))
    {
        s_neverAbsorbed.insert(apDragon->formID);
        return;
    }
    if (const auto* pFaction = Cast<TESFaction>(TESForm::GetById(DragonSoul::kNoDragonAbsorbFaction)))
    {
        apDragon->SetFactionRank(pFaction, 0);
        s_keptOut.insert(apDragon->formID);
    }
    spdlog::debug("Dragon soul: dragon {:X} kept out of the game's own absorb; its soul comes from the server", apDragon->formID);
}

void DragonSouls::Update() noexcept
{
    if (!Active())
    {
        // The server's setting turned off, or the connection is gone: back to the game's own absorb.
        if (!s_keptOut.empty())
            Reset();
        return;
    }

    static std::chrono::steady_clock::time_point s_next{};
    const auto cNow = std::chrono::steady_clock::now();
    if (cNow < s_next)
        return;
    s_next = cNow + std::chrono::milliseconds(250);

    auto view = World::Get().view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const auto cEntity : view)
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(cEntity).Id)); pActor && pActor->IsDragon())
            Hold(pActor);

    for (auto it = s_waiting.begin(); it != s_waiting.end();)
    {
        Actor* pDragon = ByServerId(it->first);
        const bool cLoaded = pDragon && pDragon->GetNiNode();
        const DragonSoul::Wait cWait = DragonSoul::WhileWaiting(cLoaded, cLoaded ? DistanceToPlayer(pDragon) : 0.f, it->second, cNow);
        if (cWait == DragonSoul::Wait::kKeepWaiting)
        {
            ++it;
            continue;
        }
        if (cWait == DragonSoul::Wait::kAbsorb)
        {
            spdlog::debug("Dragon soul: dragon {:X} (server id {:X}) absorbed here", pDragon->formID, it->first);
            s_soulHad.insert(pDragon->formID);
            Give(static_cast<int32_t>(pDragon->formID));
        }
        else
        {
            spdlog::debug("Dragon soul: server id {:X} given here without the absorb: this player stayed away, or it is gone", it->first);
            if (pDragon)
                s_soulHad.insert(pDragon->formID);
            Give(-1);
        }
        it = s_waiting.erase(it);
    }
}

void DragonSouls::OnNotify(const NotifyDragonSoul& acMessage) noexcept
{
    if (!Active())
        return;

    Actor* pDragon = ByServerId(acMessage.ServerId);
    if (pDragon)
        Hold(pDragon);
    const uint32_t cFormId = pDragon ? pDragon->formID : 0;
    const DragonSoul::Outcome cOutcome = DragonSoul::OnGrant(
        s_given.contains(acMessage.ServerId), cFormId && s_leftToGame.contains(cFormId), cFormId && s_neverAbsorbed.contains(cFormId), pDragon && pDragon->GetNiNode());
    if (cOutcome == DragonSoul::Outcome::kIgnore)
        return;
    s_given.insert(acMessage.ServerId);

    switch (cOutcome)
    {
    case DragonSoul::Outcome::kLeftToGame:
    case DragonSoul::Outcome::kNoSoul: spdlog::debug("Dragon soul: server id {:X} died; its soul is the game's own here", acMessage.ServerId); break;
    case DragonSoul::Outcome::kWaitNear:
        spdlog::debug("Dragon soul: dragon {:X} (server id {:X}) died; absorbed when this player comes near", cFormId, acMessage.ServerId);
        s_waiting[acMessage.ServerId] = std::chrono::steady_clock::now();
        break;
    default:
        spdlog::debug("Dragon soul: server id {:X} died far from here; its soul given without the absorb", acMessage.ServerId);
        if (cFormId)
            s_soulHad.insert(cFormId);
        Give(-1);
        break;
    }
}

int32_t DragonSouls::Take() noexcept
{
    std::scoped_lock _(s_lock);
    // Script thread: disconnect cleanup clears the queue.
    if (s_souls.empty())
        return 0;
    const int32_t cSoul = s_souls.front();
    s_souls.erase(s_souls.begin());
    return cSoul;
}

bool DragonSouls::LiftForSave(Actor* apActor) noexcept
{
    if (!apActor || !DragonSoul::BackInGameAbsorb(s_keptOut.contains(apActor->formID), apActor->IsDead(), s_soulHad.contains(apActor->formID)))
        return false;
    const auto* pFaction = Cast<TESFaction>(TESForm::GetById(DragonSoul::kNoDragonAbsorbFaction));
    if (!pFaction)
        return false;
    apActor->SetFactionRank(pFaction, -1);
    return true;
}

void DragonSouls::RestoreAfterSave(Actor* apActor) noexcept
{
    if (const auto* pFaction = Cast<TESFaction>(TESForm::GetById(DragonSoul::kNoDragonAbsorbFaction)); pFaction && apActor)
        apActor->SetFactionRank(pFaction, 0);
}

void DragonSouls::Reset() noexcept
{
    // Back in the game's own absorb, as alone; a body whose soul this player had stays out of it.
    uint32_t released = 0;
    if (const auto* pFaction = Cast<TESFaction>(TESForm::GetById(DragonSoul::kNoDragonAbsorbFaction)))
    {
        for (const uint32_t cFormId : s_keptOut)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(cFormId));
            if (!pActor || !pActor->IsDragon() || !DragonSoul::BackInGameAbsorb(true, pActor->IsDead(), s_soulHad.contains(cFormId)))
                continue;
            pActor->SetFactionRank(pFaction, -1);
            ++released;
        }
    }
    if (released)
        spdlog::debug("Dragon soul: {} dragon(s) back in the game's own absorb at the disconnect", released);
    s_keptOut.clear();
    s_soulHad.clear();
    s_leftToGame.clear();
    s_neverAbsorbed.clear();
    s_given.clear();
    s_waiting.clear();
    std::scoped_lock _(s_lock);
    s_souls.clear();
}
