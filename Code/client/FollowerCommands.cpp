#include <TiltedOnlinePCH.h>

#include <FollowerCommands.h>

#include <FollowerCommand.h>
#include <Components.h>
#include <Messages/NotifyFollowerCommand.h>
#include <Messages/RequestFollowerCommand.h>
#include <Services/RunnerService.h>
#include <Services/TransportService.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <Magic/MagicCaster.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Utils.h>
#include <Services/GameExtension.h>
#include <World.h>

#include <mutex>

namespace
{
constexpr const char* kPlugin = "urSovngardeCoopMenu.esp";
// SPEL urSovngardeFollowerSPEL, self, its effect running urSovngardeFollowerEffect (Tools/Esp/make_coop_menu_esp.py).
constexpr uint32_t kSpellId = 0x806;

struct Word
{
    uint32_t ServerId{};
    uint8_t Command{};
    uint32_t FromPlayerId{};
    std::chrono::steady_clock::time_point At{};
};

// A word whose script never answered (the effect dispelled, the script missing) is answered for it after this long.
constexpr auto kWordLasts = std::chrono::seconds(5);

// Read by the script thread, written by the game's: the follower this player is choosing for, and the words waiting for followers here.
std::mutex s_lock;
uint32_t s_chosenFor{};
uint32_t s_chosenForm{};
TiltedPhoques::Map<uint32_t, Word> s_words;

MagicItem* FollowerSpell() noexcept
{
    const ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(kPlugin) : nullptr;
    if (!pMod || !pMod->IsLoaded())
        return nullptr;
    return Cast<MagicItem>(TESForm::GetById(pMod->GetFormId(kSpellId)));
}

const char* NameOf(const Actor* apActor) noexcept
{
    const auto* pBase = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    const char* pName = pBase ? pBase->fullName.value.AsAscii() : nullptr;
    return pName && *pName ? pName : "The follower";
}

// The game's own spell cast, the same as a replayed one (MagicService), so it runs the effect's script at once.
bool CastFollowerSpell(Actor* apCaster) noexcept
{
    MagicItem* pSpell = FollowerSpell();
    MagicCaster* pCaster = pSpell && apCaster ? apCaster->GetMagicCaster(MagicSystem::INSTANT) : nullptr;
    if (!pCaster)
        return false;
    pCaster->CastSpellImmediate(pSpell, false, apCaster, 1.0f, false, 0.0f);
    return true;
}

void Answer(uint32_t aServerId, uint8_t aCommand, uint8_t aReply, uint32_t aToPlayerId) noexcept
{
    RequestFollowerCommand answer{};
    answer.ServerId = aServerId;
    answer.Command = aCommand;
    answer.Reply = aReply;
    answer.ReplyToPlayerId = aToPlayerId;
    World::Get().GetTransport().Send(answer);
}

const char* Said(uint8_t aCommand, uint8_t aReply) noexcept
{
    switch (aReply)
    {
    case FollowerCommand::kDone: return aCommand == FollowerCommand::kWait ? "waits here" : "follows her player again";
    case FollowerCommand::kBusy: return "can't do that now";
    case FollowerCommand::kNotThisFollower: return "takes no word from another player";
    case FollowerCommand::kOutOfReach: return "can't hear you: you are too far, or not of her player's party";
    default: return "is no longer a follower";
    }
}
} // namespace

bool FollowerCommands::Offer(Actor* apCopy) noexcept
{
    Actor* pPlayer = PlayerCharacter::Get();
    std::optional<uint32_t> serverId;
    auto view = World::Get().view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (const auto cEntity : view)
        if (view.get<FormIdComponent>(cEntity).Id == apCopy->formID)
            serverId = Utils::GetServerId(cEntity);
    if (!serverId || !FollowerSpell())
        return false;

    {
        std::scoped_lock _(s_lock);
        s_chosenFor = *serverId;
        s_chosenForm = apCopy->formID;
    }
    spdlog::debug("Follower: copy {:X} (server id {:X}) is another player's follower; this player is asked what to tell her", apCopy->formID, *serverId);
    if (CastFollowerSpell(pPlayer))
        return true;
    std::scoped_lock _(s_lock);
    s_chosenFor = 0;
    s_chosenForm = 0;
    return false;
}

uint32_t FollowerCommands::Chosen() noexcept
{
    std::scoped_lock _(s_lock);
    return s_chosenForm;
}

