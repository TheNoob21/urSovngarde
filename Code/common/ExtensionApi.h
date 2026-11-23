#pragma once

#include <stdint.h>

#define UR_EXTENSION_API_VERSION 1u
#define UR_EXTENSION_MAX_BYTES 8192u

enum UrExtensionScope
{
    UR_EXTENSION_RANGE,
    UR_EXTENSION_PARTY,
    UR_EXTENSION_OWNER
};

enum UrExtensionState
{
    UR_EXTENSION_EVENT,
    UR_EXTENSION_REPLACE,
    UR_EXTENSION_ERASE,
    UR_EXTENSION_STORE,
    UR_EXTENSION_REMOVE,
    UR_EXTENSION_READ,
    UR_EXTENSION_CLAIM,
    UR_EXTENSION_RELEASE,
    // 8 is the client's subscribe and 13 its resubscribe (ExtensionData::kSubscribe, kResubscribe); an add-on never sends them.
    UR_EXTENSION_REPLACE_OWNED_TARGET = 9,
    UR_EXTENSION_ERASE_OWNED_TARGET,
    UR_EXTENSION_RESERVE,
    UR_EXTENSION_UNRESERVE,
    UR_EXTENSION_AREA_REPLACE = 14,
    UR_EXTENSION_AREA_ERASE
};

typedef struct UrExtensionMessage
{
    uint32_t Addon;
    uint32_t Version;
    uint32_t Scope;
    uint32_t State;
    uint32_t Subject;
    uint32_t SubjectEpoch;
    uint32_t Target;
    uint32_t TargetEpoch;
    uint64_t Key;
    const uint8_t* Data;
    uint32_t Size;
    // Durable records use compare-and-swap. Only their writer knows this 128-bit capability.
    uint64_t Revision;
    uint64_t WriterHigh;
    uint64_t WriterLow;
    // Zero for broadcasts, one for an acknowledgement, two for a rejected request.
    uint32_t Result;
} UrExtensionMessage;

// Callbacks run on the game thread. Data is borrowed for the duration of Receive.
// Register once during plugin startup. Registered modules stay loaded until game exit.
typedef struct UrExtensionHandler
{
    void (*Receive)(uint32_t aSenderPlayer, const UrExtensionMessage* apMessage);
    void (*Update)(double aSeconds);
    void (*Disconnect)(void);
} UrExtensionHandler;

typedef struct UrExtensionApi
{
    uint32_t Version;
    uint32_t Size;
    uint32_t (*Register)(uint32_t aAddon, const UrExtensionHandler* apHandler);
    // All calls below run on the game thread. Zero subject and epoch select the sender's character.
    // An explicit subject must be owned at its epoch. Target, when given, must be in range.
    uint32_t (*Send)(const UrExtensionMessage* apMessage);
    uint32_t (*Actor)(uint32_t aFormId, uint32_t* apServerId, uint32_t* apEpoch, uint32_t* apOwned);
    uint32_t (*Form)(uint32_t aServerId);
    uint32_t (*EncodeForm)(uint32_t aFormId, uint32_t* apModId, uint32_t* apBaseId);
    uint32_t (*DecodeForm)(uint32_t aModId, uint32_t aBaseId);
    uint32_t (*LocalPlayer)(void);
    uint64_t (*NetworkTick)(void);
    uint32_t (*PvpEnabled)(void);
    uint32_t (*LoadingScreensQueued)(void);
    void* (*ResolveAddress)(uint64_t aSeId, uint64_t aVrId);
    void* (*NativeForm)(uint32_t aFormId);
    uint32_t (*FindPluginForm)(const char* apPlugin, uint32_t aBaseId);
    // Writes one line to the client's log at debug level. Any thread.
    void (*Log)(const char* apText);
} UrExtensionApi;

#ifdef __cplusplus
extern "C"
{
#endif
// Resolve from the main module. An unsupported version returns null.
#ifdef UR_EXTENSION_HOST
    __declspec(dllexport)
#endif
    const UrExtensionApi*
    urSovngarde_GetExtensionApi(uint32_t aVersion);
#ifdef __cplusplus
}
#endif
