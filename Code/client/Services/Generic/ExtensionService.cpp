#include <TiltedOnlinePCH.h>

#define UR_EXTENSION_HOST

#include <Services/ExtensionService.h>
#include <Systems/ExtensionHandlers.h>
#include <GameApi.h>
#include <World.h>
#include <Utils.h>
#include <Games/TES.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Messages/NotifyExtension.h>
#include <Messages/RequestExtension.h>
#include <Components.h>
#include <chrono>
#include <vector>
#include <set>

namespace
{
ExtensionHandlers& Handlers()
{
    static ExtensionHandlers handlers;
    return handlers;
}

uint32_t Register(uint32_t aAddon, const UrExtensionHandler* apHandler)
{
    return Handlers().Register(aAddon, apHandler);
}

uint32_t Send(const UrExtensionMessage* apMessage)
{
    if (!apMessage || apMessage->Size > UR_EXTENSION_MAX_BYTES || (apMessage->Size && !apMessage->Data) || apMessage->Result || !entt::locator<World>::has_value() ||
        !World::Get().GetTransport().IsOnline())
        return 0;

    RequestExtension request;
    auto& data = request.Data;
    data.Addon = apMessage->Addon;
    data.Version = apMessage->Version;
    data.Scope = apMessage->Scope;
    data.State = apMessage->State;
    data.Subject = apMessage->Subject;
    data.SubjectEpoch = apMessage->SubjectEpoch;
    data.Target = apMessage->Target;
    data.TargetEpoch = apMessage->TargetEpoch;
    data.Key = apMessage->Key;
    data.Revision = apMessage->Revision;
    data.WriterHigh = apMessage->WriterHigh;
    data.WriterLow = apMessage->WriterLow;
    if (apMessage->Size)
        data.Bytes.assign(apMessage->Data, apMessage->Data + apMessage->Size);

    return data.ValidRequest() && World::Get().GetTransport().Send(request);
}

uint32_t ActorId(uint32_t aFormId, uint32_t* apServerId, uint32_t* apEpoch, uint32_t* apOwned)
{
    if (!apServerId || !apEpoch || !apOwned || !entt::locator<World>::has_value())
        return 0;
    auto token = Utils::GetLocalOwnershipToken(aFormId);
    *apOwned = token.has_value();
    if (!token)
        token = Utils::GetRemoteOwnershipToken(aFormId);
    if (!token)
        return 0;
    *apServerId = token->ServerId;
    *apEpoch = token->OwnershipEpoch;
    return 1;
}

uint32_t Form(uint32_t aServerId)
{
    if (entt::locator<World>::has_value())
        if (const auto cEntity = Utils::FindEntityByServerId(aServerId))
            if (const auto* pForm = World::Get().try_get<FormIdComponent>(*cEntity))
                return pForm->Id;
    return 0;
}

uint32_t EncodeForm(uint32_t aFormId, uint32_t* apModId, uint32_t* apBaseId)
{
    return apModId && apBaseId && entt::locator<World>::has_value() && World::Get().GetModSystem().GetServerModId(aFormId, *apModId, *apBaseId);
}

uint32_t DecodeForm(uint32_t aModId, uint32_t aBaseId)
{
    return entt::locator<World>::has_value() ? World::Get().GetModSystem().GetGameId(aModId, aBaseId) : 0;
}

void* ResolveAddress(uint64_t aSeId, uint64_t aVrId)
{
#ifdef SKYRIMVR
    return VersionDb::Get().FindAddressById(aVrId);
#else
    return VersionDb::Get().FindAddressById(aSeId);
#endif
}

uint32_t FindPluginForm(const char* apPlugin, uint32_t aBaseId)
{
    const auto* pManager = ModManager::Get();
    const auto* pMod = apPlugin && pManager ? pManager->GetByName(apPlugin) : nullptr;
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aBaseId) : 0;
}
} // namespace

