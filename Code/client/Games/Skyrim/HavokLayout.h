#pragma once

#include <Helpers/ReadableMemory.h>
#include "NetImmerse/NiTree.h"
#include <cstdint>
#include <cstring>

//! Raw NetImmerse and Havok layouts that VRBodySync and PropHavok read and write, and the ids they call.
//! From PLANCK's static_asserts and CommonLibVR-NG: the VR NiAVObject is 0x138 bytes, the client's structs model SE's 0x110.
namespace HavokLayout
{
using NiLayout::kChildrenOffset;
using NiLayout::kWorldOffset;
inline constexpr uint32_t kVTableAsNodeSlot = NiTree::kAsNodeSlot;
inline constexpr uint32_t kVTableGetRttiSlot = 2; // NiObject::GetRTTI

// First-person BipedAnim active BIPOBJECT entries, verified on both editions.
inline constexpr uint32_t kBipedObjectsOffset = 0x10;
inline constexpr uint32_t kBipedObjectSize = 0x78;
inline constexpr uint32_t kBipedCloneOffset = 0x20;
inline constexpr size_t kBipedObjectCount = 42;

inline constexpr uint32_t kObjectNameOffset = NiLayout::kNameOffset; // NiObjectNET::name
inline constexpr uint32_t kChildDataOffset = NiLayout::kChildSlotsOffset;
inline constexpr uint32_t kChildCapacityOffset = NiLayout::kChildCountOffset;
inline constexpr uint32_t kChildEndOffset = kChildrenOffset + 0x12; // NiTArray::freeIdx
inline constexpr uint32_t kWorldObjectOffset = 0x10;                // bhkWorld::referencedObject
using NiLayout::kCollisionObjectOffset;
inline constexpr uint32_t kCollisionBodyOffset = 0x20;       // bhkNiCollisionObject::body (bhkWorldObject)
inline constexpr uint32_t kHavokBodyOffset = 0x10;           // bhkWorldObject::hkBody (hkpRigidBody)
inline constexpr uint32_t kHkWorldOffset = 0x10;             // hkpWorldObject::world
inline constexpr uint32_t kMotionTypeOffset = 0x160;         // hkpEntity::motion.type
inline constexpr uint32_t kBodyTranslationOffset = 0x1A0;    // hkpEntity::motion.motionState.transform.translation
inline constexpr uint32_t kCenterOfMassOffset = 0x1C0;       // hkpEntity::motion.motionState.sweptTransform.centerOfMass1
inline constexpr uint32_t kBodyQuaternionOffset = 0x1E0;     // hkpEntity::motion.motionState.sweptTransform.rotation1, what the keyframe turns from
inline constexpr uint32_t kLinearVelocityOffset = 0x230;     // hkpEntity::motion.linearVelocity
inline constexpr uint32_t kAngularVelocityOffset = 0x240;    // hkpEntity::motion.angularVelocity
inline constexpr uint32_t kBhkWorldOfHkpWorldOffset = 0x430; // ahkpWorld::m_userData (bhkWorld)
inline constexpr uint32_t kWorldLockOffset = 0xC598;         // bhkWorld::worldLock

// hkpEntity constraint arrays and hkpConstraintInstance endpoints.
inline constexpr uint32_t kConstraintsMasterOffset = 0x100;
inline constexpr uint32_t kConstraintsMasterSizeOffset = 0x108;
inline constexpr uint32_t kConstraintsSlaveOffset = 0x110;
inline constexpr uint32_t kConstraintsSlaveSizeOffset = 0x118;
inline constexpr uint32_t kConstraintInternalSize = 0x40;
inline constexpr uint32_t kConstraintDataOffset = 0x18;
inline constexpr uint32_t kConstraintEntityAOffset = 0x28;
inline constexpr uint32_t kConstraintEntityBOffset = 0x30;
inline constexpr uint32_t kConstraintGetTypeSlot = 0x0B;

// The flat game's ids; VR keeps its own in the second slot of each POINTER_SKYRIMSE.
// The address library of SkyrimSE.exe 1.7.104 is keyed by AE ids, so an SE id there names another function.
inline constexpr uint32_t kHavokWorldScaleAeId = 188105;  // bhkWorld::GetWorldScale's float (CommonLibSSE-NG); AE id, checked on SkyrimSE.exe 1.7.104
inline constexpr uint32_t kLockForWriteAeId = 68234;      // BSReadWriteLock::LockForWrite; AE id, checked on SkyrimSE.exe 1.7.104
inline constexpr uint32_t kUnlockForWriteAeId = 68240;    // BSReadWriteLock::UnlockForWrite; AE id, checked on SkyrimSE.exe 1.7.104
inline constexpr uint32_t kHkpEntityActivateAeId = 60849; // hkpEntity::activate (SE and VR 60096); AE id, 0x140b4aff0 on SkyrimSE.exe 1.7.104

using TWorldLock = void(void* apLock);
using THkpEntityActivate = void(void* apEntity);

template <class T> T& At(void* apObject, uint32_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

using NiTree::AsNode;

using ReadableMemory::IsReadable;

inline const char* RttiNameOf(void* apObject) noexcept
{
    if (!apObject || !IsReadable(apObject, sizeof(void*)))
        return nullptr;
    auto** ppVTable = *static_cast<void***>(apObject);
    if (!ppVTable || !IsReadable(ppVTable + kVTableGetRttiSlot, sizeof(void*)))
        return nullptr;
    using TGetRtti = const char**(__fastcall*)(void*);
    const char** ppRtti = static_cast<TGetRtti>(ppVTable[kVTableGetRttiSlot])(apObject);
    return ppRtti && IsReadable(ppRtti, sizeof(void*)) && ppRtti[0] && IsReadable(ppRtti[0], 1) ? ppRtti[0] : nullptr;
}

// The Havok body behind a node, or null. Every pointer is checked before it is followed, and aBodyBytes of the body.
inline uint8_t* HavokBodyOf(void* apNode, const size_t aBodyBytes = kMotionTypeOffset + 1) noexcept
{
    if (!apNode || !IsReadable(static_cast<uint8_t*>(apNode) + kCollisionObjectOffset, sizeof(void*)))
        return nullptr;
    void* pCollision = At<void*>(apNode, kCollisionObjectOffset);
    if (!pCollision || !IsReadable(pCollision, 0x28))
        return nullptr;
    void* pBody = At<void*>(pCollision, kCollisionBodyOffset);
    if (!pBody || !IsReadable(pBody, 0x18))
        return nullptr;
    auto* pHavok = At<uint8_t*>(pBody, kHavokBodyOffset);
    if (!pHavok || !IsReadable(pHavok, aBodyBytes))
        return nullptr;
    return pHavok;
}

// The same for a node known to be alive, with no IsReadable. A live node holds its collision object, which holds its body.
inline uint8_t* HavokBodyOfLive(void* apNode) noexcept
{
    void* pCollision = apNode ? At<void*>(apNode, kCollisionObjectOffset) : nullptr;
    void* pBody = pCollision ? At<void*>(pCollision, kCollisionBodyOffset) : nullptr;
    return pBody ? At<uint8_t*>(pBody, kHavokBodyOffset) : nullptr;
}

inline float HavokWorldScale() noexcept
{
    POINTER_SKYRIMSE(float, s_havokScale, kHavokWorldScaleAeId, 231896);
    return s_havokScale.Get() ? *s_havokScale.Get() : 0.f;
}

// The bhkWorld that owns a Havok world, checked by its RTTI name because the caller takes its lock.
inline void* BhkWorldOf(void* apHkpWorld) noexcept
{
    if (!apHkpWorld || !IsReadable(static_cast<uint8_t*>(apHkpWorld) + kBhkWorldOfHkpWorldOffset, sizeof(void*)))
        return nullptr;
    void* pWorld = At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset);
    const char* pName = RttiNameOf(pWorld);
    if (!pName || std::strncmp(pName, "bhkWorld", 8) != 0 || !IsReadable(static_cast<uint8_t*>(pWorld) + kWorldLockOffset, 8))
        return nullptr;
    return pWorld;
}

class WorldWriteLock
{
public:
    explicit WorldWriteLock(void* apBhkWorld) noexcept
    {
        POINTER_SKYRIMSE(TWorldLock, s_lockForWrite, kLockForWriteAeId, 66977);
        if (apBhkWorld && s_lockForWrite.Get())
        {
            m_pLock = static_cast<uint8_t*>(apBhkWorld) + kWorldLockOffset;
            s_lockForWrite.Get()(m_pLock);
        }
    }
    ~WorldWriteLock() noexcept
    {
        POINTER_SKYRIMSE(TWorldLock, s_unlockForWrite, kUnlockForWriteAeId, 66983);
        if (m_pLock && s_unlockForWrite.Get())
            s_unlockForWrite.Get()(m_pLock);
    }
    WorldWriteLock(const WorldWriteLock&) = delete;
    WorldWriteLock& operator=(const WorldWriteLock&) = delete;
    [[nodiscard]] bool Held() const noexcept { return m_pLock != nullptr; }

private:
    void* m_pLock = nullptr;
};

} // namespace HavokLayout
