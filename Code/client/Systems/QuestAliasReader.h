#pragma once

#include <cstddef>
#include <cstdint>

struct TESQuest;

namespace QuestAliasReader
{
// The start of every alias, as CommonLibSSE and CommonLibVR lay out BGSBaseAlias. A reader checks owningQuest before
// trusting the rest: a different layout then reads as a mismatch, not as other fields.
struct AliasHeader
{
    void* vtable;
    BSFixedString aliasName; // 0x08
    TESQuest* owningQuest;   // 0x10
    uint32_t aliasID;        // 0x18
    uint32_t flags;          // 0x1C
};
static_assert(offsetof(AliasHeader, aliasName) == 0x08);
static_assert(offsetof(AliasHeader, owningQuest) == 0x10);
static_assert(offsetof(AliasHeader, aliasID) == 0x18);
} // namespace QuestAliasReader