extern "C" __declspec(dllexport) const UrExtensionApi* urSovngarde_GetExtensionApi(uint32_t aVersion)
{
    static const UrExtensionApi api{
        UR_EXTENSION_API_VERSION,
        sizeof(UrExtensionApi),
        Register,
        Send,
        ActorId,
        Form,
        EncodeForm,
        DecodeForm,
        []() -> uint32_t { return entt::locator<World>::has_value() && World::Get().GetTransport().IsOnline() ? World::Get().GetTransport().GetLocalPlayerId() : 0; },
        []() -> uint64_t { return entt::locator<World>::has_value() ? World::Get().GetTick() : 0; },
        []() -> uint32_t { return entt::locator<World>::has_value() && World::Get().GetServerSettings().PvpEnabled; },
        UI::LoadingScreensQueued,
        ResolveAddress,
        [](uint32_t aId) -> void* { return TESForm::GetById(aId); },
        FindPluginForm,
        [](const char* apText)
        {
            if (apText)
                spdlog::debug("Add-on: {}", apText);
        }};
    return aVersion == api.Version ? &api : nullptr;
}

ExtensionService::ExtensionService(entt::dispatcher& aDispatcher)
    : m_notify(aDispatcher.sink<NotifyExtension>().connect<&ExtensionService::OnNotify>(this))
    , m_disconnect(aDispatcher.sink<DisconnectedEvent>().connect<&ExtensionService::OnDisconnect>(this))
{
}

void ExtensionService::OnNotify(const NotifyExtension& acMessage)
{
    std::scoped_lock lock(m_inboxLock);
    if (m_resync)
        return;
    if (m_inbox.size() >= 1024)
    {
        m_inbox.clear();
        m_resync = true;
        return;
    }
    m_inbox.push_back(acMessage);
}

void ExtensionService::OnDisconnect(const DisconnectedEvent&)
{
    std::scoped_lock lock(m_inboxLock);
    m_inbox.clear();
    m_disconnected = true;
    m_resync = false;
}

void ExtensionService::Update()
{
    using Clock = std::chrono::steady_clock;
    const auto cNow = Clock::now();
    const auto cElapsed = std::chrono::duration<double>(cNow - m_previous).count();
    m_previous = cNow;
    std::vector<NotifyExtension> inbox;
    bool disconnected{}, resync{};
    {
        std::scoped_lock lock(m_inboxLock);
        disconnected = std::exchange(m_disconnected, false);
        resync = std::exchange(m_resync, false);
        inbox.swap(m_inbox);
    }
    if (disconnected || resync)
        Handlers().Visit(
            [](uint32_t, const UrExtensionHandler& acHandler)
            {
                if (acHandler.Disconnect)
                    acHandler.Disconnect();
            });
    for (const auto& cNotify : inbox)
    {
        const auto& cData = cNotify.Data;
        const UrExtensionMessage cMessage{
            cData.Addon,
            cData.Version,
            cData.Scope,
            cData.State,
            cData.Subject,
            cData.SubjectEpoch,
            cData.Target,
            cData.TargetEpoch,
            cData.Key,
            cData.Bytes.data(),
            static_cast<uint32_t>(cData.Bytes.size()),
            cData.Revision,
            cData.WriterHigh,
            cData.WriterLow,
            cData.Result};
        Handlers().Receive(cNotify.Sender, cMessage);
    }
    if (disconnected)
    {
        m_previousSave = 0;
        m_announced.clear();
        m_refresh.clear();
    }
    if (!m_announced.empty() && entt::locator<World>::has_value() && World::Get().GetTransport().IsOnline())
    {
        const auto cSave = urSovngarde_GetGameApi(UR_GAME_API_VERSION)->SaveGeneration();
        if (cSave && cSave != m_previousSave)
            resync = true;
        m_previousSave = cSave;
    }
    if (resync)
        m_refresh = m_announced;
    if (entt::locator<World>::has_value())
        Handlers().Visit(
            [&](uint32_t aAddon, const UrExtensionHandler& acHandler)
            {
                const auto& cTransport = World::Get().GetTransport();
                if (cTransport.IsOnline() && (!m_announced.contains(aAddon) || m_refresh.contains(aAddon)))
                {
                    RequestExtension request;
                    request.Data.Addon = aAddon;
                    request.Data.Version = 1;
                    request.Data.State = m_refresh.contains(aAddon) ? ExtensionData::kResubscribe : ExtensionData::kSubscribe;
                    if (cTransport.Send(request))
                    {
                        // The save the subscription was made for, so the next frame does not subscribe again for the same save.
                        m_previousSave = urSovngarde_GetGameApi(UR_GAME_API_VERSION)->SaveGeneration();
                        m_announced.insert(aAddon);
                        m_refresh.erase(aAddon);
                    }
                }
                if (acHandler.Update)
                    acHandler.Update(cElapsed);
            });
}