void FollowerCommands::Ask(int32_t aCommand) noexcept
{
    uint32_t serverId = 0;
    {
        std::scoped_lock _(s_lock);
        serverId = std::exchange(s_chosenFor, 0);
        s_chosenForm = 0;
    }
    if (!serverId || !FollowerCommand::IsCommand(static_cast<uint8_t>(aCommand)))
        return;
    World::Get().GetRunner().Queue(
        [serverId, cCommand = static_cast<uint8_t>(aCommand)]
        {
            RequestFollowerCommand request{};
            request.ServerId = serverId;
            request.Command = cCommand;
            World::Get().GetTransport().Send(request);
            spdlog::debug("Follower: server id {:X} told to {}; sent to her player's game", serverId, cCommand == FollowerCommand::kWait ? "wait" : "follow");
        });
}

void FollowerCommands::OnNotify(const NotifyFollowerCommand& acMessage) noexcept
{
    const auto cEntity = Utils::FindEntityByServerId(acMessage.ServerId);
    const auto* pFormId = cEntity ? World::Get().try_get<FormIdComponent>(*cEntity) : nullptr;
    Actor* pActor = pFormId ? Cast<Actor>(TESForm::GetById(pFormId->Id)) : nullptr;

    // The answer to this player's word.
    if (acMessage.Reply != FollowerCommand::kAsked)
    {
        spdlog::debug(
            "Follower: server id {:X} answered command {} with reply {} from player {:X}", acMessage.ServerId, acMessage.Command, acMessage.Reply, acMessage.FromPlayerId);
        Utils::ShowHudMessage(fmt::format("{} {}", NameOf(pActor), Said(acMessage.Command, acMessage.Reply)).c_str());
        return;
    }

    // A word to a follower this game runs: only while she is still this player's follower here.
    const bool cLocal = cEntity && World::Get().all_of<LocalComponent>(*cEntity) && pActor && pActor->GetExtension() && !pActor->GetExtension()->IsRemote();
    if (!cLocal || pActor->IsDead() || !pActor->IsPlayerFollower())
    {
        spdlog::debug("Follower: player {:X}'s word to server id {:X} is for no follower this game runs", acMessage.FromPlayerId, acMessage.ServerId);
        Answer(acMessage.ServerId, acMessage.Command, FollowerCommand::kGone, acMessage.FromPlayerId);
        return;
    }

    FollowerCommand::State state{};
    state.InCombat = pActor->IsInCombat();
    state.BleedingOut = pActor->actorState.IsBleedingOut();
    state.InDialogue = MenuTopicManager::IsPlayerDialogueSpeaker(pActor);
    if (FollowerCommand::Check(state) == FollowerCommand::kBusy)
    {
        spdlog::debug(
            "Follower: {:X} is busy (fight {}, bleeding out {}, talking {}); player {:X}'s word refused", pActor->formID, state.InCombat, state.BleedingOut, state.InDialogue,
            acMessage.FromPlayerId);
        Answer(acMessage.ServerId, acMessage.Command, FollowerCommand::kBusy, acMessage.FromPlayerId);
        return;
    }

    {
        std::scoped_lock _(s_lock);
        s_words[pActor->formID] = Word{acMessage.ServerId, acMessage.Command, acMessage.FromPlayerId, std::chrono::steady_clock::now()};
    }
    if (!CastFollowerSpell(pActor))
    {
        {
            std::scoped_lock _(s_lock);
            s_words.erase(pActor->formID);
        }
        // Without the menu plugin an add-on may still carry the word out for a follower of its framework.
        const uint8_t cAddon = GameExtension::FollowerWord(pActor->formID, acMessage.Command);
        Answer(acMessage.ServerId, acMessage.Command, cAddon ? cAddon : FollowerCommand::kNotThisFollower, acMessage.FromPlayerId);
    }
}

bool FollowerCommands::IsMenuSpell(uint32_t aFormId) noexcept
{
    const ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(kPlugin) : nullptr;
    // The co-op power (0x801) and this spell (0x806): both run their script in the game that casts them.
    return pMod && pMod->IsLoaded() && (aFormId == pMod->GetFormId(0x801) || aFormId == pMod->GetFormId(kSpellId));
}

