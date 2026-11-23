#pragma once

#include <Misc/BSFixedString.h>

struct TESForm;
struct MiddleProcess;
struct TESAmmo;
struct bhkCharacterController;

struct AIProcess
{
    bhkCharacterController* GetCharController() noexcept;
    bool SetCurrentAmmo(TESAmmo* apAmmo) noexcept;

    void KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude);

    void* unk0;
    MiddleProcess* middleProcess;
    struct HighProcessData* highProcess;
    void* packageLock;
    struct TESPackage* package;
    uint32_t unk10[7];
    uint32_t unk34[8];
    float unk54;
    uint32_t unk58[4];
    TESForm* equippedObject[2];

    uint8_t pad88[0x137 - 0x88];

    int8_t movementType;
};

// The attack the game picked for an actor's swing (the attack's graph event and its ATKD flags), as in VR.
struct BGSAttackData
{
    void* pVTable;
    uint32_t refCount;
    uint32_t pad0C;
    BSFixedString event;
    float damageMult;
    float attackChance;
    void* pAttackSpell;
    uint32_t flags;
};

struct HighProcessData
{
    uint8_t pad0[0x218];
    char* strVoiceSubtitle;
    GameArray<std::tuple<uint32_t, void*>> KnowledgeArray; // BSTuple, std::tuple is prolly wrong
    uint8_t pad238[0x258 - 0x238];
    BGSAttackData* attackData; // null when none
};

// The game reads the same layout on VR 1.4.15: AIProcess's attack data getter (0x664650) is high process + 0x258, and
// HitData::Populate (SE 42832, VR 0x76D400) reads its damage at +0x18, spell at +0x20 and power attack bit at +0x28. The ATKD
// loader (0x3F7B40) fills +0x10 (event) and +0x18 (the ATKD record) of a 0x50 byte object, as on flat.
static_assert(offsetof(BGSAttackData, event) == 0x10);
static_assert(offsetof(BGSAttackData, flags) == 0x28);
static_assert(offsetof(HighProcessData, attackData) == 0x258);

static_assert(offsetof(AIProcess, highProcess) == 0x10);
static_assert(offsetof(AIProcess, movementType) == 0x137);