bool FollowerCommands::MayAddTradeItem(Actor* apActor, const int32_t aCount, const bool aHasOldContainer) noexcept
{
    const auto* pExtension = apActor ? apActor->GetExtension() : nullptr;
    if (aCount <= 0 || aHasOldContainer || !pExtension || !pExtension->IsRemote() || pExtension->IsPlayer() || !pExtension->IsOthersFollower())
        return false;

    const auto* pPlayer = PlayerCharacter::Get();
    const UI* pUI = UI::Get();
    FollowerCommand::TradeInventory trade{};
    trade.RemoteFollower = true;
    trade.Loaded = apActor->HasLoadedProcess() && apActor->GetNiNode();
    trade.Dead = apActor->actorState.IsDead();
    trade.InParty = World::Get().GetPartyService().IsInParty();
    trade.InReach = pPlayer && glm::distance(static_cast<glm::vec3>(apActor->position), static_cast<glm::vec3>(pPlayer->position)) <= 400.f;
    trade.MenuOpen = pUI && pUI->GetMenuOpen("ContainerMenu");
    if (trade.MenuOpen)
    {
        // CommonLib ContainerMenu::GetTargetRefHandle: AE405962, SE519421.
        // The installed SE OpenMenu writes this handle; VR's existing library also supplies519421.
        POINTER_SKYRIMSE(uint32_t, s_containerTarget, 405962, 519421);
        const uint32_t* pHandle = s_containerTarget.Get();
        trade.MenuTarget = pHandle && *pHandle && TESObjectREFR::GetByHandle(*pHandle) == apActor;
    }
    return FollowerCommand::MayAddTradeItem(trade, aCount, aHasOldContainer);
}

void FollowerCommands::Sweep() noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    TiltedPhoques::Vector<Word> late;
    {
        std::scoped_lock _(s_lock);
        for (auto it = s_words.begin(); it != s_words.end();)
        {
            if (cNow - it->second.At < kWordLasts)
            {
                ++it;
                continue;
            }
            late.push_back(it->second);
            it = s_words.erase(it);
        }
    }
    for (const auto& word : late)
    {
        spdlog::warn(
            "Follower: no answer from the follower script for server id {:X} in {} s; player {:X} is told she takes no word", word.ServerId, kWordLasts.count(), word.FromPlayerId);
        // An add-on may still carry it out for a follower of its framework.
        const auto cEntity = Utils::FindEntityByServerId(word.ServerId);
        const auto* pFormId = cEntity ? World::Get().try_get<FormIdComponent>(*cEntity) : nullptr;
        const uint8_t cAddon = pFormId ? GameExtension::FollowerWord(pFormId->Id, word.Command) : 0;
        Answer(word.ServerId, word.Command, cAddon ? cAddon : FollowerCommand::kNotThisFollower, word.FromPlayerId);
    }
}

void FollowerCommands::Reset() noexcept
{
    std::scoped_lock _(s_lock);
    s_chosenFor = 0;
    s_chosenForm = 0;
    s_words.clear();
}

int32_t FollowerCommands::Take(uint32_t aFormId) noexcept
{
    std::scoped_lock _(s_lock);
    const auto it = s_words.find(aFormId);
    return it == s_words.end() ? 0 : it->second.Command;
}

void FollowerCommands::Done(uint32_t aFormId, int32_t aCommand, int32_t aReply) noexcept
{
    Word word{};
    {
        std::scoped_lock _(s_lock);
        const auto it = s_words.find(aFormId);
        if (it == s_words.end() || it->second.Command != aCommand)
            return;
        word = it->second;
        s_words.erase(it);
    }
    const uint8_t cReply = FollowerCommand::IsReply(static_cast<uint8_t>(aReply)) ? static_cast<uint8_t>(aReply) : FollowerCommand::kNotThisFollower;
    World::Get().GetRunner().Queue(
        [aFormId, word, cScriptReply = cReply]
        {
            // Not the follower of the game's own follower quest: an add-on may carry the word out for a follower of its framework.
            const uint8_t cAddon = cScriptReply == FollowerCommand::kNotThisFollower ? GameExtension::FollowerWord(aFormId, word.Command) : 0;
            const uint8_t cReply = cAddon ? cAddon : cScriptReply;
            Answer(word.ServerId, word.Command, cReply, word.FromPlayerId);
            const auto* pActor = Cast<Actor>(TESForm::GetById(aFormId));
            spdlog::debug("Follower: {:X} {}, told by player {:X}", aFormId, Said(word.Command, cReply), word.FromPlayerId);
            if (cReply == FollowerCommand::kDone)
                Utils::ShowHudMessage(fmt::format("{} {}, told by the other player", NameOf(pActor), Said(word.Command, cReply)).c_str());
        });
}
