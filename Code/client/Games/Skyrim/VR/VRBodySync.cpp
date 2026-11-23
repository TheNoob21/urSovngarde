#include <TiltedOnlinePCH.h>
#include <Utils.h>
#include <Services/CopyWorkGate.h>
#include <Services/GameExtension.h>
#include <Services/GrabbedBody.h>
#include <Services/RagdollPoseService.h>
#include <Services/PosedHands.h>

#include <Games/Skyrim/VR/VRBodySync.h>
#include <Games/Skyrim/HavokLayout.h>
#include <Games/Skyrim/WeaponContactRules.h>
#include <Games/Skyrim/SpellAimRules.h>
#include <Games/Skyrim/VR/PoseKeepDiag.h>
#include <Games/Skyrim/VR/PosedCopyPlacement.h>
#include <Games/Skyrim/VR/VRHaptics.h>
#include <Games/Skyrim/HeldWeaponPlacement.h>
#include <Games/Skyrim/VR/VRControllers.h>
#include <Games/Skyrim/GameSound.h>
#include <Games/Skyrim/CopyBodyRules.h>
#include <Games/Skyrim/CarriedBodyFollow.h>
#include <Games/Memory.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Systems/BodySolver.h>
#include <Systems/BodyPoseOrder.h>
#include <Systems/BodyPoseValidation.h>
#include <Systems/BodyPoseBounds.h>
#include <Copies/CopyRegistry.h>
#include <Copies/FallStop.h>
#include <Copies/FrameStartSwitchOff.h>

#include <Actor.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <BSAnimationGraphManager.h>
#include <Components/BGSBipedObjectForm.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESObjectARMO.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>
#include <NetImmerse/NiGlm.h>
#include <NetImmerse/NiLayout.h>

#include <glm/gtc/quaternion.hpp>

#include <Helpers/PerfScope.h>
#include <Helpers/ReadableMemory.h>
#include <Helpers/FiniteVector.h>
#ifndef SKYRIMVR
#include <Camera/CameraView.h>
#include <Games/Skyrim/SEBodyPresentation.h>
#endif

#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <tlhelp32.h>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <vector>

// NetImmerse objects are read through raw offsets (NiLayout).
namespace
{
using namespace HavokLayout;
using NiLayout::kChildCountOffset;
using NiLayout::kChildrenOffset;
using NiLayout::kChildSlotsOffset;
using NiLayout::kLocalOffset;
using NiLayout::kNameOffset;
using NiLayout::kNiAVObjectSize;
using NiLayout::kParentOffset;
using NiLayout::kWorldOffset;
using NiTree::ChildSlots;
using NiTree::ChildSlotsOf;
using NiTree::ForEachChild;
using NiTree::VisitBelow;
using NiTree::VisitTree;
// NiAVObject::worldBound, a centre and a radius. On VR it is at 0xE4: 0xB0, where SE keeps it, is previousWorld.
// Read only through ReadWorldBound, which refuses anything not the size and place of a weapon.
constexpr uint32_t kWorldBoundOffset = 0xE4;

// BSFlattenedBoneTree: the skeleton keeps a flat array of every bone; flattened-away bones (fingers, face) only exist there.
#ifdef SKYRIMVR
// As measured on SkyrimVR 1.4.15 by TiltedEvolutionVR.
constexpr uint32_t kTreeBoneArray = 0x158;
#else
// As measured on 1.7.104, where the count sits just before the array.
constexpr uint32_t kTreeBoneArray = 0x130;
constexpr uint32_t kTreeBoneCount = 0x128;
#endif
constexpr uint32_t kBoneEntrySize = 0x80;
constexpr uint32_t kBoneEntryLocal = 0x00;
constexpr uint32_t kBoneEntryWorld = 0x34;
constexpr uint32_t kBoneEntryIndices = 0x68; // four int16, one of them the parent index
constexpr uint32_t kBoneEntryNode = 0x70;    // the bone's node, null when it was flattened away
constexpr uint32_t kMaxBones = 1024;

// PlayerCharacter's headset node (same measurement), checked by name before use.
constexpr uint32_t kHmdNodeOffset = 0x570;

constexpr std::array<const char*, VRPose::kBoneCount> kBoneNames{
    "NPC Spine1 [Spn1]",   "NPC Spine2 [Spn2]",   "NPC Neck [Neck]",       "NPC Head [Head]",
    "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
    "NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]",
    // Lower body (VRPose::HasLegs). The pelvis hangs off NPC COM, beside the spine, so it is searched from the root.
    "NPC Pelvis [Pelv]",   "NPC L Thigh [LThg]",  "NPC L Calf [LClf]",     "NPC L Foot [Lft ]",
    "NPC R Thigh [RThg]",  "NPC R Calf [RClf]",   "NPC R Foot [Rft ]",
};

// Parent bone index for each entry of kBoneNames (-1: searched under "NPC Root [Root]").
constexpr std::array<int8_t, VRPose::kBoneCount> kBoneParents{-1, 0, 1, 2, 1, 4, 5, 6, 1, 8, 9, 10, -1, 12, 13, 14, 12, 16, 17};

// Where a weapon hangs, left then right (VRPose::HasWeapons).
// Both are searched below the forearm, because the shield node hangs off its twist bone, not the hand.
constexpr std::array<const char*, 2> kAttachNames{"SHIELD", "WEAPON"};
constexpr std::array<const char*, 2> kHandNames{"NPC L Hand [LHnd]", "NPC R Hand [RHnd]"};

// PlayerCharacter's first-person hands, left then right: VR_NODE_DATA::NPCLHnd and NPCRHnd in CommonLibVR-NG.
// Not measured here, so FirstPersonHand checks each by name before use.
constexpr std::array<uint32_t, 2> kFirstPersonHandOffsets{0x590, 0x598};

using BoneNodes = std::array<void*, VRPose::kBoneCount>;

const char* GetName(void* apObject) noexcept
{
    return At<const char*>(apObject, kNameOffset);
}

/**
 * Find the node one entry of a raw bone array points to.
 *
 * @param apEntry The bone's entry, as the game keeps it.
 * @return The bone's node, or null when the bone was flattened away.
 */
void* NodeOfEntry(const uint8_t* apEntry) noexcept
{
    void* pNode = nullptr;
    std::memcpy(&pNode, apEntry + kBoneEntryNode, sizeof(pNode));
    return pNode;
}

/**
 * Find the shallowest node below a start node with this name, breadth first.
 * Physics armour (SMP/3BA) carries its own copies of skeleton nodes like "NPC L Hand", deeper down.
 *
 * @param apStart Where the search starts; the start itself is not a match.
 * @param acpName The name, in any case.
 * @return The node, or null when none below has that name.
 */
void* FindShallowest(void* apStart, const char* acpName) noexcept
{
    void* pFound = nullptr;
    VisitBelow(apStart, 8192, [&](void* pObject) {
        const char* pName = GetName(pObject);
        if (!pName || _stricmp(pName, acpName) != 0)
            return true;
        pFound = pObject;
        return false;
    });
    return pFound;
}

/**
 * Find the first object of a game class in a tree, depth first, at most eight levels down.
 *
 * @param apStart Where the search starts; the start itself can match.
 * @param acpRttiName The class's name as the game's type information gives it.
 * @param aDepth How deep the search already is.
 * @return The object, or null when none has that class.
 */
void* FindByRtti(void* apStart, const char* acpRttiName, uint32_t aDepth = 0) noexcept
{
    if (!apStart || aDepth > 8)
        return nullptr;

    using TGetRtti = const char**(__fastcall*)(void*);
    auto** ppVTable = *static_cast<void***>(apStart);
    const char** ppRtti = static_cast<TGetRtti>(ppVTable[kVTableGetRttiSlot])(apStart);
    if (ppRtti && ppRtti[0] && std::strcmp(ppRtti[0], acpRttiName) == 0)
        return apStart;

    void* pNode = AsNode(apStart);
    if (!pNode)
        return nullptr;

    void* pFound = nullptr;
    ForEachChild(pNode, [&](void* pChild) {
        pFound = FindByRtti(pChild, acpRttiName, aDepth + 1);
        return pFound == nullptr;
    });
    return pFound;
}

// The skeleton root below the actor's 3D: every bone hangs off it, and its world scale is the body's size.
void* FindSkeletonRoot(void* apRoot) noexcept
{
    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    return pSkeletonRoot ? pSkeletonRoot : apRoot;
}

// The upper body is required; the legs are optional (a skeleton without them is posed above the waist only).
bool FindBones(void* apRoot, BoneNodes& aNodes, bool* apLegsFound = nullptr) noexcept
{
    aNodes.fill(nullptr);

    void* pSkeletonRoot = FindShallowest(apRoot, "NPC Root [Root]");
    if (!pSkeletonRoot)
        pSkeletonRoot = apRoot;

    bool legsFound = true;
    // Each bone is searched inside the bone found for its parent, so only the real chain matches.
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
    {
        void* pSearchFrom = kBoneParents[i] < 0 ? pSkeletonRoot : aNodes[kBoneParents[i]];
        aNodes[i] = pSearchFrom ? FindShallowest(pSearchFrom, kBoneNames[i]) : nullptr;
        if (aNodes[i])
            continue;
        if (i < VRPose::kUpperBoneCount)
            return false;
        legsFound = false;
    }
    if (apLegsFound)
        *apLegsFound = legsFound;
    return true;
}

// Whether a full body tracking plugin is loaded (SkyrimVR FBT, Nexus 185070, or any module named like "fbt"/"fullbody").
// Legs are sent only then: otherwise they follow the walk animation on both sides anyway.
// Checked every 5 s.
bool FullBodyTrackingActive() noexcept
{
    static std::chrono::steady_clock::time_point s_nextCheck;
    static bool s_active = false;
    static std::string s_module;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_nextCheck)
        return s_active;
    s_nextCheck = now + std::chrono::seconds(5);

    bool active = false;
    std::string matched;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Module32FirstW(hSnapshot, &entry); ok && !active; ok = Module32NextW(hSnapshot, &entry))
        {
            std::string name;
            for (const wchar_t* p = entry.szModule; *p; ++p)
                name += *p < 128 ? static_cast<char>(std::towlower(*p)) : '?';
            if (name.rfind("skyrimtogether", 0) == 0)
                continue;
            if (name.find("fbt") != std::string::npos || name.find("fullbody") != std::string::npos)
            {
                active = true;
                matched = name;
            }
        }
        CloseHandle(hSnapshot);
    }

    if (active != s_active)
    {
        if (active)
            spdlog::info("VRBodySync: full body tracking plugin '{}' is loaded, the legs are sent with the pose", matched);
        else
            spdlog::info("VRBodySync: full body tracking plugin '{}' is gone, the legs are no longer sent", s_module);
    }
    s_active = active;
    if (active)
        s_module = matched;
    return active;
}

// Defined further down, with the rig.
uint8_t* GetBoneArray(void* apTree, uint32_t& aOutCount) noexcept;
uint8_t* FindBoneEntry(uint8_t* apArray, uint32_t aCount, void* apNode) noexcept;

// The flattened bone array of a skeleton and which of an entry's four indices is its parent.
struct BoneArrayInfo
{
    uint8_t* pArray = nullptr;
    uint32_t Count = 0;
    int ParentField = -1;
};

int16_t ReadEntryIndex(const uint8_t* apEntry, int aField) noexcept
{
    return *reinterpret_cast<const int16_t*>(apEntry + kBoneEntryIndices + aField * sizeof(int16_t));
}

// Whether following one index field up from an entry reaches an index within that many hops.
bool LeadsUpTo(const BoneArrayInfo& acInfo, const uint8_t* apEntry, const int16_t aAncestor, const int aField, const int aHops) noexcept
{
    for (int hop = 0; hop < aHops; ++hop)
    {
        const int16_t cParent = ReadEntryIndex(apEntry, aField);
        if (cParent == aAncestor)
            return true;
        if (cParent < 0 || static_cast<uint32_t>(cParent) >= acInfo.Count)
            return false;
        apEntry = acInfo.pArray + cParent * kBoneEntrySize;
    }
    return false;
}

BoneArrayInfo ResolveBoneArray(void* apRoot, const BoneNodes& acNodes, bool aLog) noexcept
{
    BoneArrayInfo info;
    info.pArray = GetBoneArray(FindByRtti(apRoot, "BSFlattenedBoneTree"), info.Count);

    // The parent index is the one that maps the left forearm to the upper arm and the hand to the forearm.
    // Some skeletons put twist bones between them, so a longer way up is tried only when no field does it directly.
    const uint8_t* pUpper = FindBoneEntry(info.pArray, info.Count, acNodes[VRPose::kLeftUpperArm]);
    const uint8_t* pFore = FindBoneEntry(info.pArray, info.Count, acNodes[VRPose::kLeftForearm]);
    const uint8_t* pHand = FindBoneEntry(info.pArray, info.Count, acNodes[VRPose::kLeftHand]);
    if (pUpper && pFore && pHand)
    {
        const auto indexOf = [&info](const uint8_t* apEntry) { return static_cast<int16_t>((apEntry - info.pArray) / kBoneEntrySize); };
        for (int hops = 1; hops <= 3 && info.ParentField < 0; ++hops)
            for (int field = 0; field < 4 && info.ParentField < 0; ++field)
                if (LeadsUpTo(info, pFore, indexOf(pUpper), field, hops) && LeadsUpTo(info, pHand, indexOf(pFore), field, hops))
                    info.ParentField = field;
    }

    if (!info.pArray || info.ParentField < 0)
    {
        // Nodes alone still pose the body and anything attached; flattened bones (fingers, face) then lag behind.
        if (aLog)
            spdlog::warn("VRBodySync: no usable flattened bone array under root {} (count {}, parent field {})", apRoot, info.Count, info.ParentField);
        info = BoneArrayInfo{};
    }
    return info;
}

// The finger bones of one hand: five chains of three flattened entries, thumb first.
// Flattened entries have no name, so they are found by shape: an entry under the hand with a child that has a child.
using FingerEntries = std::array<uint8_t*, VRPose::kFingersPerHand * VRPose::kBonesPerFinger>;

bool FindFingers(const BoneArrayInfo& acInfo, const uint8_t* apHandEntry, FingerEntries& aOut) noexcept
{
    aOut.fill(nullptr);
    if (!acInfo.pArray || !apHandEntry || acInfo.ParentField < 0)
        return false;
    const auto entryAt = [&acInfo](uint32_t aIndex) { return acInfo.pArray + aIndex * kBoneEntrySize; };
    const auto parentOf = [&](uint32_t aIndex) { return ReadEntryIndex(entryAt(aIndex), acInfo.ParentField); };
    const auto firstChildOf = [&](int16_t aParent) -> int16_t
    {
        for (uint32_t i = 0; i < acInfo.Count; ++i)
            if (parentOf(i) == aParent)
                return static_cast<int16_t>(i);
        return -1;
    };
    const int16_t handIndex = static_cast<int16_t>((apHandEntry - acInfo.pArray) / kBoneEntrySize);

    uint32_t found = 0;
    for (uint32_t i = 0; i < acInfo.Count; ++i)
    {
        if (parentOf(i) != handIndex)
            continue;
        const int16_t second = firstChildOf(static_cast<int16_t>(i));
        const int16_t third = second >= 0 ? firstChildOf(second) : -1;
        if (second < 0 || third < 0)
            continue;
        if (found == VRPose::kFingersPerHand)
        {
            found = 0; // a sixth chain: not the shape expected, give up rather than guess
            break;
        }
        aOut[found * VRPose::kBonesPerFinger + 0] = entryAt(i);
        aOut[found * VRPose::kBonesPerFinger + 1] = entryAt(static_cast<uint32_t>(second));
        aOut[found * VRPose::kBonesPerFinger + 2] = entryAt(static_cast<uint32_t>(third));
        ++found;
    }
    if (found != VRPose::kFingersPerHand)
    {
        aOut.fill(nullptr);
        return false;
    }
    return true;
}

// Finger rotations relative to the parent bone, read from the rendered (world) transforms.
// The first bone of each finger is relative to the hand, the others to the previous entry.
void ReadFingerRotations(const FingerEntries& acFingers, const glm::mat3& acHandWorld, glm::quat* apOut) noexcept
{
    for (size_t f = 0; f < VRPose::kFingersPerHand; ++f)
    {
        glm::mat3 parent = acHandWorld;
        for (size_t k = 0; k < VRPose::kBonesPerFinger; ++k)
        {
            const uint8_t* pEntry = acFingers[f * VRPose::kBonesPerFinger + k];
            const glm::mat3 world = ToGlm(reinterpret_cast<const NiTransform*>(pEntry + kBoneEntryWorld)->rotate);
            apOut[f * VRPose::kBonesPerFinger + k] = glm::normalize(glm::quat_cast(glm::transpose(parent) * world));
            parent = world;
        }
    }
}

// Writes finger rotations below an already posed hand.
// Each entry's world follows its parent, keeping the entry's own local offset and world scale.
void WriteFingerRotations(const FingerEntries& acFingers, const NiTransform& acHandWorld, const glm::quat* apRotations) noexcept
{
    for (size_t f = 0; f < VRPose::kFingersPerHand; ++f)
    {
        NiTransform parent = acHandWorld;
        for (size_t k = 0; k < VRPose::kBonesPerFinger; ++k)
        {
            uint8_t* pEntry = acFingers[f * VRPose::kBonesPerFinger + k];
            NiTransform& local = *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryLocal);
            NiTransform& world = *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld);
            const glm::mat3 parentRotation = ToGlm(parent.rotate);
            const glm::mat3 localRotation = glm::mat3_cast(apRotations[f * VRPose::kBonesPerFinger + k]);
            local.rotate = FromGlm(localRotation);
            world.rotate = FromGlm(parentRotation * localRotation);
            world.translate = FromGlm(ToGlm(parent.translate) + parentRotation * (ToGlm(local.translate) * parent.scale));
            parent = world;
        }
    }
}

struct NativeRemotePose
{
    // The whole pose is a position and nothing else (see VRPose::NoBones): a skeleton the bone search cannot read.
    bool NoBones = false;
    bool HasLegs = false;
    std::array<glm::quat, VRPose::kBoneCount> Bones{};
    // Kept from the last update that carried them (see VRPose::HasFingers).
    bool HasFingers = false;
    std::array<glm::quat, VRPose::kFingerBoneCount> Fingers{};
    // Kept from the last update that carried it (see VRPose::HasScale).
    bool HasScale = false;
    float RootScale = 1.f;
    // Only for a dead body being moved (see VRPose::HasRootPosition).
    // Not kept, so when the sender stops the body goes back to its own ragdoll.
    bool HasRootPosition = false;
    glm::vec3 RootPosition{};
    // Where the sender's hips are relative to its root (see VRPose::HasHips).
    // Not kept, so when the sender's trackers stop driving the legs the body goes back to its own animation.
    bool HasHips = false;
    glm::vec3 HipOffset{};
    // The owner's own hand positions relative to its root (see VRPose::HasHandCheck).
    // ReachHands puts the copy's hands there, and the hand measurement compares against them.
    bool HasHandCheck = false;
    glm::vec3 HandOffsets[2]{};
    // Where the owner's weapons are held relative to its hands, left then right (see VRPose::HasWeapons).
    // Kept from the last update that carried them; PlaceWeapons puts the copy's weapons there.
    bool HasWeapons = false;
    std::array<bool, 2> WeaponHeld{};
    std::array<glm::quat, 2> WeaponRotation{};
    std::array<glm::vec3, 2> WeaponOffset{};
    // Where the owner's held spells go (see VRPose::HasSpellAim).
    // Not kept, so a copy's spell goes back to following its body when the owner stops sending.
    bool HasSpellAim = false;
    float SpellAim[2]{};
    // A held interpolation sample cannot extend this deadline without a newer source tick.
    uint64_t Generation = 0;
    uint64_t SourceTick = 0;
    double ReceivedAtMs = 0.0;
    bool HasProvenance = false;
    bool Live = false;
    std::chrono::steady_clock::time_point Deadline{};

    bool Available(std::chrono::steady_clock::time_point aNow) const noexcept { return !Live || aNow < Deadline; }
};

std::shared_mutex s_posesLock;
TiltedPhoques::Map<uint32_t, NativeRemotePose> s_poses;

bool SparseBodyEligible(Actor* apActor) noexcept
{
    const auto* extension = apActor ? apActor->GetExtension() : nullptr;
    return extension && ::RemotePose::SparseBodyEligible(
                            apActor->actorState.IsDead(), apActor->actorState.IsInKnockRagdoll(), apActor->actorState.IsBleedingOut(),
                            extension->IsRemotePlayer() || extension->IsLocalPlayer());
}
#ifndef SKYRIMVR
// The flat game keeps only where a copy's held spell goes, apart from s_poses: an entry there would be posed (OnFrameEnd) with no bones.
TiltedPhoques::Map<uint32_t, std::array<float, 2>> s_spellAims;
#endif

/**
 * Sum up the direct children of one node: how many slots the array holds (high 16 bits) and how many are filled (low 16 bits).
 * Plain reads with no virtual call and no VirtualQuery, so it is cheap enough to run every frame.
 * It notices something attached below a bone, which the cached descendant list cannot see.
 *
 * @param apChildOwner The node; null gives 0.
 * @return The slots and filled count; only the slot count when the node has no array.
 */
uint32_t ChildFingerprintOf(void* apChildOwner) noexcept
{
    if (!apChildOwner)
        return 0;

    const ChildSlots cChildren = ChildSlotsOf(apChildOwner);
    if (!cChildren.pSlots)
        return cChildren.Count;

    uint32_t filled = 0;
    for (uint16_t i = 0; i < cChildren.Count; ++i)
        filled += cChildren.pSlots[i] != nullptr;

    return (static_cast<uint32_t>(cChildren.Count) << 16) | filled;
}

/**
 * A bone as the renderer sees it: its node, its slot in the flattened bone array, or both.
 *
 * NodeVTable and EntryNode record what the pointers pointed at when the skeleton was resolved.
 * Skyrim gives freed node memory straight to new objects, so a rebuilt 3D can reuse the same addresses.
 * When either changes, the skeleton is gone and nothing may be written through these pointers.
 */
struct RigBone
{
    void* pNode = nullptr;
    uint8_t* pEntry = nullptr;
    void* NodeVTable = nullptr;
    void* EntryNode = nullptr;
    // The node under which children hang, and what hung there when the skeleton was resolved.
    void* pChildOwner = nullptr;
    uint32_t ChildFingerprint = 0;

    [[nodiscard]] bool IsIntact() const noexcept
    {
        if (pNode && *static_cast<void**>(pNode) != NodeVTable)
            return false;
        return !pEntry || NodeOfEntry(pEntry) == EntryNode;
    }

    [[nodiscard]] bool ChildrenUnchanged() const noexcept
    {
        return ChildFingerprintOf(pChildOwner) == ChildFingerprint;
    }

    [[nodiscard]] const NiTransform& World() const noexcept
    {
        return pNode ? At<NiTransform>(pNode, kWorldOffset) : *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld);
    }

    void WriteWorld(const NiTransform& acWorld) const noexcept
    {
        if (pNode)
            At<NiTransform>(pNode, kWorldOffset) = acWorld;
        if (pEntry)
            *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryWorld) = acWorld;
    }

    void WriteLocal(const NiTransform& acLocal) const noexcept
    {
        if (pNode)
            At<NiTransform>(pNode, kLocalOffset) = acLocal;
        if (pEntry)
            *reinterpret_cast<NiTransform*>(pEntry + kBoneEntryLocal) = acLocal;
    }
};

RigBone MakeRigBone(void* apNode, uint8_t* apEntry) noexcept
{
    RigBone bone{apNode, apEntry};
    if (apNode)
    {
        bone.NodeVTable = *static_cast<void**>(apNode);
        bone.pChildOwner = AsNode(apNode);
        bone.ChildFingerprint = ChildFingerprintOf(bone.pChildOwner);
    }
    if (apEntry)
        bone.EntryNode = NodeOfEntry(apEntry);
    return bone;
}

// The two-sided finger arrays (Fingers, FingersFound) hold the left hand first, then the right.
// Not VRPose::kLeftHand and kRightHand, which are the hand bones' places in the skeleton.
constexpr size_t kLeftSide = 0;
constexpr size_t kRightSide = 1;

/**
 * The skeleton of one remote player, resolved once per 3D.
 */
struct Rig
{
    void* pRoot = nullptr;
    std::array<RigBone, VRPose::kBoneCount> Bones{};
    // Everything below each posed bone (nodes and flattened bones), the posed bones further down the chain included.
    std::array<std::vector<RigBone>, VRPose::kBoneCount> Descendants{};
    std::array<uint8_t, VRPose::kBoneCount> PoseOrder{};
    size_t PoseOrderCount = 0;
    // Every node and flattened bone of the skeleton, each once, for the whole-body offset in PoseActor.
    // Empty when the skeleton root was not found.
    std::vector<RigBone> Body;
    // The weapon attach nodes ("SHIELD" left, "WEAPON" right) and everything below them, for PlaceWeapons.
    // Empty on a side that has none.
    std::array<RigBone, 2> Attach{};
    std::array<std::vector<RigBone>, 2> AttachBelow{};
    // Finger entries below each hand (left, right); found only when the flattened array is usable.
    std::array<FingerEntries, 2> Fingers{};
    std::array<bool, 2> FingersFound{};
    // "NPC Root [Root]": its local scale is set to the sender's body size.
    RigBone SkeletonRoot{};
    std::chrono::steady_clock::time_point RetryAt{};
    // Re-resolving after an attach is rate limited.
    // Otherwise an effect that attaches every frame would rebuild the skeleton every frame.
    std::chrono::steady_clock::time_point RestructureAt{};
    bool Valid = false;
    // Whether it was in view at the last frame end: once in view, it leaves only well past the cone's edge.
    bool InView = false;
    // TEMPORARY (PoseKeepDiag): where a frame end last left the pelvis, and which frame end that was.
    glm::vec3 PelvisLeft{};
    uint64_t PelvisLeftAt = 0;
};

std::unordered_map<uint32_t, Rig> s_rigs; // frame end only

/**
 * Find the node one bone of a raw bone array points to.
 *
 * @param apArray The bone array, as the game keeps it.
 * @param aIndex Which bone, counted from 0.
 * @return The bone's node, or null when the bone has none.
 */
void* GetBoneNode(const uint8_t* apArray, uint32_t aIndex) noexcept
{
    return NodeOfEntry(apArray + static_cast<size_t>(aIndex) * kBoneEntrySize);
}

/**
 * Check if a bone array really holds as many bones as its count says, before reading through it.
 * The count must be between 1 and kMaxBones, the whole array readable, and every bone's node readable or absent.
 *
 * @param apArray The bone array, as the game keeps it.
 * @param aCount How many bones the game says it holds.
 * @return True when the array can be read safely with that count.
 */
bool IsPlausibleBoneCount(const uint8_t* apArray, uint32_t aCount) noexcept
{
    if (aCount == 0 || aCount > kMaxBones)
        return false;

    const size_t arraySize = static_cast<size_t>(aCount) * kBoneEntrySize;
    if (!ReadableMemory::IsReadable(apArray, arraySize))
        return false;

    for (uint32_t i = 0; i < aCount; ++i)
    {
        void* pNode = GetBoneNode(apArray, i);
        if (pNode && !ReadableMemory::IsReadable(pNode, kNiAVObjectSize))
            return false;
    }
    return true;
}

uint8_t* GetBoneArray(void* apTree, uint32_t& aOutCount) noexcept
{
    ReadableMemory::CacheScope regions; // a few hundred readability questions, almost all about the same few regions
    aOutCount = 0;
    if (!apTree || !ReadableMemory::IsReadable(static_cast<uint8_t*>(apTree) + kTreeBoneArray, sizeof(void*)))
        return nullptr;

    auto* pArray = At<uint8_t*>(apTree, kTreeBoneArray);
    if (!ReadableMemory::IsReadable(pArray, kBoneEntrySize))
        return nullptr;

#ifdef SKYRIMVR
    // The count sits near the array pointer at an offset that is not measured.
    for (uint32_t offset = kChildrenOffset + 0x18; offset <= kTreeBoneArray + 0x20; offset += sizeof(uint32_t))
    {
        const uint32_t candidate = At<uint32_t>(apTree, offset);
        if (IsPlausibleBoneCount(pArray, candidate))
        {
            aOutCount = candidate;
            return pArray;
        }
    }
    return nullptr;
#else
    const uint32_t count = At<uint32_t>(apTree, kTreeBoneCount);
    if (!IsPlausibleBoneCount(pArray, count))
        return nullptr;
    aOutCount = count;
    return pArray;
#endif
}

uint8_t* FindBoneEntry(uint8_t* apArray, uint32_t aCount, void* apNode) noexcept
{
    for (uint32_t i = 0; apArray && i < aCount; ++i)
        if (GetBoneNode(apArray, i) == apNode)
            return apArray + i * kBoneEntrySize;
    return nullptr;
}

/**
 * List every object below a node, depth first: each child, then everything below it, before the next child.
 *
 * @param apNode The node whose descendants are listed; not listed itself.
 * @param aOut Receives the descendants.
 * @param aDepth How deep the walk already is; it stops 64 levels down.
 */
void CollectNodeDescendants(void* apNode, std::vector<void*>& aOut, uint32_t aDepth = 0) noexcept
{
    void* pNode = aDepth < 64 ? AsNode(apNode) : nullptr;
    if (!pNode)
        return;

    ForEachChild(pNode, [&](void* pChild) {
        aOut.push_back(pChild);
        CollectNodeDescendants(pChild, aOut, aDepth + 1);
    });
}

bool ResolveRig(void* apRoot, Rig& aRig, bool aLog = true) noexcept
{
    aRig = Rig{};
    aRig.pRoot = apRoot;
    aRig.SkeletonRoot = MakeRigBone(FindSkeletonRoot(apRoot), nullptr);

    BoneNodes nodes;
    if (!FindBones(apRoot, nodes))
        return false;

    const BoneArrayInfo arrayInfo = ResolveBoneArray(apRoot, nodes, aLog);
    uint8_t* pArray = arrayInfo.pArray;
    const uint32_t count = arrayInfo.Count;
    const int parentField = arrayInfo.ParentField;
    const auto readIndex = [](const uint8_t* apEntry, int aField) { return ReadEntryIndex(apEntry, aField); };

    std::unordered_map<void*, uint8_t*> entryByNode;
    for (uint32_t i = 0; i < count; ++i)
        if (void* pNode = GetBoneNode(pArray, i))
            entryByNode.emplace(pNode, pArray + i * kBoneEntrySize);

    const auto entryFor = [&entryByNode](void* apNode) -> uint8_t*
    {
        const auto it = entryByNode.find(apNode);
        return it == entryByNode.end() ? nullptr : it->second;
    };

    for (uint32_t bone = 0; bone < VRPose::kBoneCount; ++bone)
    {
        aRig.Bones[bone] = MakeRigBone(nodes[bone], entryFor(nodes[bone]));
        if (!nodes[bone])
            continue; // a leg bone this skeleton lacks; never posed

        // Nodes below the bone: weapon, shield, magic node and the next bones.
        std::vector<RigBone>& descendants = aRig.Descendants[bone];
        std::unordered_map<const uint8_t*, bool> entriesSeen;
        std::vector<void*> childNodes;
        CollectNodeDescendants(nodes[bone], childNodes);
        for (void* pChild : childNodes)
        {
            uint8_t* pEntry = entryFor(pChild);
            descendants.push_back(MakeRigBone(pChild, pEntry));
            if (pEntry)
                entriesSeen[pEntry] = true;
        }

        // Flattened bones below it (fingers, facial bones), found through each entry's parent chain.
        const uint8_t* pBoneEntry = aRig.Bones[bone].pEntry;
        for (uint32_t i = 0; pBoneEntry && i < count; ++i)
        {
            uint8_t* pEntry = pArray + i * kBoneEntrySize;
            if (pEntry == pBoneEntry || entriesSeen.count(pEntry))
                continue;

            int16_t walk = static_cast<int16_t>(i);
            for (uint32_t step = 0; step < 128; ++step)
            {
                walk = readIndex(pArray + static_cast<uint32_t>(walk) * kBoneEntrySize, parentField);
                if (walk < 0 || static_cast<uint32_t>(walk) >= count)
                    break;
                if (pArray + static_cast<uint32_t>(walk) * kBoneEntrySize == pBoneEntry)
                {
                    descendants.push_back(MakeRigBone(NodeOfEntry(pEntry), pEntry));
                    break;
                }
            }
        }
    }

    // Derive order from this skeleton, not the wire enumeration: some rigs put the pelvis above the spine.
    BodyPoseOrder::Graph<VRPose::kBoneCount> order;
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        order.Bones[i] = {aRig.Bones[i].pNode, aRig.Bones[i].pEntry};
    for (size_t i = 0; i < VRPose::kBoneCount; ++i)
        for (const RigBone& child : aRig.Descendants[i])
            if (!order.AddDescendant(i, {child.pNode, child.pEntry}))
            {
                if (aLog)
                    spdlog::warn("VRBodySync: ambiguous descendant in rig {}; leaving it unposed", apRoot);
                return false;
            }
    if (!order.Sort(aRig.PoseOrder, aRig.PoseOrderCount))
    {
        if (aLog)
            spdlog::warn("VRBodySync: cyclic or aliased posed bones in rig {}; leaving it unposed", apRoot);
        return false;
    }

    // The whole skeleton, for the whole-body offset.
    // The posed bones miss NPC COM and NPC Spine [Spn0], and the waist is skinned to Spn0.
    if (void* pSkeleton = aRig.SkeletonRoot.pNode)
    {
        std::unordered_set<const uint8_t*> bodyEntries;
        std::vector<void*> bodyNodes{pSkeleton};
        CollectNodeDescendants(pSkeleton, bodyNodes);
        for (void* pNode : bodyNodes)
        {
            uint8_t* pEntry = entryFor(pNode);
            aRig.Body.push_back(MakeRigBone(pNode, pEntry));
            if (pEntry)
                bodyEntries.insert(pEntry);
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t* pEntry = pArray + i * kBoneEntrySize;
            if (!bodyEntries.count(pEntry))
                aRig.Body.push_back(MakeRigBone(nullptr, pEntry));
        }
    }

    // The weapon attach nodes, by name below each forearm: "SHIELD" hangs off the forearm's twist bone, not the hand.
    for (size_t side = 0; side < 2; ++side)
    {
        void* pForearm = nodes[side == 0 ? VRPose::kLeftForearm : VRPose::kRightForearm];
        void* pAttach = pForearm ? FindShallowest(pForearm, kAttachNames[side]) : nullptr;
        if (!pAttach || !AsNode(pAttach))
            continue;
        aRig.Attach[side] = MakeRigBone(pAttach, entryFor(pAttach));
        std::vector<void*> below;
        CollectNodeDescendants(pAttach, below);
        for (void* pNode : below)
            aRig.AttachBelow[side].push_back(MakeRigBone(pNode, entryFor(pNode)));
    }

    aRig.FingersFound[kLeftSide] = FindFingers(arrayInfo, aRig.Bones[VRPose::kLeftHand].pEntry, aRig.Fingers[kLeftSide]);
    aRig.FingersFound[kRightSide] = FindFingers(arrayInfo, aRig.Bones[VRPose::kRightHand].pEntry, aRig.Fingers[kRightSide]);
    if (aLog && (!aRig.FingersFound[kLeftSide] || !aRig.FingersFound[kRightSide]))
        spdlog::debug(
            "VRBodySync: finger bones not found by shape under root {} (left {}, right {}); the hands stay on the animation pose", apRoot, aRig.FingersFound[kLeftSide],
            aRig.FingersFound[kRightSide]);
    aRig.Valid = true;
    return true;
}

[[nodiscard]] bool RigIntact(const Rig& acRig) noexcept
{
    if (!acRig.SkeletonRoot.IsIntact())
        return false;
    for (const auto& bone : acRig.Bones)
        if (!bone.IsIntact())
            return false;
    for (const auto& descendants : acRig.Descendants)
        for (const auto& bone : descendants)
            if (!bone.IsIntact())
                return false;
    for (const auto& bone : acRig.Body)
        if (!bone.IsIntact())
            return false;
    for (size_t side = 0; side < 2; ++side)
    {
        if (!acRig.Attach[side].IsIntact())
            return false;
        for (const auto& bone : acRig.AttachBelow[side])
            if (!bone.IsIntact())
                return false;
    }
    return true;
}

// The descendant lists are a snapshot taken when the skeleton was resolved, so anything attached later is missing.
// A readied spell's art, for one, hangs off the magic node and would float where the animation left it.
// False means something was attached and the skeleton should be resolved again.
// Only call this on an intact rig: it reads through the cached pointers.
[[nodiscard]] bool RigStructureUnchanged(const Rig& acRig) noexcept
{
    for (const auto& bone : acRig.Bones)
        if (!bone.ChildrenUnchanged())
            return false;
    for (const auto& descendants : acRig.Descendants)
        for (const auto& bone : descendants)
            if (!bone.ChildrenUnchanged())
                return false;
    // Rig::Body is not checked: an arrow or effect attached anywhere would cost a new search of up to 50 ms.
    // A node missing from Rig::Body only misses the whole-body offset.
    return true;
}

/**
 * Check if an actor is in front of the player's headset, so it can be posed.
 * Posing an actor the renderer has culled can tear its skin into black strips, so only actors in front of the headset are posed.
 * A sphere around the chest is tested against a 50 degree cone, widened by the angle the sphere covers.
 * An actor close by therefore counts as visible whatever the angle.
 *
 * @param acChest The actor's chest in the game's world.
 * @param aMarginDegrees How many degrees wider the cone is, for an actor already in view.
 * @return True when it is in view; always true without the headset's node, and false when the headset's numbers are broken.
 */
bool IsInView(const glm::vec3& acChest, const float aMarginDegrees = 0.f) noexcept
{
#ifndef SKYRIMVR
    // A flat screen has no headset: the game's camera decides, with a third of the screen around its edges for the rest of the body.
    return CameraView::ShowsBody(NiPoint3(acChest), 0.3f + aMarginDegrees / 90.f);
#else
    const float cHalfAngle = (50.f + aMarginDegrees) * glm::pi<float>() / 180.f;
    constexpr float cBodyRadius = 100.f;

    // Without the headset's node every actor is posed; with broken numbers in it, none counts as in view.
    if (!VRControllers::HeadsetTransform())
        return true;
    const auto cHead = VRControllers::HeadSpaceNow();
    if (!cHead)
        return false;
    const float distance = glm::distance(acChest, cHead->Position);
    if (distance <= cBodyRadius)
        return true;

    return cHead->AngleFromGaze(acChest) < cHalfAngle + std::asin(cBodyRadius / distance);
#endif
}

// Where a live copy's chest is drawn: its root, moved by the pose's hips as PoseActor moves it, at chest height.
// Its own bones cannot tell: they hold the game's animation or our last pose, depending on whether the game animated this frame.
// Tested at its bones, a copy near the edge of the view can be in view one frame and out the next, so posed and not posed in turn.
glm::vec3 DrawnChest(void* apRoot, const NativeRemotePose& acPose) noexcept
{
    constexpr float cChestHeight = 90.f; // spine2 of a standing human, above its root
    const NiTransform& rootWorld = At<NiTransform>(apRoot, kWorldOffset);
    glm::vec3 local{0.f, 0.f, cChestHeight};
    // A hip a body-length from the root is a bad read, as in OnFrameEnd.
    const glm::vec3& hips = acPose.HipOffset;
    if (acPose.HasHips && std::isfinite(hips.x) && std::isfinite(hips.y) && hips.x * hips.x + hips.y * hips.y < 256.f * 256.f)
    {
        local.x = hips.x;
        local.y = hips.y;
    }
    return ToGlm(rootWorld.translate) + ToGlm(rootWorld.rotate) * local;
}

// A transform the renderer can still use: a real rotation, a sane scale, no infinities.
// A skinned body with a bone that fails this is not drawn at all, while the actor stays present and can be hit.
bool IsRenderableTransform(const NiTransform& acTransform) noexcept
{
    const glm::mat3 rotation = ToGlm(acTransform.rotate);
    for (int column = 0; column < 3; ++column)
    {
        const float length = glm::length(rotation[column]);
        if (!std::isfinite(length) || length < 0.9f || length > 1.1f)
            return false;
    }
    if (!std::isfinite(acTransform.scale) || acTransform.scale < 0.05f || acTransform.scale > 20.f)
        return false;
    return IsFiniteVector(acTransform.translate);
}

// The bone IsBodyUsable found unusable, for its log line.
struct UnusableBone
{
    const char* Name = nullptr;
    NiTransform World{};
};

// Whether the root and the upper-body bones we write are still usable by the renderer.
// False means this body is, or is about to be, undrawable and must be left alone; aOut then names the first bone that is not.
bool IsBodyUsable(const Rig& acRig, UnusableBone& aOut) noexcept
{
    if (!acRig.pRoot)
        return false;
    const NiTransform& rootWorld = At<NiTransform>(acRig.pRoot, kWorldOffset);
    if (!IsRenderableTransform(rootWorld))
    {
        aOut = {"the root", rootWorld};
        return false;
    }
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        const RigBone& bone = acRig.Bones[i];
        if ((bone.pNode || bone.pEntry) && !IsRenderableTransform(bone.World()))
        {
            aOut = {kBoneNames[i], bone.World()};
            return false;
        }
    }
    return true;
}

// The top of the tree a node hangs from.
// Two bodies drawn in the same world share it; one left in a subtree the world no longer holds does not.
void* SceneTopOf(void* apNode) noexcept
{
    void* pWalk = apNode;
    for (uint32_t depth = 0; pWalk && depth < 64; ++depth)
    {
        void* pParent = At<void*>(pWalk, kParentOffset);
        if (!pParent || !ReadableMemory::IsReadable(pParent, kNiAVObjectSize))
            break;
        pWalk = pParent;
    }
    return pWalk;
}

/**
 * Move a whole 3D tree without knowing any bone names, for a corpse being dragged elsewhere.
 * The skinned body follows the bones, not the root, so every node's world transform is shifted.
 * Used for a body VRPose::NoBones describes: a Dwemer automaton, a spider, anything the bone name search cannot read.
 * The flattened bone array is shifted too where there is one; rotations stay as the local animation has them.
 *
 * @param apRoot The tree's root; null moves nothing.
 * @param acWorldOffset How far to move it, in world units.
 */
void ShiftTree(void* apRoot, const glm::vec3& acWorldOffset) noexcept
{
    if (!apRoot)
        return;

    VisitTree(apRoot, 8192, [&acWorldOffset](void* pObject) {
        NiTransform& world = At<NiTransform>(pObject, kWorldOffset);
        world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
    });

    // Bones that were flattened away exist only in the tree's own array and have no node to walk to.
    uint32_t count = 0;
    if (uint8_t* pArray = GetBoneArray(FindByRtti(apRoot, "BSFlattenedBoneTree"), count))
    {
        for (uint32_t i = 0; i < count && i < kMaxBones; ++i)
        {
            uint8_t* pEntry = pArray + static_cast<size_t>(i) * kBoneEntrySize;
            if (NodeOfEntry(pEntry))
                continue; // already shifted above, through its node
            NiTransform& world = At<NiTransform>(pEntry, kBoneEntryWorld);
            world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
        }
    }
}

void PoseActor(const Rig& acRig, const NativeRemotePose& acPose, const glm::vec3& acWorldOffset) noexcept
{
    // Body size: the skeleton root's local scale is set so that its world scale matches the sender's.
    // Only the local is written; the game carries it into every bone's world on its next update.
    // Written only when it differs, so a copy that already matches is left alone.
    if (acPose.HasScale && acRig.SkeletonRoot.pNode)
    {
        NiTransform& local = At<NiTransform>(acRig.SkeletonRoot.pNode, kLocalOffset);
        const NiTransform& world = At<NiTransform>(acRig.SkeletonRoot.pNode, kWorldOffset);
        const float parentScale = local.scale > 0.001f ? world.scale / local.scale : 1.f;
        const float wanted = parentScale > 0.001f ? acPose.RootScale / parentScale : local.scale;
        if (wanted > 0.05f && wanted < 20.f && std::fabs(local.scale - wanted) > 0.003f)
            local.scale = wanted;
    }

    const glm::mat3 rootRotation = ToGlm(At<NiTransform>(acRig.pRoot, kWorldOffset).rotate);
    const bool cHasOffset = glm::dot(acWorldOffset, acWorldOffset) > 0.0001f;

    // The whole-body offset (a dragged corpse, or where the owner's body stands) is applied once to every bone, before any rotation.
    // Adding it at each posed bone would stack it down the chain, six times at the hand, and stretch the body.
    // It must reach the whole skeleton, because the waist is skinned to NPC Spine [Spn0], which is not a posed bone.
    // Rig::Body holds every node and entry once; the posed bones and their descendants are the fallback without a root.
    // The fallback dedups nodes and flattened entries separately, since a bone can be listed under several parents.
    if (cHasOffset && !acRig.Body.empty())
    {
        for (const RigBone& bone : acRig.Body)
        {
            if (bone.pNode)
            {
                NiTransform& world = At<NiTransform>(bone.pNode, kWorldOffset);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
            if (bone.pEntry)
            {
                NiTransform& world = *reinterpret_cast<NiTransform*>(bone.pEntry + kBoneEntryWorld);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
        }
    }
    else if (cHasOffset)
    {
        std::unordered_set<const void*> movedNodes;
        std::unordered_set<const void*> movedEntries;
        const auto shift = [&](const RigBone& acBone)
        {
            if (acBone.pNode && movedNodes.insert(acBone.pNode).second)
            {
                NiTransform& world = At<NiTransform>(acBone.pNode, kWorldOffset);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
            if (acBone.pEntry && movedEntries.insert(acBone.pEntry).second)
            {
                NiTransform& world = *reinterpret_cast<NiTransform*>(acBone.pEntry + kBoneEntryWorld);
                world.translate = FromGlm(ToGlm(world.translate) + acWorldOffset);
            }
        };
        for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        {
            shift(acRig.Bones[i]);
            for (const RigBone& below : acRig.Descendants[i])
                shift(below);
        }
    }

    // Each bone carries its descendants, so apply the resolved skeleton's parents first.
    // World transforms are written directly because nothing recomputes them from locals before the draw.
    for (size_t orderIndex = 0; orderIndex < acRig.PoseOrderCount; ++orderIndex)
    {
        const uint32_t i = acRig.PoseOrder[orderIndex];
        const RigBone& bone = acRig.Bones[i];
        // Legs only when the sender's trackers drive them; otherwise the walk animation keeps them.
        // A dragged body still moves from the waist down, since the offset above reached the legs too.
        if (i >= VRPose::kUpperBoneCount && !acPose.HasLegs)
            continue;
        if (!bone.pNode && !bone.pEntry)
            continue;
        const NiTransform current = bone.World();
        const glm::mat3 currentRotation = ToGlm(current.rotate);
        const glm::mat3 wantedRotation = rootRotation * glm::mat3_cast(acPose.Bones[i]);
        const glm::mat3 delta = wantedRotation * glm::transpose(currentRotation);
        const glm::vec3 pivot = ToGlm(current.translate);

        NiTransform wanted = current;
        wanted.rotate = FromGlm(wantedRotation);
        bone.WriteWorld(wanted);

        // The local rotation is derived fresh every frame: the parent's world rotation, inverted, times the wanted one.
        // Correcting the previous frame's local instead accumulates drift until the matrix is no longer a rotation.
        // A skinned body on such a bone stops being drawn; deriving it fresh cannot drift and repairs a bone that has.
        if (bone.pNode)
        {
            NiTransform local = At<NiTransform>(bone.pNode, kLocalOffset);
            void* pParent = At<void*>(bone.pNode, kParentOffset);
            if (pParent)
                local.rotate = FromGlm(glm::transpose(ToGlm(At<NiTransform>(pParent, kWorldOffset).rotate)) * wantedRotation);
            else
                local.rotate = FromGlm(ToGlm(local.rotate) * glm::transpose(currentRotation) * wantedRotation);
            bone.WriteLocal(local);
        }

        for (const RigBone& child : acRig.Descendants[i])
        {
            NiTransform world = child.World();
            world.rotate = FromGlm(delta * ToGlm(world.rotate));
            world.translate = FromGlm(pivot + delta * (ToGlm(world.translate) - pivot));
            child.WriteWorld(world);
        }
    }

    if (acPose.HasFingers)
    {
        constexpr size_t cPerHand = VRPose::kFingersPerHand * VRPose::kBonesPerFinger;
        if (acRig.FingersFound[kLeftSide])
            WriteFingerRotations(acRig.Fingers[kLeftSide], acRig.Bones[VRPose::kLeftHand].World(), acPose.Fingers.data());
        if (acRig.FingersFound[kRightSide])
            WriteFingerRotations(acRig.Fingers[kRightSide], acRig.Bones[VRPose::kRightHand].World(), acPose.Fingers.data() + cPerHand);
    }
}

// Turns one posed bone about its joint by a world rotation and carries everything below it along.
// It is the step PoseActor takes for each bone, used for corrections after the pose is written.
void TurnBone(const Rig& acRig, const uint32_t aBone, const glm::mat3& acDelta) noexcept
{
    const RigBone& bone = acRig.Bones[aBone];
    const NiTransform current = bone.World();
    const glm::vec3 pivot = ToGlm(current.translate);
    const glm::mat3 wantedRotation = acDelta * ToGlm(current.rotate);

    NiTransform wanted = current;
    wanted.rotate = FromGlm(wantedRotation);
    bone.WriteWorld(wanted);
    if (bone.pNode)
    {
        NiTransform local = At<NiTransform>(bone.pNode, kLocalOffset);
        if (void* pParent = At<void*>(bone.pNode, kParentOffset))
        {
            local.rotate = FromGlm(glm::transpose(ToGlm(At<NiTransform>(pParent, kWorldOffset).rotate)) * wantedRotation);
            bone.WriteLocal(local);
        }
    }
    for (const RigBone& child : acRig.Descendants[aBone])
    {
        NiTransform world = child.World();
        world.rotate = FromGlm(acDelta * ToGlm(world.rotate));
        world.translate = FromGlm(pivot + acDelta * (ToGlm(world.translate) - pivot));
        child.WriteWorld(world);
    }
}

// The rotation that turns direction acFrom onto acTo (both unit length).
glm::mat3 RotationBetween(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const glm::vec3 axis = glm::cross(acFrom, acTo);
    const float sine = glm::length(axis);
    const float cosine = glm::clamp(glm::dot(acFrom, acTo), -1.f, 1.f);
    if (sine < 1e-5f)
        return glm::mat3(1.f); // already aligned, or exactly opposite (an arm never needs a half turn in one frame)
    return glm::mat3_cast(glm::angleAxis(std::atan2(sine, cosine), axis / sine));
}

// How far a rotation turns, in degrees.
float DegreesOf(const glm::mat3& acRotation) noexcept
{
    const float cosine = glm::clamp((acRotation[0][0] + acRotation[1][1] + acRotation[2][2] - 1.f) * 0.5f, -1.f, 1.f);
    return glm::degrees(std::acos(cosine));
}

// Puts the copy's weapons where its owner holds them (VRPose::HasWeapons), once the hands are already placed.
// Each attach node and the weapon below it get the grip the owner's first-person hand has relative to its body's hand.
// A grip more than 30 units from where this skeleton hangs the weapon means VRIK could not reach the controller.
// Such a weapon is left where the skeleton hangs it.
void PlaceWeapons(const Rig& acRig, const NativeRemotePose& acPose) noexcept
{
    if (!acPose.HasWeapons)
        return;

    for (size_t side = 0; side < 2; ++side)
    {
        const RigBone& attach = acRig.Attach[side];
        const RigBone& hand = acRig.Bones[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        if (!acPose.WeaponHeld[side] || !attach.pNode || (!hand.pNode && !hand.pEntry))
            continue;

        const NiTransform handWorld = hand.World();
        const auto [wantedRotation, wantedAt] =
            HeldWeaponPlacement::InHand(ToGlm(handWorld.rotate), ToGlm(handWorld.translate), handWorld.scale, acPose.WeaponRotation[side], acPose.WeaponOffset[side]);
        const NiTransform current = attach.World();
        const glm::vec3 pivot = ToGlm(current.translate);
        const glm::mat3 delta = wantedRotation * glm::transpose(ToGlm(current.rotate));
        const float moved = glm::distance(wantedAt, pivot);
        if (!std::isfinite(moved) || !std::isfinite(wantedRotation[0][0]))
            continue;

        if (moved > 30.f)
            continue;

        NiTransform placed = current;
        placed.rotate = FromGlm(wantedRotation);
        placed.translate = FromGlm(wantedAt);
        attach.WriteWorld(placed);
        for (const RigBone& below : acRig.AttachBelow[side])
        {
            NiTransform world = below.World();
            world.rotate = FromGlm(delta * ToGlm(world.rotate));
            world.translate = FromGlm(wantedAt + delta * (ToGlm(world.translate) - pivot));
            below.WriteWorld(world);
        }
    }
}

// Puts the copy's hands where the owner's are; joint rotations alone leave them about 30 units off.
// The arms differ in length, and VRIK places the owner's body relative to the root differently than the copy's animation.
// The owner sends its hand positions relative to its root (VRPose hand offsets).
// Shoulder and elbow are bent so each hand lands there (two-bone IK, elbow kept on the side it already bends to).
// The hand then gets the owner's own orientation back.
// Returns which hands were put there: bit 0 left, bit 1 right.
uint32_t ReachHands(const Rig& acRig, const NativeRemotePose& acPose) noexcept
{
    uint32_t reached = 0;
    if (!acPose.HasHandCheck || !acRig.pRoot)
        return reached;

    const NiTransform& rootWorld = At<NiTransform>(acRig.pRoot, kWorldOffset);
    const glm::mat3 rootRotation = ToGlm(rootWorld.rotate);
    const glm::vec3 rootAt = ToGlm(rootWorld.translate);

    struct Arm
    {
        uint32_t Upper, Fore, Hand;
        int Side;
    };
    for (const Arm& arm : {Arm{VRPose::kLeftUpperArm, VRPose::kLeftForearm, VRPose::kLeftHand, 0}, Arm{VRPose::kRightUpperArm, VRPose::kRightForearm, VRPose::kRightHand, 1}})
    {
        const RigBone& upper = acRig.Bones[arm.Upper];
        const RigBone& fore = acRig.Bones[arm.Fore];
        const RigBone& hand = acRig.Bones[arm.Hand];
        if ((!upper.pNode && !upper.pEntry) || (!fore.pNode && !fore.pEntry) || (!hand.pNode && !hand.pEntry))
            continue;

        const glm::vec3 target = rootAt + rootRotation * acPose.HandOffsets[arm.Side];
        const glm::vec3 shoulder = ToGlm(upper.World().translate);
        const glm::vec3 elbow = ToGlm(fore.World().translate);
        const glm::vec3 wrist = ToGlm(hand.World().translate);

        // Fine-tuning only: the owner's rotations already shape the arm, and what is left is arm length, a few units.
        // A bigger gap means the two disagree about where the body is, and pulling that far folds the arm across the chest.
        // Then the arm keeps the owner's pose untouched.
        constexpr float cMaxCorrection = 15.f;
        const float cGap = glm::distance(target, wrist);
        if (!std::isfinite(cGap) || cGap > cMaxCorrection)
            continue;
        const float upperLength = glm::distance(shoulder, elbow);
        const float foreLength = glm::distance(elbow, wrist);
        const glm::vec3 toTarget = target - shoulder;
        const float reach = glm::length(toTarget);
        // A bad read, not a hand: further from the shoulder than two arm lengths, or not a number at all.
        if (!std::isfinite(reach) || upperLength < 1.f || foreLength < 1.f || reach < 1.f || reach > 2.f * (upperLength + foreLength))
            continue;

        const float clamped = glm::clamp(reach, std::fabs(upperLength - foreLength) + 0.5f, upperLength + foreLength - 0.5f);
        const glm::vec3 along = toTarget / reach;

        // The plane the arm bends in, from how it bends now; a straight arm bends downwards, as an elbow does.
        glm::vec3 normal = glm::cross(elbow - shoulder, wrist - shoulder);
        if (glm::dot(normal, normal) < 1e-4f)
            normal = glm::cross(along, rootRotation * glm::vec3(0.f, 0.f, -1.f));
        if (glm::dot(normal, normal) < 1e-6f)
            continue;
        glm::vec3 side = glm::normalize(glm::cross(glm::normalize(normal), along));
        if (glm::dot(elbow - shoulder, side) < 0.f)
            side = -side;

        const float cosine = glm::clamp((upperLength * upperLength + clamped * clamped - foreLength * foreLength) / (2.f * upperLength * clamped), -1.f, 1.f);
        const float sine = std::sqrt(std::max(0.f, 1.f - cosine * cosine));
        const glm::vec3 elbowWanted = shoulder + upperLength * (cosine * along + sine * side);

        TurnBone(acRig, arm.Upper, RotationBetween(glm::normalize(elbow - shoulder), glm::normalize(elbowWanted - shoulder)));

        const glm::vec3 elbowNow = ToGlm(fore.World().translate);
        const glm::vec3 wristNow = ToGlm(hand.World().translate);
        const glm::vec3 wristWanted = shoulder + clamped * along;
        if (glm::distance(wristNow, elbowNow) < 1.f || glm::distance(wristWanted, elbowNow) < 1.f)
            continue;
        TurnBone(acRig, arm.Fore, RotationBetween(glm::normalize(wristNow - elbowNow), glm::normalize(wristWanted - elbowNow)));

        // The owner's own hand orientation, which the two turns above carried away.
        const glm::mat3 handWanted = rootRotation * glm::mat3_cast(acPose.Bones[arm.Hand]);
        TurnBone(acRig, arm.Hand, handWanted * glm::transpose(ToGlm(hand.World().rotate)));
        reached |= 1u << arm.Side;
    }
    return reached;
}

#ifdef SKYRIMVR
// Which hands of each copy ReachHands put on its owner's, and at which frame end: what a held object may be drawn from (PosedHands).
// Frame ends are counted, as the hands are read on the main thread's update after one.
std::atomic<uint32_t> s_frameEnds{0};
std::mutex s_posedHandsLock;
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> s_posedHands; // form id -> frame end, hands

void NotePosedHands(const uint32_t aFormId, const uint32_t aSides) noexcept
{
    std::scoped_lock lock(s_posedHandsLock);
    if (s_posedHands.size() > 256)
        s_posedHands.clear();
    s_posedHands[aFormId] = {s_frameEnds.load(std::memory_order_relaxed), aSides};
}

bool HandsPosedFromPose(const uint32_t aActorFormId, const size_t aSide) noexcept
{
    if (aSide > 1)
        return false;
    std::scoped_lock lock(s_posedHandsLock);
    const auto it = s_posedHands.find(aActorFormId);
    if (it == s_posedHands.end() || !((it->second.second >> aSide) & 1u))
        return false;
    // Posed at the last frame end or the one before; any older and the animation has put the hand back.
    return s_frameEnds.load(std::memory_order_relaxed) - it->second.first <= 1;
}

// A held object may be drawn from the hands put on their owner's here.
TiltedPhoques::Initializer s_posedHandsQuery([]() { PosedHands::SetQuery(&HandsPosedFromPose); });
#endif

// ---------------------------------------------------------------------------------------------------------------
// Weapon touch: a controller pulse when someone else's blade or hand touches yours.
//
// No other mod does this: PLANCK covers NPC bodies, parry mods only incoming attacks, HIGGS only grabbable objects.
// Only this mod knows where another player's weapon is.
//
// A weapon is a segment from the hand's attach node to the farthest node below it, measured by walking the children.
// A weapon with no child nodes gets no blade segment; its hand still counts.
struct Segment
{
    glm::vec3 A{};
    glm::vec3 B{};
    bool Valid = false;
};

// Distance between two line segments (the usual clamped-parameter solution).
// A segment that is a single point works too, because the denominators are guarded.
float SegmentDistance(const Segment& acP, const Segment& acQ) noexcept
{
    const glm::vec3 d1 = acP.B - acP.A;
    const glm::vec3 d2 = acQ.B - acQ.A;
    const glm::vec3 r = acP.A - acQ.A;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);

    float s = 0.f, t = 0.f;
    constexpr float cEps = 1e-5f;

    if (a <= cEps && e <= cEps)
        return glm::length(acP.A - acQ.A);

    if (a <= cEps)
    {
        t = glm::clamp(f / e, 0.f, 1.f);
    }
    else
    {
        const float c = glm::dot(d1, r);
        if (e <= cEps)
        {
            s = glm::clamp(-c / a, 0.f, 1.f);
        }
        else
        {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > cEps ? glm::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f)
            {
                t = 0.f;
                s = glm::clamp(-c / a, 0.f, 1.f);
            }
            else if (t > 1.f)
            {
                t = 1.f;
                s = glm::clamp((b - c) / a, 0.f, 1.f);
            }
        }
    }

    return glm::length((acP.A + d1 * s) - (acQ.A + d2 * t));
}

// The world bound of the thing in this hand: how long it is and which way it points.
// A weapon mesh is usually one geometry at the grip with no child nodes, so the blade exists only in its vertices.
// The bound's centre sits down the middle of the blade, so grip and centre give the direction and, doubled, the length.
struct NiBoundRead
{
    glm::vec3 Centre{};
    float Radius = 0.f;
    bool Valid = false;
};

NiBoundRead ReadWorldBound(void* apObject, const glm::vec3& acGrip) noexcept
{
    NiBoundRead out;
    if (!apObject)
        return out;

    const glm::vec3 centre = ToGlm(At<NiPoint3>(apObject, kWorldBoundOffset));
    const float radius = At<float>(apObject, kWorldBoundOffset + 0xC);

    // Sizes a weapon can have: a dagger is about 20 units, a greatsword about 90.
    // Anything outside this is not a bound but whatever else lives at that offset.
    if (!std::isfinite(radius) || radius < 4.f || radius > 300.f)
        return out;
    if (!IsFiniteVector(centre))
        return out;
    if (glm::distance(centre, acGrip) > 400.f)
        return out;

    out.Centre = centre;
    out.Radius = radius;
    out.Valid = true;
    return out;
}

// What one actor holds, plus the hands themselves: a hand counts as a very short weapon.
// Searching and reading are separate: the skeleton search runs at most twice a second, and its results are checked before use.
// Searching every frame would walk node trees that other threads create and free as actors stream in and out.
// Between searches only transforms are read, which is cheap and never calls into a possibly freed object.
struct HeldSide
{
    void* pAttach = nullptr;   // the WEAPON / SHIELD node, if anything is equipped
    void* pAttachVTable = nullptr;
    void* pTipNode = nullptr;  // a distinct far node (a bow limb), when the weapon has one
    void* pTipVTable = nullptr;
    void* pHand = nullptr;
    void* pHandVTable = nullptr;
    bool UseBound = false;     // never set: the per-frame path reads no bound (see ReadHeldSegments)
};

struct Holding
{
    HeldSide Right{};
    HeldSide Left{};
    Segment RightWeapon{};
    Segment LeftWeapon{};
    Segment RightHand{};
    Segment LeftHand{};
    std::chrono::steady_clock::time_point SearchAt{};
    void* pRoot = nullptr;
};

// Skyrim reuses a freed node's memory right away, so a non-null pointer may now belong to something else.
// Comparing the vtable it had when it was found is the cheapest check for that.
bool HasSameVTable(void* apNode, void* apVTable) noexcept
{
    return apNode && apVTable && *static_cast<void**>(apNode) == apVTable;
}

/**
 * Find one side's hand, weapon attach node and weapon tip in a body's 3D, with the class table each had when found.
 * The tip is the attach node's farthest descendant, within 400 units; a weapon whose farthest node is 8 units or less away has none.
 *
 * @param apRoot The body's 3D root.
 * @param acpAttachName The attach node's name for that side (WEAPON or SHIELD).
 * @param acpHandName The hand node's name for that side.
 * @param aSide Receives what was found; anything not found stays null.
 */
void FindHeldNodes(void* apRoot, const char* acpAttachName, const char* acpHandName, HeldSide& aSide) noexcept
{
    aSide = HeldSide{};

    if (void* pHand = FindShallowest(apRoot, acpHandName))
    {
        aSide.pHand = pHand;
        aSide.pHandVTable = *static_cast<void**>(pHand);
    }

    void* pAttach = FindShallowest(apRoot, acpAttachName);
    if (!pAttach)
        return;

    aSide.pAttach = pAttach;
    aSide.pAttachVTable = *static_cast<void**>(pAttach);

    // A weapon made of several nodes (a bow's limbs, a staff's head) has a far node to track, an actual position.
    // A single-mesh weapon has none, and only its bound describes the blade.
    const glm::vec3 grip = ToGlm(At<NiTransform>(pAttach, kWorldOffset).translate);
    float best = 0.f;

    VisitBelow(pAttach, 256, [&](void* pObject) {
        const float d = glm::distance(grip, ToGlm(At<NiTransform>(pObject, kWorldOffset).translate));
        if (d > best && d < 400.f)
        {
            best = d;
            aSide.pTipNode = pObject;
        }
    });

    if (aSide.pTipNode && best > 8.f)
    {
        aSide.pTipVTable = *static_cast<void**>(aSide.pTipNode);
        return;
    }

    aSide.pTipNode = nullptr;
    aSide.UseBound = false; // see ReadHeldSegments: nothing is read from a bound on the per-frame path
}

// Per frame. Nothing here searches, and nothing here makes a virtual call.
void ReadHeldSegments(const HeldSide& acSide, Segment& aWeapon, Segment& aHand) noexcept
{
    aWeapon.Valid = false;
    aHand.Valid = false;

    if (HasSameVTable(acSide.pHand, acSide.pHandVTable))
    {
        const glm::vec3 p = ToGlm(At<NiTransform>(acSide.pHand, kWorldOffset).translate);
        if (std::isfinite(p.x))
        {
            aHand.A = p;
            aHand.B = p;
            aHand.Valid = true;
        }
    }

    if (!HasSameVTable(acSide.pAttach, acSide.pAttachVTable))
        return;

    const glm::vec3 grip = ToGlm(At<NiTransform>(acSide.pAttach, kWorldOffset).translate);
    if (!std::isfinite(grip.x))
        return;

    glm::vec3 tip = grip;

    if (HasSameVTable(acSide.pTipNode, acSide.pTipVTable))
    {
        tip = ToGlm(At<NiTransform>(acSide.pTipNode, kWorldOffset).translate);
    }
    else
    {
        // No far node, so no blade segment; the hand segment still covers this side.
        // The world bound is not read here: a raw read every frame, on nodes that may be freed, is a crash risk.
        return;
    }

    if (!std::isfinite(tip.x) || glm::distance(grip, tip) > 400.f)
        return;

    aWeapon.A = grip;
    aWeapon.B = tip;
    aWeapon.Valid = true;
}

// Locks ResolveHolding's cache: actor work runs on several threads ("Actors are multi-threaded" in BehaviorVar::Patch).
// Unlocked concurrent insert and erase on an unordered_map corrupts the heap, and the crash then shows up later in game code.
// ResolveHolding returns a copy, so nothing keeps a pointer into the map after the lock is released.
std::mutex s_heldLock;

bool ResolveHolding(Actor* apActor, Holding& aOut) noexcept
{
    static std::unordered_map<uint32_t, Holding> s_held;

    if (!apActor)
        return false;
    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(s_heldLock);

    Holding& held = s_held[apActor->formID];

    // Twice a second, or whenever the 3D was rebuilt under us. Everything else is reads.
    if (held.pRoot != pRoot || now >= held.SearchAt)
    {
        held.pRoot = pRoot;
        held.SearchAt = now + std::chrono::milliseconds(500);
        FindHeldNodes(pRoot, "WEAPON", "NPC R Hand [RHnd]", held.Right);
        FindHeldNodes(pRoot, "SHIELD", "NPC L Hand [LHnd]", held.Left);
    }

    ReadHeldSegments(held.Right, held.RightWeapon, held.RightHand);
    ReadHeldSegments(held.Left, held.LeftWeapon, held.LeftHand);

    if (s_held.size() > 64)
    {
        for (auto it = s_held.begin(); it != s_held.end();)
            it = (it->second.SearchAt + std::chrono::seconds(10) < now) ? s_held.erase(it) : std::next(it);
    }

    aOut = held;
    return true;
}

// Everything the local player could touch this frame, filled on the game thread and read at the end of it.
std::mutex s_touchLock;

// What the Havok world holds for a copy (one ragdoll bone's body and its drawn weapon's body), and which layers collide.
// Read only, once per copy per session.
// Layouts from PLANCK's static_asserts and CommonLibVR-NG; the filter info offset is measured, not from a source.
constexpr uint32_t kFilterInfoOffset = 0x4C; // hkpWorldObject::collidable.broadPhaseHandle.collisionFilterInfo

std::string DescribeHavokBody(const char* acpWhat, void* apNode, const uint8_t* apHavok) noexcept
{
    const uint32_t filter = At<uint32_t>(const_cast<uint8_t*>(apHavok), kFilterInfoOffset);
    const char* pName = GetName(apNode);
    return fmt::format("{} '{}': layer {}, group {}, collision {}, motion type {}, {}", acpWhat, pName && ReadableMemory::IsReadable(pName, 1) ? pName : "?", filter & 0x7F, filter >> 16,
                       (filter & (1u << 14)) ? "off" : "on", At<uint8_t>(const_cast<uint8_t*>(apHavok), kMotionTypeOffset),
                       At<void*>(const_cast<uint8_t*>(apHavok), kHkWorldOffset) ? "in the world" : "not in the world");
}

// Logs once per copy what physics bodies its ragdoll and its drawn weapon have (the rig tests read this line).
void LogPhysicsOnce(const uint32_t aFormId, const Rig& acRig) noexcept
{
    static std::unordered_set<uint32_t> s_logged;
    if (s_logged.size() > 256)
        s_logged.clear();
    // A drawn weapon hangs below the attach node; wait for one.
    if (s_logged.count(aFormId) || !acRig.Attach[1].pNode || acRig.AttachBelow[1].empty())
        return;
    s_logged.insert(aFormId);

    uint8_t* pWeaponBody = nullptr;
    void* pWeaponNode = nullptr;
    for (const RigBone& below : acRig.AttachBelow[1])
        if (below.pNode && (pWeaponBody = HavokBodyOf(below.pNode)) != nullptr)
        {
            pWeaponNode = below.pNode;
            break;
        }

    uint8_t* pBoneBody = nullptr;
    void* pBoneNode = nullptr;
    for (const RigBone& bone : acRig.Body)
        if (bone.pNode && (pBoneBody = HavokBodyOf(bone.pNode)) != nullptr)
        {
            pBoneNode = bone.pNode;
            break;
        }

    spdlog::info("PhysicsProbe: actor {:X} -- {}; {}", aFormId, pBoneBody ? DescribeHavokBody("ragdoll bone", pBoneNode, pBoneBody) : std::string("no ragdoll bone with a body"),
                 pWeaponBody ? DescribeHavokBody("drawn weapon", pWeaponNode, pWeaponBody) : fmt::format("no body under the drawn weapon ({} nodes)", acRig.AttachBelow[1].size()));
}

// The other player's drawn weapon as a physical body in this game.
//
// The copy's drawn weapon already has its own rigid body, which the game keeps out of the world.
// That body is keyframed, on the biped layer 8, in the copy's own collision group, so it never hits its owner's ragdoll.
// Layer 8 collides with clutter, weapons, bipeds, props and HIGGS's hands and weapons (56), not with walls or capsules.
// While the weapon is drawn, the body is added to the world and moved each frame to where the weapon is drawn.
// HIGGS drives the player's own weapon body the same way (hand.cpp, MoveHandAndWeaponCollision).
//
// The weapon's node and its body wrapper stay referenced while the body is in the world, so neither is freed under physics.
// The body leaves the world at the first frame end where the copy was not posed or its weapon is not fully drawn.
// It also leaves when the node no longer hangs under the copy's hand, or when the copy's ragdoll is in another world.
constexpr uint32_t kBodyTransformOffset = 0x170;        // hkpEntity::motion (0x150) .motionState.transform: rotation columns, then translation
constexpr uint32_t kRigidBodyTRotationOffset = 0x40;    // bhkRigidBodyT::rotation (hkQuaternion)
constexpr uint32_t kRigidBodyTTranslationOffset = 0x50; // bhkRigidBodyT::translation (hkVector4, Havok units)

using THkpWorldAddEntity = void*(void* apWorld, void* apEntity, int aActivation);
// Havok's removeEntity returns an hkBool, which MSVC returns through a hidden pointer after `this` (HIGGS calls it so).
using THkpWorldRemoveEntity = bool*(void* apWorld, bool* apResult, void* apEntity);
using THkpEntitySetPositionAndRotation = void(void* apEntity, const float* apPosition, const float* apRotation);
// The original ahkpWorld::stepDeltaTime that bhkWorld::Update calls, saved by the hook below; null until the hook is in.
using TStepDeltaTime = int(void* apHkpWorld, float aSeconds);
TStepDeltaTime* s_stepDeltaTime = nullptr;
constexpr uint32_t kMaxAngularVelocityOffset = 0x21B; // hkpEntity::motion.motionState.maxAngularVelocity (hkUFloat8)

using THkRealToUFloat8 = bool(uint8_t& aOut, const float& acValue);

struct WeaponBody
{
    void* pNode = nullptr;     // the weapon's node, referenced
    void* pWrapper = nullptr;  // its bhkRigidBody, referenced
    uint8_t* pHavok = nullptr; // the hkpRigidBody inside it
    void* pBhkWorld = nullptr; // the world it was put in, checked (by name) when it was
    bool IsT = false;          // a bhkRigidBodyT, with its own offset from the node
    bool Seen = false;         // driven at this frame end
    bool Listening = false;    // our contact listener is on it
    uint8_t GameMaxAngularVelocity = 0; // the game's limit, put back when the body is let go
};
// Per copy and side (left, right). Touched only from the renderer's frame end and the start of a frame, both on the main thread.
std::unordered_map<uint64_t, WeaponBody> s_weaponBodies;
std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_weaponBodyRetryAt;

// A weapon body found at the frame end, waiting for the start of the next frame.
struct PendingWeaponBody
{
    uint64_t Key = 0;
    uint32_t FormId = 0;
    uint32_t Side = 0;
    WeaponBody Body{};
    void* pWorld = nullptr;
    alignas(16) float Position[4]{};
    alignas(16) float Rotation[4]{};
};
TiltedPhoques::Vector<PendingWeaponBody> s_pendingWeaponBodies;

// What the weapon bodies touch, from Havok's own contact callbacks on each of them.
// Havok raises them during its step, possibly on worker threads, so they only record what touched what.
// They use a lock of their own, never held while waiting for the world's; the frame end reports the contacts.
// Layout: Havok 2010's hkpContactListener (slots 0-2, as PLANCK overrides them and CommonLibVR-NG lists them) and its events.
constexpr uint32_t kEventBodiesOffset = 0x08;       // hkpCollisionEvent::bodies[2]
constexpr uint32_t kEventContactPointOffset = 0x28; // hkpContactPointEvent::contactPoint (hkContactPoint, position first)

using THkpEntityContactListener = void(void* apEntity, void* apListener);

struct WeaponContact
{
    const void* pBodies[2]{};
    uint32_t Filters[2]{};
    uint8_t MotionTypes[2]{};
    glm::vec3 Point{};          // Havok units
    glm::vec3 Velocities[2]{};  // each body's linear velocity, Havok units a second
    // Per body: 1 if it is one of HIGGS's own bodies (hands, equipped weapon), 2 if one of them holds it, else 0.
    // HiggsAt is where that HIGGS body is, in Havok units.
    uint8_t HiggsKind[2]{};
    glm::vec3 HiggsAt[2]{};
};

// A clash: the other player's weapon met one of the local player's HIGGS bodies, or something one of them holds.
// It is queued at the frame end; CharacterService takes it to play the pulse and send it.
struct PendingClash
{
    uint32_t FormId;
    uint8_t Side;
    glm::vec3 Point; // game units
    float Speed;     // game units a second
    glm::vec3 Hand;  // game units: the HIGGS body, which says which of the player's hands it was
    bool Start;      // the two started touching (a meeting); otherwise still touching
    bool Held;       // what met it is held up with HIGGS's grab, not one of HIGGS's own bodies
};
std::mutex s_clashesLock;
std::vector<PendingClash> s_clashes;
// HIGGS's layer: only the local player's hands and the body HIGGS gives the equipped weapon are on it.
constexpr uint32_t kHiggsLayer = 56;

bool IsHiggsBody(const uint8_t* apBody) noexcept
{
    return apBody && (*reinterpret_cast<const uint32_t*>(apBody + kFilterInfoOffset) & 0x7F) == kHiggsLayer;
}

// The HIGGS body holding a dynamic body through a constraint between the two (HIGGS's physics grab), or null.
// Called inside Havok's step, where constraints are not added or removed.
// Layouts from CommonLibVR-NG (hkpEntity, hkpConstraintInstance, hkConstraintInternal).
const uint8_t* HiggsBodyHolding(const uint8_t* apBody) noexcept
{
    const auto partner = [apBody](const uint8_t* apA, const uint8_t* apB) -> const uint8_t*
    {
        const uint8_t* pOther = apA == apBody ? apB : apA;
        return pOther != apBody && IsHiggsBody(pOther) ? pOther : nullptr;
    };
    // constraintsMaster: hkSmallArray<hkConstraintInternal> (data, uint16 size), 0x40 each, entities at +0x08 and +0x10.
    const uint8_t* pMaster = *reinterpret_cast<const uint8_t* const*>(apBody + HavokLayout::kConstraintsMasterOffset);
    const uint16_t masterCount = *reinterpret_cast<const uint16_t*>(apBody + HavokLayout::kConstraintsMasterSizeOffset);
    if (pMaster && masterCount <= 32)
        for (uint16_t i = 0; i < masterCount; ++i)
        {
            const uint8_t* pInternal = pMaster + i * HavokLayout::kConstraintInternalSize;
            if (const uint8_t* pHand = partner(*reinterpret_cast<const uint8_t* const*>(pInternal + 0x08), *reinterpret_cast<const uint8_t* const*>(pInternal + 0x10)))
                return pHand;
        }
    // constraintsSlave: hkArray<hkpConstraintInstance*> (data, int32 size), entities at +0x28 and +0x30.
    const auto* const* ppSlaves = *reinterpret_cast<const uint8_t* const* const*>(apBody + HavokLayout::kConstraintsSlaveOffset);
    const int32_t slaveCount = *reinterpret_cast<const int32_t*>(apBody + HavokLayout::kConstraintsSlaveSizeOffset);
    if (ppSlaves && slaveCount > 0 && slaveCount <= 32)
        for (int32_t i = 0; i < slaveCount; ++i)
            if (const uint8_t* pInstance = ppSlaves[i])
                if (const uint8_t* pHand = partner(
                        *reinterpret_cast<const uint8_t* const*>(pInstance + HavokLayout::kConstraintEntityAOffset),
                        *reinterpret_cast<const uint8_t* const*>(pInstance + HavokLayout::kConstraintEntityBOffset)))
                    return pHand;
    return nullptr;
}

std::mutex s_contactsLock;
std::array<WeaponContact, 64> s_contacts{};
size_t s_contactCount = 0;

// Recent points (Havok units) where this player's HIGGS bodies, or what they hold, touched another player's weapon body.
// IsWeaponTouchAt uses them: a hit this player lands at one of these points landed on the weapon, not on its owner.
// Guarded by s_contactsLock.
struct WeaponTouch
{
    glm::vec3 Point{};
    std::chrono::steady_clock::time_point At{};
};
std::array<WeaponTouch, 32> s_recentTouches{};
size_t s_nextRecentTouch = 0;

void OnWeaponContactPoint(void*, const uint8_t* apEvent) noexcept
{
    WeaponContactRules::OnContactPoint<WeaponContact>(
        apEvent, kEventBodiesOffset, kEventContactPointOffset,
        [](WeaponContact& contact, size_t i)
        {
            const auto* pBody = static_cast<const uint8_t*>(contact.pBodies[i]);
            contact.Filters[i] = *reinterpret_cast<const uint32_t*>(pBody + kFilterInfoOffset);
            contact.MotionTypes[i] = pBody[kMotionTypeOffset];
            const float* pVelocity = reinterpret_cast<const float*>(pBody + HavokLayout::kLinearVelocityOffset); // hkpEntity::motion.linearVelocity
            contact.Velocities[i] = glm::vec3{pVelocity[0], pVelocity[1], pVelocity[2]};
            // Motion types 1-3 and 6 are dynamic: only those can be held by HIGGS's grab constraint.
            const uint8_t motionType = contact.MotionTypes[i];
            const uint8_t* pHiggs = IsHiggsBody(pBody) ? pBody : (motionType >= 1 && motionType <= 3) || motionType == 6 ? HiggsBodyHolding(pBody) : nullptr;
            if (pHiggs)
            {
                contact.HiggsKind[i] = pHiggs == pBody ? 1 : 2;
                const float* pAt = reinterpret_cast<const float*>(pHiggs + kBodyTranslationOffset);
                contact.HiggsAt[i] = glm::vec3{pAt[0], pAt[1], pAt[2]};
            }
        },
        [apEvent](const WeaponContact& contact)
        {
            std::lock_guard lock(s_contactsLock);
            if (GameExtension::HasTouchObservers())
            {
                static_assert(offsetof(WeaponContact, Point) == offsetof(UrGameTouch, Point));
                static_assert(offsetof(WeaponContact, HiggsAt) == offsetof(UrGameTouch, HeldAt));
                const auto current = std::bit_cast<UrGameTouch>(contact);
                const uint32_t hasPoint = *reinterpret_cast<const void* const*>(apEvent + kEventContactPointOffset) != nullptr;
                const auto admitted = GameExtension::SelectTouch(nullptr, &current, hasPoint);
                if (admitted == 2)
                    return;
                if (admitted == 1)
                {
                    if (contact.HiggsKind[0] || contact.HiggsKind[1])
                        s_recentTouches[s_nextRecentTouch++ % s_recentTouches.size()] = {contact.Point, std::chrono::steady_clock::now()};
                    for (size_t i = 0; i < s_contactCount; ++i)
                    {
                        const auto previous = std::bit_cast<UrGameTouch>(s_contacts[i]);
                        const auto choice = GameExtension::SelectTouch(&previous, &current, hasPoint);
                        if (choice == 3)
                            s_contacts[i] = contact;
                        if (choice == 2 || choice == 3)
                            return;
                    }
                    if (s_contactCount < s_contacts.size())
                        s_contacts[s_contactCount++] = contact;
                    return;
                }
            }
            if (contact.HiggsKind[0] != 0 || contact.HiggsKind[1] != 0)
                s_recentTouches[s_nextRecentTouch++ % s_recentTouches.size()] = {contact.Point, std::chrono::steady_clock::now()};
            WeaponContactRules::Record(s_contacts, s_contactCount, contact);
        });
}

void IgnoreContactEvent(void*, const void*) noexcept
{
}

// Slot 0 contactPointCallback, 1 collisionAddedCallback, 2 collisionRemovedCallback; the rest are never wanted here.
void* s_contactListenerVTable[8] = {reinterpret_cast<void*>(&OnWeaponContactPoint), reinterpret_cast<void*>(&IgnoreContactEvent), reinterpret_cast<void*>(&IgnoreContactEvent),
                                    reinterpret_cast<void*>(&IgnoreContactEvent),   reinterpret_cast<void*>(&IgnoreContactEvent), reinterpret_cast<void*>(&IgnoreContactEvent),
                                    reinterpret_cast<void*>(&IgnoreContactEvent),   reinterpret_cast<void*>(&IgnoreContactEvent)};
struct ContactListenerObject
{
    void** pVTable;
};
ContactListenerObject s_contactListener{s_contactListenerVTable};

// Where each weapon body should be: set at the frame end, read right before each Havok step, possibly on another thread.
// ReleaseWeaponBody removes a body from this list before taking it out of the world.
// The release and the step's drive both hold the world's write lock, so the drive never touches a body being released.
struct WeaponTarget
{
    alignas(16) float Position[4];
    alignas(16) float Rotation[4];
    // Where the last step was to leave it: where it should be when the next step begins.
    alignas(16) float StepPosition[4];
    alignas(16) float StepRotation[4];
    bool HasStep;
    uint8_t* pHavok;
    void* pBhkWorld; // checked when the body was added; the step's own world is matched against it
};
std::mutex s_targetsLock;
std::vector<WeaponTarget> s_targets;

void SetWeaponTarget(uint8_t* apHavok, void* apBhkWorld, const float* apPosition, const float* apRotation) noexcept
{
    std::lock_guard lock(s_targetsLock);
    auto it = std::find_if(s_targets.begin(), s_targets.end(), [apHavok](const WeaponTarget& acTarget) { return acTarget.pHavok == apHavok; });
    if (it == s_targets.end())
    {
        s_targets.push_back({});
        it = s_targets.end() - 1;
        it->pHavok = apHavok;
        it->HasStep = false;
    }
    it->pBhkWorld = apBhkWorld;
    std::copy_n(apPosition, 4, it->Position);
    std::copy_n(apRotation, 4, it->Rotation);
}

void ForgetWeaponTarget(const uint8_t* apHavok) noexcept
{
    std::lock_guard lock(s_targetsLock);
    s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(), [apHavok](const WeaponTarget& acTarget) { return acTarget.pHavok == apHavok; }), s_targets.end());
}

// Where the body must be for the weapon to sit where it is drawn, in Havok units.
// That is the node's world transform, times the body's own offset for a bhkRigidBodyT (as in HIGGS's GetRigidBodyTLocalTransform).
void WeaponBodyTarget(const WeaponBody& acBody, const float aHavokScale, float* apPosition, float* apRotation) noexcept
{
    const NiTransform& node = At<NiTransform>(acBody.pNode, kWorldOffset);
    glm::mat3 rotation = ToGlm(node.rotate);
    glm::vec3 position = ToGlm(node.translate);
    if (acBody.IsT)
    {
        const float* pLocalRotation = reinterpret_cast<const float*>(static_cast<uint8_t*>(acBody.pWrapper) + kRigidBodyTRotationOffset);
        const float* pLocalTranslation = reinterpret_cast<const float*>(static_cast<uint8_t*>(acBody.pWrapper) + kRigidBodyTTranslationOffset);
        const glm::quat localRotation{pLocalRotation[3], pLocalRotation[0], pLocalRotation[1], pLocalRotation[2]};
        const glm::vec3 localTranslation = glm::vec3{pLocalTranslation[0], pLocalTranslation[1], pLocalTranslation[2]} / aHavokScale;
        position += rotation * (localTranslation * node.scale);
        rotation = rotation * glm::mat3_cast(localRotation);
    }
    const glm::quat quaternion = glm::normalize(glm::quat_cast(rotation));
    position *= aHavokScale;
    apPosition[0] = position.x;
    apPosition[1] = position.y;
    apPosition[2] = position.z;
    apPosition[3] = 0.f;
    apRotation[0] = quaternion.x;
    apRotation[1] = quaternion.y;
    apRotation[2] = quaternion.z;
    apRotation[3] = quaternion.w;
}

// hkMotionState::transform's rotation: three hkVector4 columns.
glm::quat BodyRotation(const uint8_t* apHavok) noexcept
{
    const float* pColumns = reinterpret_cast<const float*>(apHavok + kBodyTransformOffset);
    const glm::mat3 rotation{glm::vec3{pColumns[0], pColumns[1], pColumns[2]}, glm::vec3{pColumns[4], pColumns[5], pColumns[6]}, glm::vec3{pColumns[8], pColumns[9], pColumns[10]}};
    return glm::normalize(glm::quat_cast(rotation));
}

float DegreesBetween(const glm::quat& acA, const glm::quat& acB) noexcept
{
    const float cosHalf = std::min(1.f, std::fabs(glm::dot(acA, acB)));
    return glm::degrees(2.f * std::acos(cosHalf));
}

glm::vec3 BodyPosition(const uint8_t* apHavok, const float aHavokScale) noexcept
{
    const float* pTranslation = reinterpret_cast<const float*>(apHavok + kBodyTransformOffset + 0x30);
    return glm::vec3{pTranslation[0], pTranslation[1], pTranslation[2]} / aHavokScale;
}

// Whether to log a weapon body going in or out of the world: at most every ten seconds per weapon and direction.
// A body that flickers between usable and not would otherwise log two lines a frame.
bool ShouldLogWeaponBody(const uint64_t aKey, const bool aIn) noexcept
{
    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_quietUntil;
    if (s_quietUntil.size() > 256)
        s_quietUntil.clear();
    const auto cNow = std::chrono::steady_clock::now();
    auto& quietUntil = s_quietUntil[aKey << 1 | (aIn ? 1 : 0)];
    if (cNow < quietUntil)
        return false;
    quietUntil = cNow + std::chrono::seconds(10);
    return true;
}

// False when the body is still in a world afterwards (its world was busy): the record is kept, to be tried again next frame.
// Dropping the record then would leave the body in the world with our listener, and the next frame takes it for someone else's.
bool ReleaseWeaponBody(const uint64_t aKey, WeaponBody& aBody, const char* acpWhy) noexcept
{
    if (!aBody.pHavok)
        return true;
    ForgetWeaponTarget(aBody.pHavok);
    POINTER_SKYRIMSE(THkpWorldRemoveEntity, s_removeEntity, 9000003, 9000003);
    POINTER_SKYRIMSE(THkpEntityContactListener, s_removeContactListener, 60095, 60095);
    void* pHkpWorld = At<void*>(aBody.pHavok, kHkWorldOffset);
    if (pHkpWorld && s_removeEntity.Get())
    {
        WorldWriteLock lock(BhkWorldOf(pHkpWorld));
        // Read again under the lock: the game may have taken it out (a cell unloading) in the meantime.
        if (lock.Held() && At<void*>(aBody.pHavok, kHkWorldOffset) == pHkpWorld)
        {
            if (aBody.Listening && s_removeContactListener.Get())
                s_removeContactListener.Get()(aBody.pHavok, &s_contactListener);
            aBody.Listening = false;
            aBody.pHavok[kMaxAngularVelocityOffset] = aBody.GameMaxAngularVelocity;
            bool removed = false;
            s_removeEntity.Get()(pHkpWorld, &removed, aBody.pHavok);
        }
    }
    if (At<void*>(aBody.pHavok, kHkWorldOffset))
    {
        static std::unordered_set<uint64_t> s_saidWaiting;
        if (s_saidWaiting.size() > 256)
            s_saidWaiting.clear();
        if (s_saidWaiting.insert(aKey).second)
            spdlog::info("VRWeaponBody: actor {:X} {} weapon body could not be taken out yet ({}; its world was busy); tried again next frame",
                         static_cast<uint32_t>(aKey >> 1), (aKey & 1) ? "right" : "left", acpWhy);
        return false;
    }
    // Out of every world already: nothing simulates it, so the listener comes off without a lock.
    if (aBody.Listening && s_removeContactListener.Get())
        s_removeContactListener.Get()(aBody.pHavok, &s_contactListener);
    aBody.Listening = false;
    aBody.pHavok[kMaxAngularVelocityOffset] = aBody.GameMaxAngularVelocity;
    if (ShouldLogWeaponBody(aKey, false))
        spdlog::info("VRWeaponBody: actor {:X} {} weapon body out of the world ({})", static_cast<uint32_t>(aKey >> 1), (aKey & 1) ? "right" : "left", acpWhy);
    static_cast<NiRefObject*>(aBody.pWrapper)->DecRef();
    static_cast<NiRefObject*>(aBody.pNode)->DecRef();
    aBody = {};
    return true;
}

// Whether a node hangs below another, up to six levels up.
// A drawn weapon's node sits two or three levels below the hand's "WEAPON" node; a sheathed one hangs off the sheath.
bool HangsUnder(void* apNode, void* apAncestor) noexcept
{
    void* pNode = apNode;
    for (int depth = 0; pNode && depth < 6; ++depth)
    {
        pNode = At<void*>(pNode, kParentOffset);
        if (pNode == apAncestor)
            return true;
    }
    return false;
}

// Adds the copy's drawn weapons to the Havok world and sets where the step drives them. Called right after the copy is posed.
void UpdateWeaponBodies(const uint32_t aFormId, Actor* apActor, const Rig& acRig) noexcept
{
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    POINTER_SKYRIMSE(THkpWorldAddEntity, s_addEntity, 9000001, 9000001);
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityContactListener, s_addContactListener, 60094, 60094);
    if (!s_stepDeltaTime || !s_havokScale.Get() || !s_addEntity.Get() || !s_setPositionAndRotation.Get())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            spdlog::error("VRWeaponBody: a Havok address did not resolve (step hook {}, scale {}, addEntity {}, setPositionAndRotation {}); weapons stay out of the world",
                          s_stepDeltaTime != nullptr, s_havokScale.Get() != nullptr, s_addEntity.Get() != nullptr, s_setPositionAndRotation.Get() != nullptr);
        }
        return;
    }
    const float havokScale = *s_havokScale.Get();
    if (!(havokScale > 0.f))
        return;

    // The copy's ragdoll says which world it is in; nothing is added without one.
    void* pCopyWorld = nullptr;
    for (const RigBone& bone : acRig.Body)
        if (uint8_t* pBone = bone.pNode ? HavokBodyOfLive(bone.pNode) : nullptr)
            if ((pCopyWorld = At<void*>(pBone, kHkWorldOffset)) != nullptr)
                break;

    // Out means weapon state 3 (drawn), 4 (asked to sheathe) or 5 (sheathing); the node hanging under the hand decides.
    // State 4 counts: a copy's own AI asks to sheathe, but its graph only follows its owner's actions and may never do it.
    const bool cOut = apActor->actorState.IsWeaponInHand();
    for (uint32_t side = 0; side < 2; ++side)
    {
        const uint64_t key = (static_cast<uint64_t>(aFormId) << 1) | side;
        void* pAttach = acRig.Attach[side].pNode;

        // The weapon's node and body as the rig has them now.
        void* pNode = nullptr;
        uint8_t* pHavok = nullptr;
        if (pAttach)
            for (const RigBone& below : acRig.AttachBelow[side])
                if (below.pNode && (pHavok = HavokBodyOfLive(below.pNode)) != nullptr)
                {
                    pNode = below.pNode;
                    break;
                }
        const bool cInHand = pNode && HangsUnder(pNode, pAttach);
        if (!cOut || !cInHand)
        {
            pNode = nullptr;
            pHavok = nullptr;
        }

        auto it = s_weaponBodies.find(key);
        if (it != s_weaponBodies.end())
        {
            WeaponBody& body = it->second;
            void* pBodyWorld = At<void*>(body.pHavok, kHkWorldOffset);
            const char* pWhy = !cOut                               ? "the weapon is not out"
                               : body.pHavok != pHavok             ? "another weapon or none in the hand"
                               : !pCopyWorld || pBodyWorld != pCopyWorld ? "the copy is in another world, or the game took the body out"
                                                                   : nullptr;
            if (pWhy)
            {
                if (!ReleaseWeaponBody(key, body, pWhy))
                    continue;
                s_weaponBodies.erase(it);
                it = s_weaponBodies.end();
            }
        }
        if (!pHavok || !pCopyWorld)
            continue;

        alignas(16) float position[4];
        alignas(16) float rotation[4];
        if (it == s_weaponBodies.end())
        {
            static std::unordered_set<uint64_t> s_saidBusy;
            if (s_saidBusy.size() > 256)
                s_saidBusy.clear();
            if (At<void*>(pHavok, kHkWorldOffset) != nullptr)
            {
                // Someone else put it in a world; it is theirs.
                if (s_saidBusy.insert(key).second)
                    spdlog::info("VRWeaponBody: actor {:X} {} weapon body is already in a world; left alone", aFormId, side ? "right" : "left");
                continue;
            }
            // A failed add is tried again after five seconds, not every frame: the checks below are slow ones.
            const auto cNowAdd = std::chrono::steady_clock::now();
            if (const auto retry = s_weaponBodyRetryAt.find(key); retry != s_weaponBodyRetryAt.end() && cNowAdd < retry->second)
                continue;
            s_weaponBodyRetryAt[key] = cNowAdd + std::chrono::seconds(5);
            void* pBhkWorld = BhkWorldOf(pCopyWorld);
            if (!pBhkWorld)
            {
                if (s_saidBusy.insert(key).second)
                    spdlog::warn("VRWeaponBody: actor {:X}: the world its ragdoll is in has no bhkWorld behind it; its weapon stays out", aFormId);
                continue;
            }

            WeaponBody body;
            body.pNode = pNode;
            body.pWrapper = At<void*>(At<void*>(pNode, kCollisionObjectOffset), kCollisionBodyOffset);
            body.pHavok = pHavok;
            body.pBhkWorld = pBhkWorld;
            const char* pWrapperClass = RttiNameOf(body.pWrapper);
            body.IsT = pWrapperClass && std::strcmp(pWrapperClass, "bhkRigidBodyT") == 0;
            if (!pWrapperClass || (std::strcmp(pWrapperClass, "bhkRigidBody") != 0 && !body.IsT) || !ReadableMemory::IsReadable(pHavok, kBodyQuaternionOffset + 0x10))
            {
                if (s_saidBusy.insert(key).second)
                    spdlog::warn("VRWeaponBody: actor {:X} {} weapon's body is a {}, not a rigid body; left out", aFormId, side ? "right" : "left", pWrapperClass ? pWrapperClass : "?");
                continue;
            }
            WeaponBodyTarget(body, havokScale, position, rotation);
            PendingWeaponBody pending{};
            pending.Key = key;
            pending.FormId = aFormId;
            pending.Side = side;
            pending.Body = body;
            pending.pWorld = pCopyWorld;
            std::copy_n(position, 4, pending.Position);
            std::copy_n(rotation, 4, pending.Rotation);
            static_cast<NiRefObject*>(body.pNode)->IncRef();
            static_cast<NiRefObject*>(body.pWrapper)->IncRef();
            s_pendingWeaponBodies.push_back(pending);
            continue;
        }

        // Only the target is set here; DriveWeaponBodiesBeforeStep drives the body right before Havok's step, as HIGGS does.
        // A velocity set here at the frame end is not the one Havok integrates, so the body would never settle.
        WeaponBody& body = it->second;
        body.Seen = true;
        WeaponBodyTarget(body, havokScale, position, rotation);
        SetWeaponTarget(body.pHavok, body.pBhkWorld, position, rotation);

        // Every 5 s: how far the body is from where the weapon is drawn, and where its centre of mass is held.
        // The rig tests read this line, and live-check aims its swings at that centre.
        static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextGapLog;
        if (s_nextGapLog.size() > 256)
            s_nextGapLog.clear();
        const auto cNow = std::chrono::steady_clock::now();
        if (auto& nextGapLog = s_nextGapLog[key]; cNow >= nextGapLog)
        {
            nextGapLog = cNow + std::chrono::seconds(5);
            const glm::vec3 wanted = glm::vec3{position[0], position[1], position[2]} / havokScale;
            const glm::quat target{rotation[3], rotation[0], rotation[1], rotation[2]};
            // Computed from the target: between steps the game moves the body to where its animation holds the sword.
            const float* pCentreLocal = reinterpret_cast<const float*>(body.pHavok + kBodyTransformOffset + 0x80);
            const glm::vec3 centre = wanted + glm::mat3_cast(target) * (glm::vec3{pCentreLocal[0], pCentreLocal[1], pCentreLocal[2]} / havokScale);
            spdlog::debug(
                "VRWeaponBody: actor {:X} {} weapon body {:.1f} units from where the weapon is drawn; its centre of mass at ({:.1f})", aFormId, side ? "right" : "left",
                glm::length(BodyPosition(body.pHavok, havokScale) - wanted), centre);
        }
    }
}

// Puts the weapon bodies of the last frame in the world, at the start of a frame, when no job is running.
// PLANCK answers an add with an SKSE task, and adding at the frame end could not get its lock: a running task that reads the world waits for this thread's world lock.
// A job can change the weapon after the frame end, so each is looked at again here, and the references taken then are the ones the body keeps.
void AddPendingBodies() noexcept
{
    if (s_pendingWeaponBodies.empty())
        return;

    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    POINTER_SKYRIMSE(THkpWorldAddEntity, s_addEntity, 9000001, 9000001);
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityContactListener, s_addContactListener, 60094, 60094);
    POINTER_SKYRIMSE(THkRealToUFloat8, s_toUFloat8, 9000010, 9000010);
    const float havokScale = *s_havokScale.Get();

    for (PendingWeaponBody& pending : s_pendingWeaponBodies)
    {
        WeaponBody& body = pending.Body;
        uint8_t* pHavok = body.pHavok;
        void* pCollision = At<void*>(body.pNode, kCollisionObjectOffset);
        const bool cSameBody = pCollision && At<void*>(pCollision, kCollisionBodyOffset) == body.pWrapper;
        bool added = false;
        if (cSameBody && BhkWorldOf(pending.pWorld) == body.pBhkWorld)
        {
            WorldWriteLock lock(body.pBhkWorld);
            if (!lock.Held() || At<void*>(pHavok, kHkWorldOffset) != nullptr)
            {
                static_cast<NiRefObject*>(body.pWrapper)->DecRef();
                static_cast<NiRefObject*>(body.pNode)->DecRef();
                continue;
            }
            s_setPositionAndRotation.Get()(pHavok, pending.Position, pending.Rotation);
            // The game caps its weapon body at 31.6 rad/s, slower than a swung sword, so the keyframe never catches up.
            // HIGGS gives its own weapon bodies 500 rad/s; so does this while the body is ours.
            body.GameMaxAngularVelocity = pHavok[kMaxAngularVelocityOffset];
            if (s_toUFloat8.Get())
            {
                constexpr float cMaxAngularVelocity = 500.f;
                s_toUFloat8.Get()(pHavok[kMaxAngularVelocityOffset], cMaxAngularVelocity);
            }
            s_addEntity.Get()(pending.pWorld, pHavok, 1); // HK_ENTITY_ACTIVATION_DO_ACTIVATE
            if (s_addContactListener.Get())
            {
                s_addContactListener.Get()(pHavok, &s_contactListener);
                body.Listening = true;
            }
            added = true;
        }
        if (!added)
        {
            static_cast<NiRefObject*>(body.pWrapper)->DecRef();
            static_cast<NiRefObject*>(body.pNode)->DecRef();
            continue;
        }
        body.Seen = true;
        s_weaponBodies[pending.Key] = body;
        s_weaponBodyRetryAt.erase(pending.Key);
        const uint32_t filter = At<uint32_t>(pHavok, kFilterInfoOffset);
        const char* pName = GetName(body.pNode);
        const char* pClass = RttiNameOf(body.pWrapper);
        // Read back from where Havok keeps it: 0 here says the transform is read at the right offset.
        const float placedOff = glm::length(BodyPosition(pHavok, havokScale) - glm::vec3{pending.Position[0], pending.Position[1], pending.Position[2]} / havokScale);
        if (ShouldLogWeaponBody(pending.Key, true))
            spdlog::info("VRWeaponBody: actor {:X} {} weapon '{}' ({}) is a body in the world now: layer {}, group {}, motion type {}; placed, it reads {:.1f} units from where it was put; "
                         "hkpRigidBody at {}",
                         pending.FormId, pending.Side ? "right" : "left", pName && ReadableMemory::IsReadable(pName, 1) ? pName : "?", pClass ? pClass : "?", filter & 0x7F, filter >> 16,
                         At<uint8_t>(pHavok, kMotionTypeOffset), placedOff, fmt::ptr(pHavok));
    }
    s_pendingWeaponBodies.clear();
}

// Right before Havok steps a world, wakes every weapon body in it and keyframes it to where its weapon was last drawn.
// The keyframe covers exactly the time of this step. Called from inside bhkWorld::Update.
// The write lock is taken only if free or already held by this thread: waiting could deadlock on this thread's own read lock.
// Otherwise this step goes undriven.
void DriveWeaponBodiesBeforeStep(void* apHkpWorld, const float aSeconds) noexcept
{
    if (!(aSeconds > 0.f))
        return;
    {
        std::lock_guard lock(s_targetsLock);
        if (s_targets.empty())
            return;
    }
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, 60096, 60096);
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_setPositionAndRotation.Get() || !s_activate.Get() || !s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return;

    // The world being stepped is alive; its bhkWorld is trusted only if one of our bodies was added to it (checked then).
    void* pBhkWorld = At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset);
    {
        std::lock_guard targetsLock(s_targetsLock);
        if (!pBhkWorld || std::none_of(s_targets.begin(), s_targets.end(), [pBhkWorld](const WeaponTarget& acTarget) { return acTarget.pBhkWorld == pBhkWorld; }))
            return;
    }
    const auto* pLock = reinterpret_cast<const volatile uint32_t*>(static_cast<uint8_t*>(pBhkWorld) + kWorldLockOffset); // BSReadWriteLock: writer thread, lock word
    if (pLock[0] != GetCurrentThreadId() && pLock[1] != 0)
        return;
    WorldWriteLock lock(pBhkWorld);
    if (!lock.Held())
        return;

    std::array<WeaponTarget, 8> targets;
    size_t count = 0;
    {
        std::lock_guard targetsLock(s_targetsLock);
        for (const WeaponTarget& target : s_targets)
            if (count < targets.size() && target.pBhkWorld == pBhkWorld && At<void*>(target.pHavok, kHkWorldOffset) == apHkpWorld)
                targets[count++] = target;
    }

    const float havokScale = *s_havokScale.Get();
    for (size_t i = 0; i < count; ++i)
    {
        WeaponTarget& target = targets[i];
        uint8_t* pHavok = target.pHavok;
        // Awake, as HIGGS keeps its own weapon bodies: a sleeping body ignores the velocity a keyframe gives it.
        s_activate.Get()(pHavok);
        const glm::mat3 targetRotation = glm::mat3_cast(glm::quat{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]});
        const glm::vec3 targetPosition{target.Position[0], target.Position[1], target.Position[2]};
        const float* pColumns = reinterpret_cast<const float*>(pHavok + kBodyTransformOffset);
        const glm::mat3 bodyRotation{glm::vec3{pColumns[0], pColumns[1], pColumns[2]}, glm::vec3{pColumns[4], pColumns[5], pColumns[6]},
                                     glm::vec3{pColumns[8], pColumns[9], pColumns[10]}};
        const glm::vec3 bodyPosition{pColumns[12], pColumns[13], pColumns[14]};

        // Further than a sword's length from the target is a jump (a teleport, a load), not a swing: placed, not swept.
        if (!target.HasStep || glm::length(bodyPosition - targetPosition) / havokScale > 100.f)
        {
            s_setPositionAndRotation.Get()(pHavok, target.Position, target.Rotation);
            float* pLinear = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset - 0x10);
            float* pAngular = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset);
            std::fill_n(pLinear, 4, 0.f);
            std::fill_n(pAngular, 4, 0.f);
        }
        else
        {
            // The step starts from where the last step left the body.
            // Between steps the game moves the body to where its animation holds the sword (bhkRigidBody setters, worker thread).
            // Swept from there, it would cut through everything nearby, so it is put back with no speed first.
            // Then it moves only as far as the drawn sword moved.
            const glm::mat3 startRotation = glm::mat3_cast(glm::quat{target.StepRotation[3], target.StepRotation[0], target.StepRotation[1], target.StepRotation[2]});
            const glm::vec3 startPosition{target.StepPosition[0], target.StepPosition[1], target.StepPosition[2]};
            glm::quat moved = glm::normalize(glm::quat_cast(bodyRotation * glm::transpose(startRotation)));
            const float movedDegrees = glm::degrees(glm::angle(moved.w < 0.f ? -moved : moved));
            const float movedUnits = glm::length(bodyPosition - startPosition) / havokScale;
            if (movedUnits > 1.f || movedDegrees > 3.f)
                s_setPositionAndRotation.Get()(pHavok, target.StepPosition, target.StepRotation);

            // The velocities that take it from there to the target over this step, computed from rotation matrices.
            // Passing the quaternion to applyHardKeyFrame instead leaves the body about 145 degrees off around one axis.
            glm::quat turn = glm::normalize(glm::quat_cast(targetRotation * glm::transpose(startRotation)));
            if (turn.w < 0.f)
                turn = -turn;
            const float angle = glm::angle(turn);
            const glm::vec3 spin = angle > 1e-5f ? glm::axis(turn) * (angle / aSeconds) : glm::vec3{};
            const float* pCentreLocal = reinterpret_cast<const float*>(pHavok + kBodyTransformOffset + 0x80);
            const glm::vec3 centreLocal{pCentreLocal[0], pCentreLocal[1], pCentreLocal[2]};
            const glm::vec3 linear = ((targetPosition + targetRotation * centreLocal) - (startPosition + startRotation * centreLocal)) / aSeconds;
            float* pLinear = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset - 0x10);
            float* pAngular = reinterpret_cast<float*>(pHavok + kAngularVelocityOffset);
            pLinear[0] = linear.x, pLinear[1] = linear.y, pLinear[2] = linear.z, pLinear[3] = 0.f;
            pAngular[0] = spin.x, pAngular[1] = spin.y, pAngular[2] = spin.z, pAngular[3] = 0.f;
        }

        // This step leaves it at the target.
        {
            std::lock_guard targetsLock(s_targetsLock);
            for (WeaponTarget& stored : s_targets)
                if (stored.pHavok == pHavok)
                {
                    std::copy_n(target.Position, 4, stored.StepPosition);
                    std::copy_n(target.Rotation, 4, stored.StepRotation);
                    stored.HasStep = true;
                }
        }
    }
}

// Replaces the call to hkpWorld::stepDeltaTime in bhkWorld::Update, VR 0xDFB722, only if that instruction is a call.
// HIGGS hooks the same call (src/hooks.cpp, prePhysicsStepHookLoc); whichever patches second calls the other.
int HookStepDeltaTime(void* apHkpWorld, float aSeconds)
{
    DriveWeaponBodiesBeforeStep(apHkpWorld, aSeconds);
    return s_stepDeltaTime(apHkpWorld, aSeconds);
}

#ifdef SKYRIMVR
TiltedPhoques::Initializer s_stepHook(
    []()
    {
        VersionDbPtr<uint8_t> callSite(9000009);
        uint8_t* pCall = callSite.Get();
        if (!pCall || *pCall != 0xE8)
        {
            spdlog::error("VRWeaponBody: the call to Havok's step is not where it was expected; weapons stay out of the world");
            return;
        }
        TiltedPhoques::SwapCall(pCall, s_stepDeltaTime, &HookStepDeltaTime);
    });
#endif

// Drives a remote player's copy's ragdoll toward its drawn pose instead of its animation.
// The ragdoll bodies are what everything collides with, and the animation pose keeps them far from the drawn hands.
// The graph hands the ragdoll driver its pose in the generator output's pose track, just before hkbRagdollDriver::driveToPose.
// PLANCK hooks the same call to steer its active ragdolls; this hook writes the copy's last drawn pose into that track.
//
// The frame end records the drawn world transform of every animation bone that has a node of the same name.
// The hook turns those into the track's parent-relative transforms, parents first; bones with no node keep the animation's.
// Units are measured once per copy (the track's model-to-world against the copy's position), never assumed.
// If anything is unexpected, the track is left as the game made it.
constexpr uint32_t kDriverCharacterOffset = 0x80;     // hkbRagdollDriver::character (PLANCK's static_assert)
constexpr uint32_t kCharacterSetupOffset = 0x50;      // hkbCharacter::setup
constexpr uint32_t kCharacterWorldFromModel = 0x88;   // hkbCharacter::worldFromModel (hkQsTransform*)
constexpr uint32_t kSetupAnimationSkeleton = 0x20;    // hkbCharacterSetup::m_animationSkeleton
constexpr uint32_t kSkeletonParents = 0x18;           // hkaSkeleton::parentIndices (hkArray<int16>: data, size)
constexpr uint32_t kSkeletonBones = 0x28;             // hkaSkeleton::bones (hkArray<hkaBone>, 0x10 each, name first)
constexpr uint32_t kQsTransformSize = 0x30;           // hkQsTransform: translation, rotation (x, y, z, w), scale

struct DrawnPose
{
    std::vector<NiTransform> World;  // by animation bone
    std::vector<uint8_t> Has;        // 1 where the bone has a drawn node
    glm::vec3 ActorAt{};             // the copy's own position (the game's), for the units
    uint32_t FormId = 0;
    std::chrono::steady_clock::time_point At{};
};
std::mutex s_drawnPosesLock;
std::unordered_map<const void*, DrawnPose> s_drawnPoses; // by hkbCharacter
std::atomic<uint32_t> s_drawnPoseCount{0};
// The original target of the hooked hkbRagdollDriver::postPhysics call, saved by HookPostPhysicsOnce; null until then.
using TPostPhysics = bool(void* apDriver, const void* apContext, void* apInOut);
TPostPhysics* s_postPhysicsNext = nullptr;

// Per copy, on the frame-end thread only: which hkbCharacter is its, and the node of each of its animation bones.
struct CharacterLink
{
    void* pRoot = nullptr;
    void* pCharacter = nullptr;
    std::vector<void*> BoneNodes;
};
std::unordered_map<uint32_t, CharacterLink> s_characterLinks;

void* CharacterOf(Actor* apActor) noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    void* pCharacter = nullptr;
    if (apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) && pManager)
    {
        const uint32_t index = pManager->ResolveGraphIndex();
        if (index < pManager->animationGraphs.size)
            if (BShkbAnimationGraph* pGraph = pManager->animationGraphs.Get(index))
                pCharacter = &pGraph->character;
    }
    if (pManager)
        pManager->Release();
    return pCharacter;
}

void HookPostPhysicsOnce() noexcept;

void RecordDrawnPose(const uint32_t aFormId, Actor* apActor, const Rig& acRig) noexcept
{
#ifdef SKYRIMVR
    HookPostPhysicsOnce();
    if (!s_postPhysicsNext)
        return;
#else
    return;
#endif
    CharacterLink& link = s_characterLinks[aFormId];
    if (link.pRoot != acRig.pRoot || !link.pCharacter)
    {
        // Once per 3D load: the lookups search the skeleton for each bone, too slow for every frame.
        link = {};
        link.pRoot = acRig.pRoot;
        link.pCharacter = CharacterOf(apActor);
        void* pSetup = link.pCharacter ? At<void*>(link.pCharacter, kCharacterSetupOffset) : nullptr;
        void* pSkeleton = pSetup ? At<void*>(pSetup, kSetupAnimationSkeleton) : nullptr;
        if (!pSkeleton || !ReadableMemory::IsReadable(static_cast<uint8_t*>(pSkeleton) + kSkeletonBones, 0x10))
        {
            link.pCharacter = nullptr;
            return;
        }
        const int32_t count = At<int32_t>(pSkeleton, kSkeletonBones + 0x8);
        const uint8_t* pBones = At<uint8_t*>(pSkeleton, kSkeletonBones);
        if (count <= 0 || count > 512 || !pBones || !ReadableMemory::IsReadable(pBones, static_cast<size_t>(count) * 0x10))
        {
            link.pCharacter = nullptr;
            return;
        }
        link.BoneNodes.assign(count, nullptr);
        for (int32_t i = 0; i < count; ++i)
        {
            const auto* pName = reinterpret_cast<const char*>(*reinterpret_cast<const uintptr_t*>(pBones + i * 0x10) & ~uintptr_t{1});
            if (pName && ReadableMemory::IsReadable(pName, 2))
                link.BoneNodes[i] = FindShallowest(acRig.pRoot, pName);
        }
    }

    DrawnPose pose;
    pose.World.resize(link.BoneNodes.size());
    pose.Has.resize(link.BoneNodes.size());
    for (size_t i = 0; i < link.BoneNodes.size(); ++i)
        if (void* pNode = link.BoneNodes[i])
        {
            pose.World[i] = At<NiTransform>(pNode, kWorldOffset);
            pose.Has[i] = 1;
        }
    pose.ActorAt = glm::vec3{apActor->position.x, apActor->position.y, apActor->position.z};
    pose.FormId = aFormId;
    const auto cNow = std::chrono::steady_clock::now();
    pose.At = cNow;

    std::lock_guard lock(s_drawnPosesLock);
    s_drawnPoses[link.pCharacter] = std::move(pose);
    // Drops entries older than 2 s, left by copies no longer posed.
    for (auto it = s_drawnPoses.begin(); it != s_drawnPoses.end();)
        it = cNow - it->second.At > std::chrono::seconds(2) ? s_drawnPoses.erase(it) : std::next(it);
    s_drawnPoseCount = static_cast<uint32_t>(s_drawnPoses.size());
}

struct QsPose
{
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 Translation{};
    float Scale = 1.f;
};

QsPose Compose(const QsPose& acParent, const QsPose& acLocal) noexcept
{
    return QsPose{acParent.Rotation * acLocal.Rotation, acParent.Translation + acParent.Rotation * (acLocal.Translation * acParent.Scale), acParent.Scale * acLocal.Scale};
}

QsPose Inverse(const QsPose& acPose) noexcept
{
    const glm::quat inverse = glm::inverse(acPose.Rotation);
    const float inverseScale = acPose.Scale != 0.f ? 1.f / acPose.Scale : 1.f;
    return QsPose{inverse, inverse * (-acPose.Translation) * inverseScale, inverseScale};
}

QsPose ReadQs(const float* apQs) noexcept
{
    return QsPose{glm::normalize(glm::quat{apQs[7], apQs[4], apQs[5], apQs[6]}), glm::vec3{apQs[0], apQs[1], apQs[2]}, apQs[8]};
}

// The generator output's pose track (track 2) when it is dense hkQsTransforms, one per bone; null otherwise.
float* PoseTrackOf(void* apGeneratorOutput, int16_t& aCount) noexcept
{
    aCount = 0;
    auto* pTracks = apGeneratorOutput ? At<uint8_t*>(apGeneratorOutput, 0) : nullptr;
    if (!pTracks || At<int32_t>(pTracks, 0x4) <= 2)
        return nullptr;
    const uint8_t* pHeader = pTracks + 0x10 + 2 * 0x10;
    const int16_t numData = *reinterpret_cast<const int16_t*>(pHeader + 0x2);
    const int16_t dataOffset = *reinterpret_cast<const int16_t*>(pHeader + 0x4);
    const int16_t elementSize = *reinterpret_cast<const int16_t*>(pHeader + 0x6);
    const int8_t flags = *reinterpret_cast<const int8_t*>(pHeader + 0xC);
    const int8_t type = *reinterpret_cast<const int8_t*>(pHeader + 0xD);
    if (type != 1 || (flags & 0x6) || elementSize != kQsTransformSize || numData <= 0 || numData > 512)
        return nullptr;
    aCount = numData;
    return reinterpret_cast<float*>(pTracks + dataOffset);
}

// Whether this ragdoll driver's character is a copy whose drawn pose is being fed to its ragdoll.
bool IsDrivenCharacter(const void* apDriver) noexcept
{
    if (s_drawnPoseCount == 0 || !apDriver)
        return false;
    const void* pCharacter = At<void*>(const_cast<void*>(apDriver), kDriverCharacterOffset);
    std::lock_guard lock(s_drawnPosesLock);
    const auto it = s_drawnPoses.find(pCharacter);
    return it != s_drawnPoses.end() && std::chrono::steady_clock::now() - it->second.At <= std::chrono::milliseconds(250);
}

// Puts the drawn pose in the track; what was there before goes into aSaved, to be put back once the ragdoll is driven.
bool PutDrawnPoseIntoTrack(void* apDriver, void* apGeneratorOutput, std::vector<uint8_t>& aSaved) noexcept
{
    if (s_drawnPoseCount == 0 || !apDriver || !apGeneratorOutput)
        return false;
    void* pCharacter = At<void*>(apDriver, kDriverCharacterOffset);
    if (!pCharacter)
        return false;
    DrawnPose drawn;
    {
        std::lock_guard lock(s_drawnPosesLock);
        const auto it = s_drawnPoses.find(pCharacter);
        if (it == s_drawnPoses.end() || std::chrono::steady_clock::now() - it->second.At > std::chrono::milliseconds(250))
            return false;
        drawn = it->second;
    }

    // The tracks: the pose (2) must be dense hkQsTransforms, one per animation bone; model-to-world (0) as well.
    auto* pTracks = At<uint8_t*>(apGeneratorOutput, 0);
    if (!pTracks || At<int32_t>(pTracks, 0x4) <= 2)
        return false;
    const auto header = [pTracks](const int aTrack) { return pTracks + 0x10 + aTrack * 0x10; };
    const uint8_t* pPoseHeader = header(2);
    const int16_t numData = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x2);
    const int16_t dataOffset = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x4);
    const int16_t elementSize = *reinterpret_cast<const int16_t*>(pPoseHeader + 0x6);
    const float onFraction = *reinterpret_cast<const float*>(pPoseHeader + 0x8);
    const int8_t flags = *reinterpret_cast<const int8_t*>(pPoseHeader + 0xC);
    const int8_t type = *reinterpret_cast<const int8_t*>(pPoseHeader + 0xD);
    void* pSetup = At<void*>(pCharacter, kCharacterSetupOffset);
    void* pSkeleton = pSetup ? At<void*>(pSetup, kSetupAnimationSkeleton) : nullptr;
    if (!pSkeleton || type != 1 || (flags & 0x6) || elementSize != kQsTransformSize || !(onFraction > 0.f) || numData != static_cast<int16_t>(drawn.World.size()) ||
        At<int32_t>(pSkeleton, kSkeletonParents + 0x8) != numData)
        return false;
    auto* pPose = reinterpret_cast<float*>(pTracks + dataOffset);
    const auto* pParents = At<const int16_t*>(pSkeleton, kSkeletonParents);

    const uint8_t* pWorldHeader = header(0);
    const float* pWorldFromModel = nullptr;
    if (*reinterpret_cast<const int8_t*>(pWorldHeader + 0xD) == 1 && *reinterpret_cast<const float*>(pWorldHeader + 0x8) > 0.f)
        pWorldFromModel = reinterpret_cast<const float*>(pTracks + *reinterpret_cast<const int16_t*>(pWorldHeader + 0x4));
    else
        pWorldFromModel = At<const float*>(pCharacter, kCharacterWorldFromModel);
    if (!pWorldFromModel || !pParents)
        return false;
    const QsPose worldFromModel = ReadQs(pWorldFromModel);

    // The track's units (game or Havok), found by comparing where it has the copy with where the game does.
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    const float havokScale = s_havokScale.Get() ? *s_havokScale.Get() : 0.f;
    float scale = 0.f;
    if (glm::length(worldFromModel.Translation - drawn.ActorAt) < 64.f)
        scale = 1.f;
    else if (havokScale > 0.f && glm::length(worldFromModel.Translation / havokScale - drawn.ActorAt) < 64.f)
        scale = havokScale;
    static std::unordered_set<uint32_t> s_saidUnits;
    if (s_saidUnits.size() > 256)
        s_saidUnits.clear();
    if (scale == 0.f)
    {
        if (s_saidUnits.insert(drawn.FormId).second)
            spdlog::debug(
                "VRRagdoll: actor {:X}: its ragdoll's model-to-world ({:.1f}) is not where the copy is ({:.1f}) in either unit; "
                "left to its animation",
                drawn.FormId, worldFromModel.Translation, drawn.ActorAt);
        return false;
    }

    aSaved.assign(reinterpret_cast<const uint8_t*>(pPose), reinterpret_cast<const uint8_t*>(pPose) + static_cast<size_t>(numData) * kQsTransformSize);
    const QsPose modelFromWorld = Inverse(worldFromModel);
    std::vector<QsPose> model(numData);
    for (int16_t i = 0; i < numData; ++i)
    {
        float* pLocal = pPose + i * (kQsTransformSize / sizeof(float));
        const int16_t parent = pParents[i];
        const QsPose parentModel = parent >= 0 && parent < i ? model[parent] : QsPose{};
        if (!drawn.Has[i])
        {
            model[i] = Compose(parentModel, ReadQs(pLocal));
            continue;
        }
        const NiTransform& world = drawn.World[i];
        QsPose drawnWorld{glm::normalize(glm::quat_cast(ToGlm(world.rotate))), ToGlm(world.translate) * scale, world.scale};
        QsPose target = Compose(modelFromWorld, drawnWorld);
        const QsPose local = Compose(Inverse(parentModel), target);
        if (!IsFiniteVector(local.Translation) || !std::isfinite(local.Rotation.w))
        {
            model[i] = Compose(parentModel, ReadQs(pLocal));
            continue;
        }
        pLocal[0] = local.Translation.x, pLocal[1] = local.Translation.y, pLocal[2] = local.Translation.z;
        pLocal[4] = local.Rotation.x, pLocal[5] = local.Rotation.y, pLocal[6] = local.Rotation.z, pLocal[7] = local.Rotation.w;
        // Scale stays the animation's.
        target.Scale = parentModel.Scale * pLocal[8];
        model[i] = target;
    }
    return true;
}

// The call to hkbRagdollDriver::driveToPose, VR 0xB266AB (PLANCK's driveToPoseHookLoc).
// PLANCK hooks the same call; whichever of the two patches second calls the other. Replaced only if it is a call.
using TDriveToPose = void(void* apDriver, float aDeltaTime, const void* apContext, void* apGeneratorOutput);
TDriveToPose* s_driveToPose = nullptr;

// The track is also what the game poses the copy's skeleton from, so it holds the drawn pose only while the ragdoll is driven.
// Left in, the drawn pose would feed into itself and push the copy's hands off its owner's (live-weapon-grip).
void HookDriveToPose(void* apDriver, float aDeltaTime, const void* apContext, void* apGeneratorOutput)
{
    thread_local std::vector<uint8_t> t_saved;
    const bool cWrote = PutDrawnPoseIntoTrack(apDriver, apGeneratorOutput, t_saved);
    s_driveToPose(apDriver, aDeltaTime, apContext, apGeneratorOutput);
    if (cWrote)
    {
        int16_t count = 0;
        if (float* pPose = PoseTrackOf(apGeneratorOutput, count); pPose && static_cast<size_t>(count) * kQsTransformSize == t_saved.size())
            std::memcpy(pPose, t_saved.data(), t_saved.size());
    }
}

// After the step the game writes the ragdoll's pose back into the animation, and PLANCK blends it in.
// For a driven copy the animation is put back as it was, so the ragdoll never moves what is drawn.
// This hook wraps PLANCK's on that call (installed once the game runs), so PLANCK's blend is undone too.

bool HookPostPhysicsOutside(void* apDriver, const void* apContext, void* apInOut)
{
    if (!IsDrivenCharacter(apDriver))
        return s_postPhysicsNext(apDriver, apContext, apInOut);
    thread_local std::vector<uint8_t> t_saved;
    int16_t count = 0;
    float* pPose = PoseTrackOf(apInOut, count);
    if (pPose)
        t_saved.assign(reinterpret_cast<const uint8_t*>(pPose), reinterpret_cast<const uint8_t*>(pPose) + static_cast<size_t>(count) * kQsTransformSize);
    const bool cResult = s_postPhysicsNext(apDriver, apContext, apInOut);
    int16_t countAfter = 0;
    if (pPose && PoseTrackOf(apInOut, countAfter) == pPose && countAfter == count)
        std::memcpy(pPose, t_saved.data(), t_saved.size());
    return cResult;
}

// Once the game runs (every plugin loaded), routes the call to hkbRagdollDriver::postPhysics through the hook above.
// VR 0xB268DC (PLANCK's postPhysicsHookLoc). Only if it is a call.
void HookPostPhysicsOnce() noexcept
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    VersionDbPtr<uint8_t> callSite(9000012);
    uint8_t* pCall = callSite.Get();
    if (!pCall || *pCall != 0xE8)
    {
        spdlog::error("VRRagdoll: the call to postPhysics is not where it was expected; the drawn pose is not fed to ragdolls");
        s_drawnPoseCount = 0;
        return;
    }
    s_postPhysicsNext = TiltedPhoques::GetCall<TPostPhysics*>(pCall);
    TiltedPhoques::PutCall(pCall, &HookPostPhysicsOutside);
}

#ifdef SKYRIMVR
// Installed before the plugins, so PLANCK's hook on the same call wraps ours: ours writes the drawn pose after PLANCK's work.
// The other way round, PLANCK copies the animation pose over the track and the ragdoll hands end up far from the drawn ones.
TiltedPhoques::Initializer s_driveToPoseHook(
    []()
    {
        VersionDbPtr<uint8_t> callSite(9000011);
        uint8_t* pCall = callSite.Get();
        if (!pCall || *pCall != 0xE8)
        {
            spdlog::error("VRRagdoll: the call to driveToPose is not where it was expected; ragdolls follow their animation");
            return;
        }
        TiltedPhoques::SwapCall(pCall, s_driveToPose, &HookDriveToPose);
    });
#endif

// Handles what the weapon bodies touched since the last frame end: queues clashes, and logs each touched body once per ten seconds.
void ProcessWeaponContacts() noexcept
{
    std::array<WeaponContact, 64> contacts;
    size_t count = 0;
    {
        std::lock_guard lock(s_contactsLock);
        count = s_contactCount;
        std::copy_n(s_contacts.begin(), count, contacts.begin());
        s_contactCount = 0;
    }
    if (count == 0)
        return;

    const float havokScale = HavokLayout::HavokWorldScale();
    static std::unordered_map<const void*, std::chrono::steady_clock::time_point> s_lastSaid; // by the other body
    if (s_lastSaid.size() > 256)
        s_lastSaid.clear();
    static uint64_t s_total = 0;
    const auto cNow = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
    {
        const WeaponContact& contact = contacts[i];
        // Which of the two is the weapon: the listener is only on weapon bodies, but both could be.
        uint64_t key = 0;
        size_t ours = 2;
        for (const auto& [bodyKey, body] : s_weaponBodies)
            for (size_t side = 0; side < 2 && ours == 2; ++side)
                if (contact.pBodies[side] == body.pHavok)
                {
                    key = bodyKey;
                    ours = side;
                }
        if (ours == 2)
            continue;
        ++s_total;
        const size_t other = 1 - ours;
        // Touched by one of the local player's HIGGS bodies, or by something one holds.
        // A clash needs a quarter second with nothing of the player's on this weapon before it, and comes at most once a quarter second.
        // Counted per weapon, not per pair of bodies, so a hand resting on the blade or briefly lifting off does not clash again.
        if (contact.HiggsKind[other] != 0 && havokScale > 0.f)
        {
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_lastTouch;
            static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextClash;
            if (s_lastTouch.size() > 256)
                s_lastTouch.clear();
            if (s_nextClash.size() > 256)
                s_nextClash.clear();
            auto& lastTouch = s_lastTouch[key];
            const bool cStarts = cNow - lastTouch >= std::chrono::milliseconds(250);
            lastTouch = cNow;
            auto& nextClash = s_nextClash[key];
            // A clash must also be fast enough to be a blow; a slower meeting is a touch.
            // A resting hand brushes a blade at 0-5 units a second, while real blows are almost all faster than 20.
            constexpr float cMeetingSpeed = 10.f;
            const float speed = glm::length(contact.Velocities[0] - contact.Velocities[1]) / havokScale;
            if (cStarts && speed >= cMeetingSpeed && cNow >= nextClash)
            {
                nextClash = cNow + std::chrono::milliseconds(250);
                std::lock_guard clashesLock(s_clashesLock);
                if (s_clashes.size() < 16)
                    s_clashes.push_back({static_cast<uint32_t>(key >> 1), static_cast<uint8_t>(key & 1), contact.Point / havokScale, speed, contact.HiggsAt[other] / havokScale, true,
                                         contact.HiggsKind[other] == 2});
            }
            else
            {
                // Still touching: a blade resting on the other one blocks it all the same (the defender's rule).
                // Queued every tenth of a second, with no pulse, no sound and no network message.
                static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> s_nextTouch;
                if (s_nextTouch.size() > 256)
                    s_nextTouch.clear();
                auto& nextTouch = s_nextTouch[key];
                if (cNow >= nextTouch)
                {
                    nextTouch = cNow + std::chrono::milliseconds(100);
                    std::lock_guard clashesLock(s_clashesLock);
                    if (s_clashes.size() < 16)
                        s_clashes.push_back({static_cast<uint32_t>(key >> 1), static_cast<uint8_t>(key & 1), contact.Point / havokScale, 0.f, contact.HiggsAt[other] / havokScale, false,
                                             contact.HiggsKind[other] == 2});
                }
            }
        }
        auto& lastSaid = s_lastSaid[contact.pBodies[other]];
        if (cNow - lastSaid < std::chrono::seconds(10))
            continue;
        lastSaid = cNow;
        const glm::vec3 point = havokScale > 0.f ? contact.Point / havokScale : glm::vec3{};
        spdlog::debug(
            "VRWeaponBody: actor {:X} {} weapon touches a body on layer {} (group {}, motion type {}{}) at ({:.1f}); {} contacts so far", static_cast<uint32_t>(key >> 1),
            (key & 1) ? "right" : "left", contact.Filters[other] & 0x7F, contact.Filters[other] >> 16, contact.MotionTypes[other],
            contact.HiggsKind[other] == 1   ? ", one of the player's HIGGS bodies"
            : contact.HiggsKind[other] == 2 ? ", held by the player's HIGGS hand"
                                            : "",
            point, s_total);
    }
}

// Bodies of copies not posed at this frame end leave the world.
void ReleaseUnseenWeaponBodies() noexcept
{
    ProcessWeaponContacts();
    for (auto it = s_weaponBodies.begin(); it != s_weaponBodies.end();)
    {
        if (!it->second.Seen)
        {
            if (ReleaseWeaponBody(it->first, it->second, "its copy was not posed"))
            {
                it = s_weaponBodies.erase(it);
                continue;
            }
        }
        it->second.Seen = false;
        ++it;
    }
}

TiltedPhoques::Vector<uint32_t> s_touchCandidates;

// When the owner's bones last moved a dead body here (ApplyRemotePoses). See IsRemoteBodyGrabbedHere.
// Written at the frame's end and read from the world's update, which can be different threads.
std::mutex s_bodyPosedLock;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_bodyPosedByOwnerAt;

std::optional<std::chrono::steady_clock::time_point> BodyPosedAt(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_bodyPosedLock);
    const auto cPosed = s_bodyPosedByOwnerAt.find(aFormId);
    return cPosed != s_bodyPosedByOwnerAt.end() ? std::optional(cPosed->second) : std::nullopt;
}

// Where PlaceCorpse last put each copy's body, and how its moves went.
struct PlacedCorpse
{
    std::chrono::steady_clock::time_point NextAt{};
    std::chrono::steady_clock::time_point MovedAt{};
    bool SayAfter = false;
    glm::vec3 WantedAt{};
    float ApartBeforeMove = -1.f; // how far it lay when last moved for this place; -1: not moved yet
    uint32_t Fruitless = 0;
    uint32_t Streak = 0; // moves in a row
    bool SaidInView = false;
    std::chrono::steady_clock::time_point PosedAt{}; // the end of the owner's last drag this has seen
    // Who the copy is shown for, as its interpolation last said (each frame it runs); a change starts this record afresh.
    CopyBodyRules::CopyAuthority Authority{};
    std::chrono::steady_clock::time_point AuthoritySeenAt{};
    // When its owner's bones last arrived: a move queued before that is canceled.
    std::chrono::steady_clock::time_point BonesSeenAt{};
    // The owner's last pose of this dead body while its bones came, and its one-shot settle into it (per stream).
    VRPose OwnerPose{};
    bool HasOwnerPose = false;
    CopyBodyRules::OneShot Settle{};
    // A knocked copy put where its owner's NPC fell: once per fall (Fall, from its interpolation).
    CopyBodyRules::OneShot KnockPlace{};
    uint64_t Fall = 0;
    // A plain move queued and not run yet (flat).
    bool MovePending = false;
    // A correction blended in view (CopyBodyRules::BlendSteps): the steps made of how many; 0 steps when none runs.
    uint32_t BlendStep = 0;
    uint32_t BlendSteps = 0;
    // TEMPORARY, for the CorpseFall line: when this body was first measured, how far below its owner's corpse it was then, and whether its fall was logged.
    std::chrono::steady_clock::time_point FirstSeenAt{};
    float FirstDepth = 0.f;
    bool SaidFallen = false;
};
std::unordered_map<uint32_t, PlacedCorpse> s_placedCorpses;
// The world's update decides moves and the start of the next frame makes them; both on the main thread, but a load may not be.
std::mutex s_placedCorpsesLock;
// The NPCs whose deaths this game reported, and when (NoteDeathHere); CaptureBodyPose takes each once.
std::mutex s_diedHereLock;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_diedHere;

/**
 * Measure how close the nearer of the player's hands is to any node of a body.
 * Every node counts, not only the root: a dragon is dragged by a wing or the tail, far from its root.
 *
 * @param apBodyRoot The body's 3D root.
 * @return The distance in units; very large when the body or the hands are not known.
 */
float NearestHandToBody(void* apBodyRoot) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    Holding mine;
    if (!apBodyRoot || !pPlayer || !ResolveHolding(pPlayer, mine) || (!mine.RightHand.Valid && !mine.LeftHand.Valid))
        return 1e9f;

    float nearest = 1e9f;
    VisitTree(apBodyRoot, 1024, [&](void*, void* pNode) {
        if (!pNode)
            return;
        const glm::vec3 at = ToGlm(At<NiTransform>(pNode, kWorldOffset).translate);
        const Segment point{at, at, true};
        for (const Segment* pHand : {&mine.RightHand, &mine.LeftHand})
            if (pHand->Valid)
                nearest = std::min(nearest, SegmentDistance(*pHand, point));
    });
    return nearest;
}

// ---------------------------------------------------------------------------------------------------------------
// A copy's ragdoll moved to where its owner's body lies (PlaceCorpse), on both builds.
// VR sets each body through hkpEntity's own function; flat through its bhkRigidBody wrapper's virtual, which needs no address.

#ifndef SKYRIMVR
// bhkRigidBody::SetPositionAndRotation(hkVector4&, hkQuaternion&) (CommonLibSSE-NG bhkRigidBody.h), Havok units.
constexpr uint32_t kBhkRigidBodySetPositionAndRotationSlot = 0x37; // AE vtable slot, checked on SkyrimSE.exe 1.7.104
// hkpWorld::shouldActivateOnRigidBodyTransformChange (CommonLibSSE-NG hkpWorld.h, checked on 1.7.104): the set honours it and wakes the body.
constexpr uint32_t kActivateOnTransformChangeOffset = 0x156;
using TBhkRigidBodySetPositionAndRotation = void(void* apWrapper, const float* apPosition, const float* apRotation);
#endif

// One of a ragdoll's rigid bodies: the node it hangs on, its wrapper (bhkRigidBody) and the hkpRigidBody inside it.
struct RagdollBody
{
    void* pNode = nullptr;
    void* pWrapper = nullptr;
    uint8_t* pHavok = nullptr;
};

// The ragdoll's bodies under a 3D root, and the Havok world they are in; null when it has none.
// A live tree, so only null checks, no IsReadable (it costs milliseconds per call here).
// Only dynamic bodies in a world count; the first found from the root is the ragdoll's root (pelvis or centre of mass).
void* CollectRagdoll(void* apRoot, TiltedPhoques::Vector<RagdollBody>& aBodies) noexcept
{
    aBodies.clear();
    void* pHkpWorld = nullptr;
    VisitTree(
        apRoot, 512,
        [&](void* apObject)
        {
            if (uint8_t* pBody = HavokBodyOfLive(apObject))
            {
                const uint8_t motionType = pBody[kMotionTypeOffset];
                void* pWorld = At<void*>(pBody, kHkWorldOffset);
                if (pWorld && ((motionType >= 1 && motionType <= 3) || motionType == 6) && (!pHkpWorld || pWorld == pHkpWorld) && aBodies.size() < 64)
                {
                    pHkpWorld = pWorld;
                    aBodies.push_back(RagdollBody{apObject, At<void*>(At<void*>(apObject, kCollisionObjectOffset), kCollisionBodyOffset), pBody});
                }
            }
            return true;
        });
    return aBodies.empty() ? nullptr : pHkpWorld;
}

// Where a body is now, in game units, and how it is turned.
glm::vec3 RagdollBodyPosition(const uint8_t* apHavok, const float aScale) noexcept
{
    const float* pAt = reinterpret_cast<const float*>(apHavok + kBodyTranslationOffset);
    return glm::vec3{pAt[0], pAt[1], pAt[2]} / aScale;
}

glm::quat RagdollBodyRotation(const uint8_t* apHavok) noexcept
{
    const float* pQuaternion = reinterpret_cast<const float*>(apHavok + kBodyQuaternionOffset);
    return glm::quat{pQuaternion[3], pQuaternion[0], pQuaternion[1], pQuaternion[2]};
}

// Where one body goes, in Havok units; both 16-byte aligned, as Havok reads them with aligned loads (movaps).
struct BodyTarget
{
    alignas(16) float Position[4]{};
    alignas(16) float Rotation[4]{};
};

// Each body moved by one offset (game units), turned as it is.
void OffsetTargets(const TiltedPhoques::Vector<RagdollBody>& acBodies, const glm::vec3& acOffset, const float aScale, std::vector<BodyTarget>& aOut) noexcept
{
    aOut.assign(acBodies.size(), BodyTarget{});
    for (size_t i = 0; i < acBodies.size(); ++i)
    {
        const float* pAt = reinterpret_cast<const float*>(acBodies[i].pHavok + kBodyTranslationOffset);
        for (int axis = 0; axis < 3; ++axis)
            aOut[i].Position[axis] = pAt[axis] + acOffset[axis] * aScale;
        std::memcpy(aOut[i].Rotation, acBodies[i].pHavok + kBodyQuaternionOffset, sizeof(aOut[i].Rotation));
    }
}

// Which skeleton a ragdoll's bodies make, by their nodes' names in order (RagdollBodiesPose::Layout).
uint32_t RagdollLayout(const TiltedPhoques::Vector<RagdollBody>& acBodies) noexcept
{
    RagdollBodiesPose::Layout layout;
    for (const RagdollBody& cBody : acBodies)
        layout.Add(GetName(cBody.pNode));
    return layout.Key();
}

// Where each of a ragdoll's bodies is now, in game units, and how it is turned.
void ReadRagdollBodies(const TiltedPhoques::Vector<RagdollBody>& acBodies, const float aScale, std::vector<RagdollBodiesPose::Body>& aOut) noexcept
{
    aOut.resize(acBodies.size());
    for (size_t i = 0; i < acBodies.size(); ++i)
        aOut[i] = RagdollBodiesPose::Body{RagdollBodyPosition(acBodies[i].pHavok, aScale), RagdollBodyRotation(acBodies[i].pHavok)};
}

bool ReadOwnRagdoll(void* apRoot, std::vector<RagdollBodiesPose::Body>& aOut, uint32_t& aLayout) noexcept
{
    const float cScale = HavokWorldScale();
    TiltedPhoques::Vector<RagdollBody> bodies;
    if (!(cScale > 0.f) || !CollectRagdoll(apRoot, bodies))
        return false;
    ReadRagdollBodies(bodies, cScale, aOut);
    aLayout = RagdollLayout(bodies);
    return true;
}

// Where the owner's game has each body it sends now (RagdollPoseService): the first aSent of them. False when it sent none lately
// for this skeleton, or they lie far from this copy's.
bool OwnerBodyTargets(
    const uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority, const TiltedPhoques::Vector<RagdollBody>& acBodies, const float aScale, std::vector<BodyTarget>& aOut,
    size_t& aSent) noexcept
{
    std::vector<RagdollBodiesPose::Body> owner;
    if (acBodies.empty() || !RagdollPoseService::OwnerBodies(aFormId, acAuthority, acBodies.size(), RagdollLayout(acBodies), owner) || owner.empty() ||
        !RagdollBodiesPose::Near(owner[0].Position, RagdollBodyPosition(acBodies[0].pHavok, aScale)))
        return false;
    aSent = std::min(owner.size(), acBodies.size());
    aOut.assign(aSent, BodyTarget{});
    for (size_t i = 0; i < aSent; ++i)
    {
        for (int axis = 0; axis < 3; ++axis)
            aOut[i].Position[axis] = owner[i].Position[axis] * aScale;
        aOut[i].Rotation[0] = owner[i].Rotation.x;
        aOut[i].Rotation[1] = owner[i].Rotation.y;
        aOut[i].Rotation[2] = owner[i].Rotation.z;
        aOut[i].Rotation[3] = owner[i].Rotation.w;
    }
    return true;
}

// Where a dead creature's bodies came to rest at its owner's (RagdollPoseService::RestBodies); the bodies past the sent ones keep
// their place on the first. False when none are held for this skeleton, or they lie far from where its owner has the body (acWanted).
bool RestBodyTargets(
    const uint32_t aFormId, const TiltedPhoques::Vector<RagdollBody>& acBodies, const glm::vec3& acWanted, const float aScale, std::vector<BodyTarget>& aOut) noexcept
{
    std::vector<RagdollBodiesPose::Body> rest;
    if (acBodies.empty() || !RagdollPoseService::RestBodies(aFormId, acBodies.size(), RagdollLayout(acBodies), rest) || rest.empty() ||
        !RagdollBodiesPose::Near(rest[0].Position, acWanted))
        return false;
    OffsetTargets(acBodies, rest[0].Position - RagdollBodyPosition(acBodies[0].pHavok, aScale), aScale, aOut);
    for (size_t i = 0; i < rest.size() && i < aOut.size(); ++i)
    {
        for (int axis = 0; axis < 3; ++axis)
            aOut[i].Position[axis] = rest[i].Position[axis] * aScale;
        aOut[i].Rotation[0] = rest[i].Rotation.x;
        aOut[i].Rotation[1] = rest[i].Rotation.y;
        aOut[i].Rotation[2] = rest[i].Rotation.z;
        aOut[i].Rotation[3] = rest[i].Rotation.w;
    }
    return true;
}

// The most any bone turned from one pose of a body to another, in degrees; NaN when they cannot be compared.
float MostDegreesApart(const VRPose& acFrom, const VRPose& acTo) noexcept
{
    if (acFrom.NoBones || acTo.NoBones || acFrom.HasLegs != acTo.HasLegs)
        return std::numeric_limits<float>::quiet_NaN();
    const uint32_t cCount = acFrom.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
    float most = 0.f;
    for (uint32_t i = 0; i < cCount; ++i)
    {
        const float cDot = std::abs(glm::dot(static_cast<const glm::quat&>(acFrom.Bones[i]), static_cast<const glm::quat&>(acTo.Bones[i])));
        if (!std::isfinite(cDot))
            return std::numeric_limits<float>::quiet_NaN();
        most = std::max(most, glm::degrees(2.f * std::acos(std::min(1.f, cDot))));
    }
    return most;
}

// Whether a copy's ragdoll already lies where the bodies held for it rest (RagdollPoseService::RestBodies).
bool LiesInRestBodies(const uint32_t aFormId, const TiltedPhoques::Vector<RagdollBody>& acBodies) noexcept
{
    const float cScale = HavokWorldScale();
    std::vector<RagdollBodiesPose::Body> rest;
    if (!(cScale > 0.f) || acBodies.empty() || !RagdollPoseService::RestBodies(aFormId, acBodies.size(), RagdollLayout(acBodies), rest))
        return false;
    std::vector<RagdollBodiesPose::Body> here;
    ReadRagdollBodies(acBodies, cScale, here);
    return RagdollBodiesPose::LiesIn(rest, here);
}

// Whether a copy's skeleton already lies in a person's pose: each bone turned as in it, against the 3D root (as CaptureBodyPose reads them).
bool LiesInPose(void* apRoot, const VRPose& acPose) noexcept
{
    BoneNodes bones;
    if (!apRoot || acPose.NoBones || !FindBones(apRoot, bones))
        return false;
    const glm::mat3 cInverseRoot = glm::transpose(ToGlm(At<NiTransform>(apRoot, kWorldOffset).rotate));
    const uint32_t cCount = acPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
    VRPose here{};
    here.HasData = true;
    here.HasLegs = acPose.HasLegs;
    for (uint32_t i = 0; i < cCount; ++i)
    {
        if (!bones[i])
            return false;
        here.Bones[i] = glm::normalize(glm::quat_cast(cInverseRoot * ToGlm(At<NiTransform>(bones[i], kWorldOffset).rotate)));
    }
    return !CopyBodyRules::BonesMoved(MostDegreesApart(here, acPose));
}

// Whether a node hangs somewhere below another.
bool IsBelow(void* apNode, const void* apAncestor) noexcept
{
    void* pParent = apNode ? At<void*>(apNode, kParentOffset) : nullptr;
    for (uint32_t depth = 0; pParent && depth < 64; ++depth, pParent = At<void*>(pParent, kParentOffset))
        if (pParent == apAncestor)
            return true;
    return false;
}

uint32_t DepthOf(void* apNode) noexcept
{
    uint32_t depth = 0;
    for (void* pParent = apNode ? At<void*>(apNode, kParentOffset) : nullptr; pParent && depth < 64; pParent = At<void*>(pParent, kParentOffset))
        ++depth;
    return depth;
}

// Where each body goes for the body to lie in the owner's pose with its 3D root at acWanted (game units).
// The bones are turned as PoseActor turns them, each with everything below it, but on copies of their transforms:
// nothing of the copy's tree is written. Each body keeps its own offset from its bone, read from how both lie now
// (the ragdoll draws the bone from the body), as WeaponBodyTarget keeps a weapon body's.
// False when the skeleton cannot be read or the result is not a body near acWanted.
bool PoseTargets(
    void* apRoot, const TiltedPhoques::Vector<RagdollBody>& acBodies, const VRPose& acPose, const glm::vec3& acWanted, const float aLift, const float aScale,
    std::vector<BodyTarget>& aOut) noexcept
{
    BoneNodes bones;
    bool legsFound = false;
    if (acPose.NoBones || !FindBones(apRoot, bones, &legsFound))
        return false;

    struct Tracked
    {
        void* pNode;
        glm::mat3 Rotation;
        glm::vec3 Position;
    };
    std::vector<Tracked> tracked;
    const auto track = [&tracked](void* apNode, const glm::vec3& acOffset)
    {
        if (!apNode)
            return;
        for (const Tracked& known : tracked)
            if (known.pNode == apNode)
                return;
        const NiTransform& world = At<NiTransform>(apNode, kWorldOffset);
        tracked.push_back(Tracked{apNode, ToGlm(world.rotate), ToGlm(world.translate) + acOffset});
    };
    const NiTransform& rootWorld = At<NiTransform>(apRoot, kWorldOffset);
    const glm::mat3 cRootRotation = ToGlm(rootWorld.rotate);
    const glm::vec3 cOffset = acWanted - ToGlm(rootWorld.translate);
    for (void* pBone : bones)
        track(pBone, cOffset);
    for (const RagdollBody& body : acBodies)
        track(body.pNode, cOffset);

    // Parents first, as the resolved skeleton has them: each bone carries what hangs below it.
    std::array<uint32_t, VRPose::kBoneCount> order{};
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        order[i] = i;
    std::array<uint32_t, VRPose::kBoneCount> depths{};
    for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        depths[i] = DepthOf(bones[i]);
    std::stable_sort(order.begin(), order.end(), [&depths](const uint32_t aA, const uint32_t aB) { return depths[aA] < depths[aB]; });

    for (const uint32_t i : order)
    {
        // Legs only when the owner sent them; otherwise they hang from the pelvis as the ragdoll has them.
        if (!bones[i] || (i >= VRPose::kUpperBoneCount && !(acPose.HasLegs && legsFound)))
            continue;
        auto self = std::find_if(tracked.begin(), tracked.end(), [&](const Tracked& acKnown) { return acKnown.pNode == bones[i]; });
        if (self == tracked.end())
            continue;
        const glm::mat3 cWanted = cRootRotation * glm::mat3_cast(static_cast<glm::quat>(acPose.Bones[i]));
        const glm::mat3 cDelta = cWanted * glm::transpose(self->Rotation);
        const glm::vec3 cPivot = self->Position;
        self->Rotation = cWanted;
        for (Tracked& other : tracked)
            if (other.pNode != bones[i] && IsBelow(other.pNode, bones[i]))
            {
                other.Rotation = cDelta * other.Rotation;
                other.Position = cPivot + cDelta * (other.Position - cPivot);
            }
    }

    // A pose that puts a body far from where its owner has the body is a bad read, not a corpse.
    constexpr float cFarthest = 256.f;
    aOut.assign(acBodies.size(), BodyTarget{});
    for (size_t b = 0; b < acBodies.size(); ++b)
    {
        const auto node = std::find_if(tracked.begin(), tracked.end(), [&](const Tracked& acKnown) { return acKnown.pNode == acBodies[b].pNode; });
        if (node == tracked.end())
            return false;
        const NiTransform& now = At<NiTransform>(acBodies[b].pNode, kWorldOffset);
        const glm::mat3 cNodeNow = ToGlm(now.rotate);
        const glm::mat3 cBodyNow = glm::mat3_cast(RagdollBodyRotation(acBodies[b].pHavok));
        const glm::mat3 cOwnRotation = glm::transpose(cNodeNow) * cBodyNow;
        const glm::vec3 cOwnOffset = glm::transpose(cNodeNow) * (RagdollBodyPosition(acBodies[b].pHavok, aScale) - ToGlm(now.translate));

        const glm::vec3 cAt = node->Position + node->Rotation * cOwnOffset + glm::vec3{0.f, 0.f, aLift};
        const glm::quat cTurn = glm::normalize(glm::quat_cast(node->Rotation * cOwnRotation));
        if (!std::isfinite(cAt.x) || !std::isfinite(cAt.y) || !std::isfinite(cAt.z) || !std::isfinite(cTurn.w) || glm::distance(cAt, acWanted) > cFarthest)
            return false;
        aOut[b].Position[0] = cAt.x * aScale;
        aOut[b].Position[1] = cAt.y * aScale;
        aOut[b].Position[2] = cAt.z * aScale;
        aOut[b].Rotation[0] = cTurn.x;
        aOut[b].Rotation[1] = cTurn.y;
        aOut[b].Rotation[2] = cTurn.z;
        aOut[b].Rotation[3] = cTurn.w;
    }
    return true;
}

// Whether this build can move a ragdoll's bodies at all, said once when it cannot.
bool CanMoveRagdolls(void* apHkpWorld) noexcept
{
#ifdef SKYRIMVR
    (void)apHkpWorld;
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    return s_setPositionAndRotation.Get() != nullptr;
#else
    // The ids and the slot above are checked on SkyrimSE.exe 1.7.104.
    // A body set while asleep needs waking to settle. The world's flag controls only the setter's implicit wake;
    // SetRagdollBody also calls the verified hkpEntity::activate explicitly (AE 60849, 1.7.104).
    // RC3's flat world has this flag off, so requiring it even with that call left every corpse where it fell.
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, kHkpEntityActivateAeId, 60096);
    if (apHkpWorld && !At<bool>(apHkpWorld, kActivateOnTransformChangeOffset) && !s_activate.Get())
    {
        static bool s_said = false;
        if (!s_said)
            spdlog::warn("CorpseDiag: neither the transform setter nor hkpEntity::activate can wake a body here; ragdolls are left where they fell");
        s_said = true;
        return false;
    }
    return true;
#endif
}

// Sets one body where it goes, with no speed left, and wakes it so that it settles from there. False when nothing was written.
// acVelocity (Havok units a second) gives it a speed instead: a carried body followed on flat (FollowCarriedBodies).
bool SetRagdollBody(const RagdollBody& acBody, const BodyTarget& acTarget, const glm::vec3& acVelocity = glm::vec3{}) noexcept
{
#ifdef SKYRIMVR
    POINTER_SKYRIMSE(THkpEntitySetPositionAndRotation, s_setPositionAndRotation, 9000002, 9000002);
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, 60096, 60096);
    if (!s_setPositionAndRotation.Get())
        return false;
    s_setPositionAndRotation.Get()(acBody.pHavok, acTarget.Position, acTarget.Rotation);
#else
    // Only through a wrapper known to be a bhkRigidBody (or a bhkRigidBodyT): each class's table is asked its name once.
    static std::unordered_map<void*, bool> s_isRigidBodyTable;
    void** pTable = acBody.pWrapper ? *static_cast<void***>(acBody.pWrapper) : nullptr;
    if (!pTable)
        return false;
    auto known = s_isRigidBodyTable.find(pTable);
    if (known == s_isRigidBodyTable.end())
    {
        const char* pName = RttiNameOf(acBody.pWrapper);
        known = s_isRigidBodyTable.emplace(pTable, pName && std::strncmp(pName, "bhkRigidBody", 12) == 0).first;
    }
    if (!known->second)
        return false;
    reinterpret_cast<TBhkRigidBodySetPositionAndRotation*>(pTable[kBhkRigidBodySetPositionAndRotationSlot])(acBody.pWrapper, acTarget.Position, acTarget.Rotation);
#endif
    std::memset(acBody.pHavok + kAngularVelocityOffset - 0x10, 0, 16); // linear velocity
    std::memcpy(acBody.pHavok + kAngularVelocityOffset - 0x10, &acVelocity, sizeof(float) * 3);
    std::memset(acBody.pHavok + kAngularVelocityOffset, 0, 16);
#ifdef SKYRIMVR
    if (s_activate.Get())
        s_activate.Get()(acBody.pHavok);
#else
    // Required when this world's implicit transform-wake flag is off (CanMoveRagdolls).
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, kHkpEntityActivateAeId, 60096);
    if (s_activate.Get())
        s_activate.Get()(acBody.pHavok);
#endif
    return true;
}

// Moves a copy's ragdoll so that it lies at acWanted: every body by one offset, or, with apPose, each into the owner's pose.
// Takes the world's write lock. Says how it ended (CopyBodyRules::OutcomeOf): a pose that cannot be read falls back to the offset,
// which is a translation, not a pose. aBlend below 1 moves each body only that part of the way, for a blend step.
CopyBodyRules::MoveOutcome MoveRagdoll(Actor* apActor, const glm::vec3& acWanted, const VRPose* apPose, const float aApart, const float aBlend = 1.f) noexcept
{
    const float cScale = HavokWorldScale();
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    TiltedPhoques::Vector<RagdollBody> bodies;
    void* pHkpWorld = cScale > 0.f && pRoot ? CollectRagdoll(pRoot, bodies) : nullptr;
    if (!pHkpWorld || !CanMoveRagdolls(pHkpWorld))
        return CopyBodyRules::MoveOutcome::NotWritten;

    // Its root put just above where the owner has the body, to settle from there. Where the owner has a dead body is its 3D root, which
    // follows its ragdoll's root, not the ground: dropped 20 units into its owner's pose, the copy fell out of it (live-copy-death-pose).
    // A posed body is laid where its owner's lay, raised only to stay above the copy's own lowest body (SettleLift).
    const float cLift = CopyBodyRules::kSettleDropUnits;
    const glm::vec3 cFrom = RagdollBodyPosition(bodies[0].pHavok, cScale);
    std::vector<BodyTarget> targets;
    // A creature lies in the bodies its owner's came to rest in; a person in its owner's bones.
    const bool cPosed =
        apPose && (apPose->NoBones ? RestBodyTargets(apActor->formID, bodies, acWanted, cScale, targets) : PoseTargets(pRoot, bodies, *apPose, acWanted, 0.f, cScale, targets));
    float posedLift = cLift;
    const auto cSource = CopyBodyRules::TargetSourceOf(cPosed);
    if (cSource == CopyBodyRules::TargetSource::OwnerPoseSettled)
    {
        constexpr float cNaN = std::numeric_limits<float>::quiet_NaN();
        CopyBodyRules::SettleGround ground{};
        ground.Apart = aApart;
        ground.LowestHere = std::numeric_limits<float>::max();
        ground.LowestPosed = std::numeric_limits<float>::max();
        // Any reading that is not a number makes it a NaN, so that the rule keeps the old drop.
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const float cHere = RagdollBodyPosition(bodies[i].pHavok, cScale).z;
            const float cPosedZ = targets[i].Position[2] / cScale;
            const float* pVelocity = reinterpret_cast<const float*>(bodies[i].pHavok + kAngularVelocityOffset - 0x10);
            const float cSpeed = glm::length(glm::vec3{pVelocity[0], pVelocity[1], pVelocity[2]}) / cScale;
            ground.LowestHere = std::isfinite(cHere) ? std::min(ground.LowestHere, cHere) : cNaN;
            ground.LowestPosed = std::isfinite(cPosedZ) ? std::min(ground.LowestPosed, cPosedZ) : cNaN;
            ground.FastestSpeed = std::isfinite(cSpeed) ? std::max(ground.FastestSpeed, cSpeed) : cNaN;
            if (std::isnan(ground.LowestHere) || std::isnan(ground.LowestPosed) || std::isnan(ground.FastestSpeed))
                break;
        }
        posedLift = CopyBodyRules::SettleLift(ground);
        for (BodyTarget& target : targets)
            target.Position[2] += posedLift * cScale;
    }
    else if (cSource == CopyBodyRules::TargetSource::CurrentBodies)
        OffsetTargets(bodies, glm::vec3{acWanted.x - cFrom.x, acWanted.y - cFrom.y, acWanted.z + cLift - cFrom.z}, cScale, targets);

    const bool cStep = aBlend < 1.f;
    if (cStep)
    {
        // A part of the way from where each body is now, turned the same part.
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const float* pAt = reinterpret_cast<const float*>(bodies[i].pHavok + kBodyTranslationOffset);
            for (int axis = 0; axis < 3; ++axis)
                targets[i].Position[axis] = pAt[axis] + (targets[i].Position[axis] - pAt[axis]) * aBlend;
            const glm::quat cTo{targets[i].Rotation[3], targets[i].Rotation[0], targets[i].Rotation[1], targets[i].Rotation[2]};
            const glm::quat cTurn = glm::normalize(glm::slerp(RagdollBodyRotation(bodies[i].pHavok), cTo, aBlend));
            targets[i].Rotation[0] = cTurn.x;
            targets[i].Rotation[1] = cTurn.y;
            targets[i].Rotation[2] = cTurn.z;
            targets[i].Rotation[3] = cTurn.w;
        }
    }

    WorldWriteLock lock(BhkWorldOf(pHkpWorld));
    if (!lock.Held())
        return CopyBodyRules::MoveOutcome::NotWritten;
    uint32_t written = 0;
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        // Still in that world now that it is locked.
        if (At<void*>(bodies[i].pHavok, kHkWorldOffset) != pHkpWorld)
            continue;
        if (SetRagdollBody(bodies[i], targets[i]))
            ++written;
    }
    const auto cOutcome = CopyBodyRules::OutcomeOf(written, static_cast<uint32_t>(bodies.size()), cPosed);
    if (cStep)
        spdlog::debug("CorpseDiag: {:X}'s ragdoll moved {:.2f} of the way to where its owner's corpse is ({} of {} bodies)", apActor->formID, aBlend, written, bodies.size());
    else if (CopyBodyRules::BodyMoved(cOutcome))
    {
        spdlog::debug(
            "CorpseDiag: {:X}'s ragdoll lay {:.0f} units from where its owner's corpse is; moved there ({} of {} bodies{}); "
            "from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}); its cell {:X}, attached {}",
            apActor->formID, aApart, written, bodies.size(),
            cPosed   ? ", in its owner's last pose"
            : apPose ? ", only moved: its owner's pose unreadable"
                     : "",
            cFrom.x, cFrom.y, cFrom.z, acWanted.x, acWanted.y, acWanted.z, apActor->parentCell ? apActor->parentCell->formID : 0,
            apActor->parentCell && apActor->parentCell->IsAttached());
        if (cPosed)
            spdlog::debug("CorpseDiag: {:X} was laid {:.1f} units above where its owner has the body", apActor->formID, posedLift);
    }
    else
        spdlog::debug(
            "CorpseDiag: {:X}'s ragdoll lies {:.0f} units from where its owner's corpse is; none of its {} bodies could be moved", apActor->formID, aApart, bodies.size());
    return cOutcome;
}

const char* MoveKindName(const CopyBodyRules::MoveKind aKind) noexcept
{
    switch (aKind)
    {
    case CopyBodyRules::MoveKind::Settle: return "its settle into its owner's pose";
    case CopyBodyRules::MoveKind::KnockPlace: return "its move to where its owner's NPC fell";
    default: return "its move";
    }
}

// What a move's end means for its copy's record: the one-shots count only a move that did what it is for, and the back-off
// and the "now lies" line only a body that moved. aQueued: it was queued (flat) rather than made at once.
void FinishMove(
    const uint32_t aFormId, PlacedCorpse& aPlaced, const CopyBodyRules::MoveKind aKind, const CopyBodyRules::MoveOutcome aOutcome, const float aApart,
    const std::chrono::steady_clock::time_point aNow, const bool aQueued) noexcept
{
    using namespace CopyBodyRules;
    // A whole move ends any blend.
    aPlaced.BlendStep = 0;
    aPlaced.BlendSteps = 0;
    CopyBodyRules::OneShot* pShot = aKind == MoveKind::Settle ? &aPlaced.Settle : aKind == MoveKind::KnockPlace ? &aPlaced.KnockPlace : nullptr;
    // A queued one-shot no longer waiting was started afresh meanwhile (a new owner stream, a new fall): this end is not its.
    if (pShot)
    {
        if (!aQueued || pShot->Pending)
        {
            *pShot = OnFinished(*pShot, aKind, aOutcome);
            if (GaveUp(*pShot))
                spdlog::debug("CorpseDiag: {:X}: {} was given up after {} tries", aFormId, MoveKindName(aKind), static_cast<uint32_t>(kMaxMoveTries));
        }
    }
    else
        aPlaced.MovePending = false;
    if (aKind == MoveKind::KnockPlace && Succeeded(aKind, aOutcome))
        spdlog::debug("KnockDown: NPC copy {:X} lay {:.0f} units from where its owner's NPC fell; its ragdoll was put there", aFormId, aApart);
    if (!BodyMoved(aOutcome))
        return;
    aPlaced.MovedAt = aNow;
    aPlaced.SayAfter = true;
    if (aKind != MoveKind::Settle)
    {
        aPlaced.ApartBeforeMove = aApart;
        ++aPlaced.Streak;
    }
}

// A blend step that moved goes on to the next; one that did not starts the blend over. The move it is part of is not finished yet.
void FinishBlendStep(PlacedCorpse& aPlaced, const CopyBodyRules::MoveOutcome aOutcome) noexcept
{
    aPlaced.MovePending = false;
    if (CopyBodyRules::BodyMoved(aOutcome))
        ++aPlaced.BlendStep;
    else
    {
        aPlaced.BlendStep = 0;
        aPlaced.BlendSteps = 0;
    }
}

// Whether this player holds the body: the grab key on flat, a hand on it in VR.
bool HeldHere(Actor* apActor, void* apRoot) noexcept
{
#ifdef SKYRIMVR
    return NearestHandToBody(apRoot) <= 40.f;
#else
    (void)apRoot;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && pPlayer->grabbedObjectHandle && TESObjectREFR::GetByHandle(pPlayer->grabbedObjectHandle) == apActor;
#endif
}

// Whether a container or loot menu is open while the body lies close to the player: it may be the body being looted.
// The menu's target is not read, so any body that close waits until the menu closes.
bool LootOpenNear(const glm::vec3& acBody) noexcept
{
    constexpr float cNear = 500.f;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    UI* pUI = UI::Get();
    if (!pPlayer || !pUI || glm::distance(static_cast<glm::vec3>(pPlayer->position), acBody) > cNear)
        return false;
    static const BSFixedString s_container("ContainerMenu");
    static const BSFixedString s_loot("LootMenu"); // QuickLoot's
    return pUI->GetMenuOpen(s_container) || pUI->GetMenuOpen(s_loot);
}

// Whether the player has a view for the move to wait on: a headset in VR, the game's camera on flat.
bool PlayerHasView() noexcept
{
#ifdef SKYRIMVR
    return VRControllers::HeadsetTransform() != nullptr;
#else
    return true;
#endif
}

#ifndef SKYRIMVR
// A move decided in the world's update, made at the start of the next frame (MovePendingRagdolls), outside the world's step.
// It carries what it was decided under, and is looked at again before it runs (CopyBodyRules::StillAuthorized).
struct PendingRagdollMove
{
    uint32_t FormId = 0;
    CopyBodyRules::CopyAuthority Authority{};
    CopyBodyRules::MoveKind Kind = CopyBodyRules::MoveKind::Place;
    glm::vec3 Wanted{};
    float Apart = 0.f;
    VRPose Pose{};
    std::chrono::steady_clock::time_point QueuedAt{};
    uint32_t ActorCell = 0;
    uint32_t PlayerCell = 0;
    // The part of the way it moves (1: all of it), and whether it is a step of a blend, which may run in view.
    float Blend = 1.f;
    bool Blended = false;
};
std::mutex s_pendingRagdollMovesLock;
std::vector<PendingRagdollMove> s_pendingRagdollMoves;
#endif

// A carried body followed (CarriedBodyFollow): asked for in the world's update (PlaceCorpse), set at the start of the next frame.
// VR follows only a creature's ragdoll this way (RagdollPoseService); it draws a person from its owner's bones (OnFrameEnd).
struct FollowRequest
{
    uint32_t FormId = 0;
    CopyBodyRules::CopyAuthority Authority{};
    VRPose Pose{};
    glm::vec3 Wanted{};
    bool BonesMove = false;
    // When the pose was taken, on which reference and in which cells: set a frame later only if none changed (StillAsked).
    CarriedBodyFollow::Asked Stamp{};
};
struct FollowedBody
{
    CopyBodyRules::CopyAuthority Authority{};
    CarriedBodyFollow::Follow State{};
};
// Taken after s_placedCorpsesLock, never before it.
std::mutex s_followLock;
std::vector<FollowRequest> s_followRequests;
std::unordered_map<uint32_t, FollowedBody> s_followed;

// Called with s_placedCorpsesLock held: the latest pose of the update replaces an older one.
void AskToFollow(
    const uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority, const VRPose& acPose, const glm::vec3& acWanted, const bool aBonesMove,
    const CarriedBodyFollow::Asked& acStamp) noexcept
{
    if (!acPose.HasData || (acPose.NoBones && !acPose.HasRootPosition))
        return;
    std::scoped_lock lock(s_followLock);
    const FollowRequest cRequest{aFormId, acAuthority, acPose, acWanted, aBonesMove, acStamp};
    const auto it = std::find_if(s_followRequests.begin(), s_followRequests.end(), [aFormId](const FollowRequest& acRequest) { return acRequest.FormId == aFormId; });
    if (it != s_followRequests.end())
        *it = cRequest;
    else if (s_followRequests.size() < 64)
        s_followRequests.push_back(cRequest);
}

// Whether that copy's ragdoll is set into its owner's bones now.
bool FollowedNow(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_followLock);
    return s_followed.find(aFormId) != s_followed.end();
}

// ---------------------------------------------------------------------------------------------------------------
// Blades as segments, for the blade stop (StopLocalBlades).
// A blade runs along its node's +Y, where its bound and its body's centre of mass lie, from the grip to the bound's far edge.
// An iron sword's bound is 31.9 units around a point 21.9 along +Y, so its blade is 53.8 long.
struct Blade
{
    glm::vec3 Grip{};
    glm::vec3 Axis{}; // unit length
    float Length = 0.f;
};

// How far a weapon's blade reaches along its node's +Y.
// Measured before anything of ours moves the node this frame, while its world transform still matches the game's bound.
struct BladeExtent
{
    void* pNode = nullptr; // the weapon's own node, hanging under WEAPON or SHIELD
    float Length = 0.f;
    float Radius = 0.f;
    float Along = 0.f;
};

// False for what is not held at one end and long along +Y: no bound, or one centred off that axis (a shield).
bool MeasureBlade(void* apNode, BladeExtent& aOut) noexcept
{
    const NiTransform& world = At<NiTransform>(apNode, kWorldOffset);
    const glm::vec3 grip = ToGlm(world.translate);
    const NiBoundRead bound = ReadWorldBound(apNode, grip);
    const glm::vec3 axis = ToGlm(world.rotate)[1];
    const float axisLength = glm::length(axis);
    if (!bound.Valid || bound.Radius < 8.f || bound.Radius > 150.f || !(axisLength > 0.5f))
        return false;
    const float along = glm::dot(bound.Centre - grip, axis / axisLength);
    if (!(along >= 0.4f * bound.Radius))
        return false;
    aOut = BladeExtent{apNode, along + bound.Radius, bound.Radius, along};
    return true;
}

Blade BladeAt(const BladeExtent& acExtent) noexcept
{
    const NiTransform& world = At<NiTransform>(acExtent.pNode, kWorldOffset);
    return Blade{ToGlm(world.translate), glm::normalize(ToGlm(world.rotate)[1]), acExtent.Length};
}

// Which side of his blade ours is on: the sign of our blade's direction against the plane through our grip and his blade.
// The plane's unit normal goes in aNormal. The sign flips when our blade or our grip passes through his blade's line.
// Returns 0 when the plane is not defined (our grip on his line).
float SideOf(const Blade& acOurs, const Blade& acHis, glm::vec3& aNormal) noexcept
{
    const glm::vec3 normal = glm::cross(acHis.Grip - acOurs.Grip, acHis.Axis);
    const float length = glm::length(normal);
    if (!(length > 1.f))
        return 0.f;
    aNormal = normal / length;
    return glm::dot(acOurs.Axis, aNormal);
}

// Where two lines come closest: aT along the first (from acA along acU) and aV along the second (from acB along acW).
// Both directions are unit length. False when the lines are within about 10 degrees of parallel.
bool ClosestPointsOnLines(const glm::vec3& acA, const glm::vec3& acU, const glm::vec3& acB, const glm::vec3& acW, float& aT, float& aV) noexcept
{
    const float b = glm::dot(acU, acW);
    const float denominator = 1.f - b * b;
    if (!(denominator >= 0.03f))
        return false;
    const glm::vec3 r = acA - acB;
    const float d = glm::dot(acU, r);
    const float e = glm::dot(acW, r);
    aT = (b * e - d) / denominator;
    aV = (e - b * d) / denominator;
    return std::isfinite(aT) && std::isfinite(aV);
}

float DistanceToBlade(const glm::vec3& acPoint, const Blade& acBlade) noexcept
{
    const float t = glm::clamp(glm::dot(acPoint - acBlade.Grip, acBlade.Axis), 0.f, acBlade.Length);
    return glm::distance(acPoint, acBlade.Grip + acBlade.Axis * t);
}

// The copies' drawn blades at this frame end, once they are posed (RecordCopyBlades). Frame end only.
struct CopyBlade
{
    uint64_t Key = 0; // form id << 1 | side (0 left, 1 right)
    Blade Segment{};
};
std::vector<CopyBlade> s_copyBlades;
} // namespace

namespace VRBodySync
{
uint32_t CaptureSolvedPose(UrGamePose* apOut) noexcept
{
    auto* player = PlayerCharacter::Get();
    if (!apOut || !player || !player->parentCell || player->IsStillLoading() || LoadingScreenUp() || player->actorState.IsDeadOrDying() || player->actorState.IsInKnockRagdoll() ||
        player->actorState.IsBleedingOut())
        return 0;
    auto* root = player->GetNiNode();
    if (!root || !ReadableMemory::IsReadable(root, kNiAVObjectSize))
        return 0;
    ReadableMemory::CacheScope memory;
    BoneNodes nodes{};
    bool legs{};
    if (!FindBones(root, nodes, &legs) || !legs)
        return 0;
    const auto& world = At<NiTransform>(root, kWorldOffset);
    const auto rotation = ToGlm(world.rotate);
    const auto origin = ToGlm(world.translate);
    if (!BodyPoseValidation::IsRotation(rotation) || !IsFiniteVector(origin))
        return 0;
    UrGamePose frame{};
    const auto forward = rotation * glm::vec3(0.f, 1.f, 0.f);
    frame.Heading = std::atan2(forward.x, forward.y);
    const auto inverse = glm::transpose(glm::mat3(glm::rotate(glm::mat4(1.f), -frame.Heading, glm::vec3(0.f, 0.f, 1.f))));
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (!ReadableMemory::IsReadable(nodes[i], kNiAVObjectSize))
            return 0;
        const auto& transform = At<NiTransform>(nodes[i], kWorldOffset);
        const auto bone = ToGlm(transform.rotate);
        if (!BodyPoseValidation::IsRotation(bone))
            return 0;
        Quaternion_NetQuantize relative;
        relative = glm::normalize(glm::quat_cast(inverse * bone));
        frame.Bones[i] = relative.Pack();
        float* offset = i == VRPose::kPelvis ? frame.Hips : i == VRPose::kLeftHand ? frame.LeftHand : i == VRPose::kRightHand ? frame.RightHand : nullptr;
        if (offset)
        {
            const auto value = inverse * (ToGlm(transform.translate) - origin);
            if (!(glm::dot(value, value) < 512.f * 512.f))
                return 0;
            std::copy_n(&value.x, 3, offset);
        }
    }
    std::copy_n(&origin.x, 3, frame.Position);
    if (const auto* skeleton = FindSkeletonRoot(root); skeleton && ReadableMemory::IsReadable(skeleton, kNiAVObjectSize))
    {
        const auto scale = At<NiTransform>(const_cast<void*>(skeleton), kWorldOffset).scale;
        if (std::isfinite(scale) && scale >= .05f && scale <= 20.f)
            frame.Scale = static_cast<uint32_t>(scale * 1000.f + .5f);
    }
    *apOut = frame;
    return 1;
}

void AddPendingWeaponBodies() noexcept
{
    AddPendingBodies();
}

bool BodyPelvisHeight(Actor* apActor, float& aHeight, glm::vec3& aPelvis) noexcept
{
    // A body a hand is on, in this game or its owner's, is held up, not standing.
    constexpr auto cPosedLately = std::chrono::seconds(5);
    constexpr float cHandReach = 150.f;
    if (const auto cPosedAt = BodyPosedAt(apActor->formID); cPosedAt && std::chrono::steady_clock::now() - *cPosedAt < cPosedLately)
        return false;

    void* pRoot = apActor->GetNiNode();
    void* pPelvis = pRoot ? FindShallowest(pRoot, "NPC Pelvis [Pelv]") : nullptr;
    void* pLeft = pPelvis ? FindShallowest(pRoot, "NPC L Foot [Lft ]") : nullptr;
    void* pRight = pPelvis ? FindShallowest(pRoot, "NPC R Foot [Rft ]") : nullptr;
    if (!pLeft || !pRight)
        return false;

    const NiPoint3& pelvis = At<NiTransform>(pPelvis, kWorldOffset).translate;
    aPelvis = glm::vec3{pelvis.x, pelvis.y, pelvis.z};
    for (uint8_t side = 0; side < 2; ++side)
        if (glm::vec3 hand{}; LocalHandPosition(side, hand) && glm::distance(hand, aPelvis) < cHandReach)
            return false;

    aHeight = pelvis.z - std::min(At<NiTransform>(pLeft, kWorldOffset).translate.z, At<NiTransform>(pRight, kWorldOffset).translate.z);
    return true;
}

// The blade stop, after HandParries below; OnFrameEnd calls these.
std::array<BladeExtent, 2> MeasureCopyBlades(const Rig& acRig) noexcept;
void RecordCopyBlades(uint32_t aFormId, Actor* apActor, const std::array<BladeExtent, 2>& acExtents) noexcept;
void StopLocalBlades(std::chrono::steady_clock::time_point aNow) noexcept;

// Runs on the game thread, where the actor list is safe to walk, not at the renderer's frame end like the body sync.
// A frame-old transform does not matter for a haptic pulse.
void BeginWeaponTouch() noexcept
{
    std::lock_guard lock(s_touchLock);
    s_touchCandidates.clear();
}

void ConsiderForWeaponTouch(Actor* apActor) noexcept
{
    if (!apActor)
        return;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || apActor == pPlayer)
        return;

    // Only actors within reach: a blade is under two metres and an arm less, so 300 units is generous.
    // This keeps the per-frame work to the two or three actors next to the player.
    const glm::vec3 delta = static_cast<glm::vec3>(apActor->position) - static_cast<glm::vec3>(pPlayer->position);
    if (glm::dot(delta, delta) > 300.f * 300.f)
        return;

    std::lock_guard lock(s_touchLock);
    if (s_touchCandidates.size() < 16)
        s_touchCandidates.push_back(apActor->formID);
}

void EndWeaponTouch() noexcept
{
    TiltedPhoques::Vector<uint32_t> candidates;
    {
        std::lock_guard lock(s_touchLock);
        candidates = s_touchCandidates;
    }
    if (candidates.empty())
        return;

    // One thread at a time through the whole pass; if another thread is already in it, this one skips the frame.
    static std::mutex s_passLock;
    std::unique_lock pass(s_passLock, std::try_to_lock);
    if (!pass.owns_lock())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // Not while dying or bleeding out: through death and respawn the 3D is torn down and rebuilt, and reading it then can crash.
    if (pPlayer->actorState.IsDeadOrDying() || pPlayer->actorState.IsBleedingOut())
        return;

    Holding mine;
    if (!ResolveHolding(pPlayer, mine))
        return;

    // How close counts as contact: each segment runs down the middle of a blade, and blades have width.
    // 10 units is about 14 cm, a touch rather than a near miss.
    constexpr float cTouch = 10.f;

    struct Side
    {
        const Segment* pWeapon;
        const Segment* pHand;
        bool RightHand;
    };
    const Side sides[2] = {{&mine.RightWeapon, &mine.RightHand, true}, {&mine.LeftWeapon, &mine.LeftHand, false}};

    // The last distance to each copy, by own hand, so how fast a gap closes is measured to the same copy each frame, never between
    // two copies when the nearest changes. Safe only because the pass above is serialised.
    static std::unordered_map<uint32_t, std::array<float, 2>> s_lastDistance;
    std::unordered_map<uint32_t, std::array<float, 2>> distances;

    for (const uint32_t formId : candidates)
    {
        Actor* pOther = Cast<Actor>(TESForm::GetById(formId));
        if (!pOther)
            continue;
        Holding theirs;
        if (!ResolveHolding(pOther, theirs))
            continue;
        auto& nearest = distances.try_emplace(formId, std::array<float, 2>{1e9f, 1e9f}).first->second;
        for (const Side& side : sides)
        {
            const int slot = side.RightHand ? 0 : 1;
            for (const Segment* pTheir : {&theirs.RightWeapon, &theirs.LeftWeapon, &theirs.RightHand, &theirs.LeftHand})
            {
                if (!pTheir->Valid)
                    continue;
                for (const Segment* pMineSide : {side.pWeapon, side.pHand})
                    if (pMineSide->Valid)
                        nearest[slot] = std::min(nearest[slot], SegmentDistance(*pMineSide, *pTheir));
            }
        }
    }

    for (const Side& side : sides)
    {
        if (!side.pWeapon->Valid && !side.pHand->Valid)
            continue;
        const int slot = side.RightHand ? 0 : 1;
        float strength = -1.f;
        for (const auto& [cFormId, cNearest] : distances)
        {
            const float cDistance = cNearest[slot];
            if (cDistance > cTouch)
                continue;
            const auto cLast = s_lastDistance.find(cFormId);
            // A copy first seen this close is touching, not closing in.
            const float cPrevious = cLast != s_lastDistance.end() ? cLast->second[slot] : cDistance;
            // Strength from how fast the gap is closing plus how deep the overlap is.
            // A blade resting against another still buzzes faintly: it is a touch, not a hit.
            const float cClosing = glm::max(0.f, cPrevious - cDistance);
            const float cDepth = (cTouch - cDistance) / cTouch;
            strength = glm::max(strength, glm::clamp(0.25f + cDepth * 0.45f + cClosing * 0.08f, 0.f, 1.f));
        }
        if (strength < 0.f)
            continue;

        VRHaptics::Pulse(side.RightHand, static_cast<uint16_t>(strength * 3999.f));
    }
    s_lastDistance = std::move(distances);
}

// Copies with an arrow on their bow string or in a hand (NotifyArrowNocked).
// The game attaches the arrow to the hand bone as a third-person draw holds it, which points anywhere on a hand posed from VR.
// PlaceNockedArrows lays it from the string hand to the bow hand, or where the owner holds it, once the hands are posed.
std::mutex s_nockedLock;
struct CopyArrow
{
    // The "Arrow:0" nodes the copy had before the arrow was attached; only a node not in this list is its arrow.
    // The quiver model's root is also named "Arrow:0", and the arrow is made a moment after AttachArrow returns.
    std::vector<void*> Before;
    // Held in a hand, not on the string.
    ArrowHold Hold;
    // The hand the arrow was last seen in, -1 when never: the bow is in the other one.
    int StringSide = -1;
};
std::unordered_map<uint32_t, CopyArrow> s_nocked;

/**
 * List the "Arrow:0" nodes in a character's 3D, the root included.
 *
 * @param apRoot The character's 3D root.
 * @return The nodes, breadth first.
 */
std::vector<void*> ArrowNodesOf(void* apRoot) noexcept
{
    std::vector<void*> out;
    VisitTree(
        apRoot, 4096,
        [&out](void* pObject)
        {
            if (const char* pName = GetName(pObject); pName && std::strcmp(pName, "Arrow:0") == 0)
                out.push_back(pObject);
        });
    return out;
}

// The arrow model's length axis in its own frame, pointing from the nock to the head.
// The model's origin is the arrowhead; the nock lies about 64 units back along -y (the vanilla iron arrow mesh).
constexpr glm::vec3 kArrowForward{0.f, 1.f, 0.f};
constexpr float kArrowNockDistance = 64.f;

// What PlaceCopyArrow found in a copy's 3D, kept between frames: the hands, the bow's string ends and the nocked arrow, everything below the
// arrow and the string's middle bone, and every object above those. Each is checked to hang in its parent's slot still (plain reads), and
// looked for afresh on a new 3D or arrow, when one no longer hangs there, and every 250 ms for what was added.
struct ArrowNodes
{
    struct Item
    {
        void* pObject;
        void* pNode;
        int Parent;
        uint16_t Slot;
    };
    // Parents before their children, the root first.
    std::vector<Item> Items;
    void* pRoot = nullptr;
    std::vector<void*> Before;
    std::array<void*, 2> pHands{};
    std::array<void*, 2> pStringEnds{};
    int Arrow = -1;
    std::chrono::steady_clock::time_point FindAgainAt{};
    std::chrono::steady_clock::time_point UsedAt{};
};
// By copy, on the one thread that places the arrows (the VR frame end, or the flat presenter's commit).
std::unordered_map<uint32_t, ArrowNodes> s_arrowNodes;

// One walk finds the two hands, the bow's string ends and the nocked arrow: an "Arrow:0" that was not there before it was attached.
// False when it finds no arrow or not both hands.
bool FindArrowNodes(void* apRoot, const std::vector<void*>& acBefore, ArrowNodes& aOut) noexcept
{
    std::vector<ArrowNodes::Item> walked{{apRoot, nullptr, -1, 0}};
    int hands[2]{-1, -1};
    int stringEnds[2]{-1, -1};
    int arrow = -1;
    for (size_t head = 0; head < walked.size() && head < 4096; ++head)
    {
        void* pObject = walked[head].pObject;
        const int cAt = static_cast<int>(head);
        if (const char* pName = GetName(pObject))
        {
            // The first found is the shallowest: physics armour carries deeper copies of the hands.
            if (_stricmp(pName, kHandNames[0]) == 0)
            {
                if (hands[0] < 0)
                    hands[0] = cAt;
            }
            else if (_stricmp(pName, kHandNames[1]) == 0)
            {
                if (hands[1] < 0)
                    hands[1] = cAt;
            }
            else if (std::strcmp(pName, "Arrow:0") == 0 && std::find(acBefore.begin(), acBefore.end(), pObject) == acBefore.end())
                arrow = cAt;
            else if (stringEnds[0] < 0 && std::strcmp(pName, "Bow_StringBone1") == 0)
                stringEnds[0] = cAt;
            else if (stringEnds[1] < 0 && std::strcmp(pName, "Bow_StringBone2") == 0)
                stringEnds[1] = cAt;
        }
        void* pNode = AsNode(pObject);
        walked[head].pNode = pNode;
        if (!pNode)
            continue;
        const ChildSlots cChildren = ChildSlotsOf(pNode);
        for (uint16_t slot = 0; cChildren.pSlots && slot < cChildren.Count; ++slot)
            if (void* pChild = cChildren.pSlots[slot])
                walked.push_back({pChild, nullptr, cAt, slot});
    }
    // A walk cut short at 4096 objects may have missed the hands or the arrow: nothing is moved then.
    if (arrow < 0 || hands[0] < 0 || hands[1] < 0 || walked.size() > 4096)
        return false;

    // Kept: what the placement moves (the arrow, the string's middle bone and all below them), what it reads, and the way down to each.
    std::vector<uint8_t> kept(walked.size(), 0);
    std::vector<uint8_t> moved(walked.size(), 0);
    const auto cKeepUp = [&walked, &kept](int aAt)
    {
        for (; aAt >= 0 && !kept[aAt]; aAt = walked[aAt].Parent)
            kept[aAt] = 1;
    };
    for (size_t i = 0; i < walked.size(); ++i)
    {
        const int cAt = static_cast<int>(i);
        moved[i] = cAt == arrow || cAt == stringEnds[1] || (walked[i].Parent >= 0 && moved[walked[i].Parent]);
        if (moved[i])
            cKeepUp(cAt);
    }
    cKeepUp(hands[0]);
    cKeepUp(hands[1]);
    cKeepUp(stringEnds[0]);

    std::vector<int> keptAt(walked.size(), -1);
    aOut.Items.clear();
    for (size_t i = 0; i < walked.size(); ++i)
    {
        if (!kept[i])
            continue;
        keptAt[i] = static_cast<int>(aOut.Items.size());
        aOut.Items.push_back({walked[i].pObject, walked[i].pNode, walked[i].Parent >= 0 ? keptAt[walked[i].Parent] : -1, walked[i].Slot});
    }
    aOut.pRoot = apRoot;
    aOut.Before = acBefore;
    aOut.pHands = {walked[hands[0]].pObject, walked[hands[1]].pObject};
    aOut.pStringEnds = {stringEnds[0] >= 0 ? walked[stringEnds[0]].pObject : nullptr, stringEnds[1] >= 0 ? walked[stringEnds[1]].pObject : nullptr};
    aOut.Arrow = keptAt[arrow];
    return true;
}

// Whether every object kept still hangs where it was found, from the root down: only objects already checked are read.
bool ArrowNodesIntact(const ArrowNodes& acNodes, void* apRoot) noexcept
{
    if (!apRoot || acNodes.pRoot != apRoot || acNodes.Arrow < 0 || acNodes.Items.empty() || acNodes.Items[0].pObject != apRoot)
        return false;
    for (size_t i = 1; i < acNodes.Items.size(); ++i)
    {
        const ArrowNodes::Item& cItem = acNodes.Items[i];
        void* pParentNode = cItem.Parent >= 0 ? acNodes.Items[cItem.Parent].pNode : nullptr;
        if (!pParentNode)
            return false;
        const ChildSlots cSlots = ChildSlotsOf(pParentNode);
        if (!cSlots.pSlots || cItem.Slot >= cSlots.Count || cSlots.pSlots[cItem.Slot] != cItem.pObject)
            return false;
    }
    return true;
}

// Lays one copy's arrow from the string hand to the bow hand (aBowSide), or where its owner holds it, from the hands as they are now.
void PlaceCopyArrow(const uint32_t aFormId, const CopyArrow& acArrow, void* apRoot, const size_t aBowSide, void** appPlaced = nullptr) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    static std::chrono::steady_clock::time_point s_nextForget{};
    if (cNow >= s_nextForget)
    {
        // Copies no longer placed: their arrow went, or they did.
        s_nextForget = cNow + std::chrono::seconds(1);
        std::erase_if(s_arrowNodes, [cNow](const auto& acNodes) { return cNow - acNodes.second.UsedAt > std::chrono::seconds(1); });
    }

    ArrowNodes& nodes = s_arrowNodes[aFormId];
    nodes.UsedAt = cNow;
    if (cNow >= nodes.FindAgainAt || nodes.Before != acArrow.Before || !ArrowNodesIntact(nodes, apRoot))
    {
        nodes.FindAgainAt = cNow + std::chrono::milliseconds(250);
        if (!FindArrowNodes(apRoot, acArrow.Before, nodes))
        {
            nodes.FindAgainAt = {};
            nodes.Arrow = -1;
            return;
        }
    }
    const std::vector<ArrowNodes::Item>& items = nodes.Items;
    const std::array<void*, 2>& pHands = nodes.pHands;
    const std::array<void*, 2>& pStringEnds = nodes.pStringEnds;
    const int arrow = nodes.Arrow;

    const glm::vec3 cStringAt = ToGlm(At<NiTransform>(pHands[1 - aBowSide], kWorldOffset).translate);
    const glm::vec3 cBowAt = ToGlm(At<NiTransform>(pHands[aBowSide], kWorldOffset).translate);
    const glm::vec3 cAlong = cBowAt - cStringAt;
    void* pArrow = items[arrow].pObject;
    if (appPlaced)
        *appPlaced = pArrow;
    NiTransform current = At<NiTransform>(pArrow, kWorldOffset);
    glm::mat3 wanted{};
    glm::vec3 wantedAt{};
    if (acArrow.Hold.InHand)
    {
        // Where the owner holds it against the hand's third-person bone, which this copy's hand is posed from.
        const NiTransform& hand = At<NiTransform>(pHands[acArrow.Hold.Side & 1], kWorldOffset);
        const auto cHeld = HeldWeaponPlacement::InHand(ToGlm(hand.rotate), ToGlm(hand.translate), hand.scale, acArrow.Hold.Rotation, acArrow.Hold.Offset);
        wanted = cHeld.Rotation;
        wantedAt = cHeld.Position;
        if (!std::isfinite(wantedAt.x) || !std::isfinite(wanted[0][0]))
            return;
    }
    else
    {
        const glm::vec3 cFacing = ToGlm(At<NiTransform>(apRoot, kWorldOffset).rotate)[1];
        glm::vec3 aim{cFacing.x, cFacing.y, 0.f};
        if (pStringEnds[0] && pStringEnds[1])
        {
            // The unshifted string ends and the grip in the drawn bow form its aim plane.
            void* pGrip = At<void*>(pStringEnds[0], kParentOffset);
            for (uint32_t depth = 0; pGrip && depth < 6; ++depth, pGrip = At<void*>(pGrip, kParentOffset))
            {
                const char* pName = GetName(pGrip);
                if (!pName || std::strcmp(pName, "Bow_MidBone") != 0)
                    continue;
                const glm::vec3 cGrip = ToGlm(At<NiTransform>(pGrip, kWorldOffset).translate);
                if (glm::distance(cGrip, cBowAt) < 40.f && IsBelow(pStringEnds[1], pGrip))
                {
                    const auto cRest = [](void* apEnd)
                    {
                        const auto& parent = At<NiTransform>(At<void*>(apEnd, kParentOffset), kWorldOffset);
                        const auto& local = At<NiTransform>(apEnd, kLocalOffset);
                        return ToGlm(parent.translate) + ToGlm(parent.rotate) * (ToGlm(local.translate) * parent.scale);
                    };
                    if (const auto cAim = HeldWeaponPlacement::BowDirection(cGrip, cRest(pStringEnds[0]), cRest(pStringEnds[1])))
                        aim = *cAim;
                }
                break;
            }
        }
        HeldWeaponPlacement::Placement onString;
        if (!HeldWeaponPlacement::OnStringAimed(cStringAt, cBowAt, aim, kArrowNockDistance * current.scale, onString))
            return;
        wanted = onString.Rotation;
        wantedAt = onString.Position;
    }

    const glm::vec3 pivot = ToGlm(current.translate);
    const glm::mat3 delta = wanted * glm::transpose(ToGlm(current.rotate));
    NiTransform placed = current;
    placed.rotate = FromGlm(wanted);
    placed.translate = FromGlm(wantedAt);
    At<NiTransform>(pArrow, kWorldOffset) = placed;
    // Everything below the arrow node moves with it.
    for (size_t i = static_cast<size_t>(arrow) + 1; i < items.size(); ++i)
    {
        bool below = false;
        for (int up2 = items[i].Parent; up2 >= 0 && !below; up2 = items[up2].Parent)
            below = up2 == arrow;
        if (!below)
            continue;
        NiTransform& world = At<NiTransform>(items[i].pObject, kWorldOffset);
        world.rotate = FromGlm(delta * ToGlm(world.rotate));
        world.translate = FromGlm(ToGlm(placed.translate) + delta * (ToGlm(world.translate) - pivot));
    }

    // Draws the string back: the string's middle bone (Bow_StringBone2) and the bones below it move to the string hand.
    // The third-person draw animation does this, but a VR owner draws by hand and the copy has no animation for that.
    // Only within a draw's reach of the bow hand, so a pose that is not a draw (a nock, hands close together) leaves the string alone.
    float stringMoved = -1.f;
    for (size_t i = 0; i < items.size() && !acArrow.Hold.InHand && HeldWeaponPlacement::IsDraw(glm::length(cAlong)); ++i)
    {
        const char* pName = GetName(items[i].pObject);
        if (!pName || std::strcmp(pName, "Bow_StringBone2") != 0)
            continue;
        NiTransform& bone = At<NiTransform>(items[i].pObject, kWorldOffset);
        const glm::vec3 from = ToGlm(bone.translate);
        if (glm::distance(cStringAt, cBowAt) > 90.f || glm::distance(from, cBowAt) > 90.f)
            break;
        const glm::vec3 shift = cStringAt - from;
        bone.translate = FromGlm(cStringAt);
        for (size_t j = i + 1; j < items.size(); ++j)
        {
            bool below = false;
            for (int up2 = items[j].Parent; up2 >= 0 && !below; up2 = items[up2].Parent)
                below = up2 == static_cast<int>(i);
            if (below)
                At<NiTransform>(items[j].pObject, kWorldOffset).translate = FromGlm(ToGlm(At<NiTransform>(items[j].pObject, kWorldOffset).translate) + shift);
        }
        stringMoved = glm::length(shift);
        break;
    }

    // Once every 10 s, and at once when the arrow goes from the hand to the string or back.
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextSaid;
    static std::unordered_map<uint32_t, bool> s_saidInHand;
    if (s_nextSaid.size() > 256)
        s_nextSaid.clear();
    if (s_saidInHand.size() > 256)
        s_saidInHand.clear();
    auto& next = s_nextSaid[aFormId];
    const auto cSaid = s_saidInHand.find(aFormId);
    if (cNow >= next || cSaid == s_saidInHand.end() || cSaid->second != acArrow.Hold.InHand)
    {
        next = cNow + std::chrono::seconds(10);
        s_saidInHand[aFormId] = acArrow.Hold.InHand;
        if (acArrow.Hold.InHand)
            spdlog::info(
                "ArrowNocked: {:X} arrow held in the {} hand as its owner holds it, its head {:.0f} units from the hand (hand scale {:.2f}), turned {:.0f} degrees from "
                "where the game had it",
                aFormId, acArrow.Hold.Side ? "right" : "left", glm::distance(wantedAt, ToGlm(At<NiTransform>(pHands[acArrow.Hold.Side & 1], kWorldOffset).translate)),
                At<NiTransform>(pHands[acArrow.Hold.Side & 1], kWorldOffset).scale, DegreesOf(delta));
        else
            spdlog::info(
                "ArrowNocked: {:X} arrow laid from the {} hand to the bow, {:.0f} units, turned {:.0f} degrees from where the game had it; string {}", aFormId,
                aBowSide ? "left" : "right", glm::length(cAlong), DegreesOf(delta),
                stringMoved >= 0.f ? fmt::format("drawn back {:.0f} units to the hand", stringMoved) : std::string("left alone (no string bone, or not a draw's reach)"));
    }
}

/**
 * Place each copy's nocked arrow as its owner has it, and draw its bow string back.
 * An arrow held in a hand goes where the owner holds it against that hand; any other lies on the string, along the line from the string hand to the bow hand.
 * With the arrow on the string and the hands within a draw's reach, the bow's string bone is moved to the string hand.
 * A copy whose arrow or hands cannot be found is left as the game has it.
 */
void PlaceNockedArrows() noexcept
{
    std::vector<std::pair<uint32_t, CopyArrow>> ids;
    {
        std::scoped_lock lock(s_nockedLock);
        ids.assign(s_nocked.begin(), s_nocked.end());
    }
    for (const auto& [cFormId, cArrow] : ids)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(cFormId));
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        if (!pRoot)
            continue;

        // The bow is in the hand the arrow was not taken in; else where the owner holds a weapon; else the left.
        bool hasWeapons = false;
        std::array<bool, 2> held{};
        {
            std::shared_lock lock(s_posesLock);
            if (const auto it = s_poses.find(cFormId); it != s_poses.end() && it->second.Available(std::chrono::steady_clock::now()))
            {
                hasWeapons = it->second.HasWeapons;
                held = it->second.WeaponHeld;
            }
        }
        PlaceCopyArrow(cFormId, cArrow, pRoot, HeldWeaponPlacement::BowSide(cArrow.StringSide, hasWeapons, held[0], held[1]));
    }
}

namespace
{
// A copy's hands, its weapon attach nodes ("SHIELD" left, "WEAPON" right) and what hangs below them.
// Found once per 3D, then checked each frame with plain reads (no virtual call, no walk of the tree).
struct WeaponNodes
{
    void* pRoot = nullptr;
    std::array<RigBone, 2> Hands{};
    std::array<void*, 2> HandParents{};
    std::array<RigBone, 2> Attach{};
    std::array<void*, 2> AttachParents{};
    std::array<uint64_t, 2> AttachChildren{};
    std::array<std::vector<RigBone>, 2> Below{};
    // For each node below, the index of its parent in Below; -1 for the attach node. Parents come before their children.
    std::array<std::vector<int32_t>, 2> BelowParent{};
    std::array<std::vector<uint64_t>, 2> BelowChildren{};
};

struct HeldWeapons
{
    WeaponNodes Nodes;
    std::chrono::steady_clock::time_point FindAgainAt{};
    // The worlds written at the last placement, to tell whether any still holds that value once placement stops.
    std::vector<std::pair<NiTransform*, NiTransform>> Written;
    // The pass before's, in the same order, to tell a world the game has not updated since.
    std::vector<std::pair<NiTransform*, NiTransform>> LastWritten;
};

// Used from the flat presenter's commit, which runs one at a time; a copy that goes is dropped from the game thread (ForgetCorpse).
std::mutex s_heldWeaponsLock;
std::unordered_map<uint32_t, HeldWeapons> s_heldWeapons;

// Further than this from where the body has the attach node, a grip is a bad read, not a hold.
constexpr float kWeaponGripReach = 30.f;

// Every child pointer of a node, so a child swapped for another in the same slot is noticed too. Plain reads.
uint64_t ChildrenHashOf(void* apChildOwner) noexcept
{
    if (!apChildOwner)
        return 0;
    void** pChildren = At<void**>(apChildOwner, kChildrenOffset + 0x8);
    const uint16_t slots = At<uint16_t>(apChildOwner, kChildrenOffset + 0x10);
    uint64_t hash = 1469598103934665603ull ^ slots;
    for (uint16_t i = 0; pChildren && i < slots; ++i)
        hash = (hash ^ reinterpret_cast<uintptr_t>(pChildren[i])) * 1099511628211ull;
    return hash;
}

void FindWeaponNodes(void* apRoot, WeaponNodes& aOut) noexcept
{
    aOut = WeaponNodes{};
    aOut.pRoot = apRoot;
    for (size_t side = 0; side < 2; ++side)
    {
        if (void* pHand = FindShallowest(apRoot, kHandNames[side]))
        {
            aOut.Hands[side] = MakeRigBone(pHand, nullptr);
            aOut.HandParents[side] = At<void*>(pHand, kParentOffset);
        }
        void* pAttach = FindShallowest(apRoot, kAttachNames[side]);
        if (!pAttach || !AsNode(pAttach) || !At<void*>(pAttach, kParentOffset))
            continue;
        std::vector<void*> nodes;
        CollectNodeDescendants(pAttach, nodes);
        if (nodes.size() > 512)
            continue;
        std::vector<RigBone> below;
        std::vector<int32_t> parents;
        std::vector<uint64_t> children;
        below.reserve(nodes.size());
        parents.reserve(nodes.size());
        children.reserve(nodes.size());
        bool known = true;
        for (size_t i = 0; i < nodes.size() && known; ++i)
        {
            void* pParent = At<void*>(nodes[i], kParentOffset);
            int32_t parent = pParent == pAttach ? -1 : -2;
            for (size_t j = i; parent == -2 && j-- > 0;)
                if (nodes[j] == pParent)
                    parent = static_cast<int32_t>(j);
            known = parent != -2;
            below.push_back(MakeRigBone(nodes[i], nullptr));
            parents.push_back(parent);
            children.push_back(ChildrenHashOf(below.back().pChildOwner));
        }
        if (!known)
            continue;
        aOut.Attach[side] = MakeRigBone(pAttach, nullptr);
        aOut.AttachParents[side] = At<void*>(pAttach, kParentOffset);
        aOut.AttachChildren[side] = ChildrenHashOf(aOut.Attach[side].pChildOwner);
        aOut.Below[side] = std::move(below);
        aOut.BelowParent[side] = std::move(parents);
        aOut.BelowChildren[side] = std::move(children);
    }
}

// Still the nodes FindWeaponNodes found: the same objects, under the same parents, holding the same children.
// Parents are checked before their children, so a node let go of is never read: its parent's children changed first.
bool WeaponNodesIntact(const WeaponNodes& acNodes) noexcept
{
    const auto cUnder = [](const RigBone& acBone, void* apParent)
    {
        return acBone.IsIntact() && At<void*>(acBone.pNode, kParentOffset) == apParent;
    };
    for (size_t side = 0; side < 2; ++side)
    {
        if (acNodes.Hands[side].pNode && !cUnder(acNodes.Hands[side], acNodes.HandParents[side]))
            return false;
        const RigBone& attach = acNodes.Attach[side];
        if (!attach.pNode)
            continue;
        if (!cUnder(attach, acNodes.AttachParents[side]) || ChildrenHashOf(attach.pChildOwner) != acNodes.AttachChildren[side])
            return false;
        const auto& below = acNodes.Below[side];
        const auto& parents = acNodes.BelowParent[side];
        const auto& children = acNodes.BelowChildren[side];
        for (size_t i = 0; i < below.size(); ++i)
            if (!cUnder(below[i], parents[i] < 0 ? attach.pNode : below[static_cast<size_t>(parents[i])].pNode) || ChildrenHashOf(below[i].pChildOwner) != children[i])
                return false;
    }
    return true;
}

HeldWeaponPlacement::Frame FrameOf(const NiTransform& acTransform) noexcept
{
    return {ToGlm(acTransform.rotate), ToGlm(acTransform.translate), acTransform.scale};
}

NiTransform TransformOf(const HeldWeaponPlacement::Frame& acFrame) noexcept
{
    NiTransform transform{};
    transform.rotate = FromGlm(acFrame.Rotation);
    transform.translate = FromGlm(acFrame.Position);
    transform.scale = acFrame.Scale;
    return transform;
}

bool FrameIsFinite(const HeldWeaponPlacement::Frame& acFrame) noexcept
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(acFrame.Position[i]) || !std::isfinite(acFrame.Rotation[i][0]) || !std::isfinite(acFrame.Rotation[i][1]) || !std::isfinite(acFrame.Rotation[i][2]))
            return false;
    return std::isfinite(acFrame.Scale);
}

// World bounds (flat only, NiAVObject::worldBound at +E4 on SE). The game culls by the bounds its own update left around
// where it hung each item; an item moved after that update needs its bound, and those above it, to hold where it is now.
// A union only grows, so nothing the game's bound drew can be culled. The game's next update rebuilds them.
using BoundSphere = BodyPoseBounds::Sphere;

uint32_t GrowBound(void* apObject, const BoundSphere& acSphere) noexcept
{
#ifdef SKYRIMVR
    (void)apObject;
    (void)acSphere;
    return 0;
#else
    BoundSphere& bound = At<BoundSphere>(apObject, BodyPoseBounds::kNodeWorldBound);
    BoundSphere grown{};
    if (!BodyPoseBounds::Union(bound, acSphere, grown) || BodyPoseBounds::SameBits(grown, bound))
        return 0;
    bound = grown;
    return 1;
#endif
}

// From an object up to the copy's root (included), each bound grown to hold acSphere.
// Only when the walk up reaches the root: an object no longer under the copy leaves every bound alone.
uint32_t GrowBoundsUp(void* apFrom, void* apRoot, const BoundSphere& acSphere) noexcept
{
    std::array<void*, 64> path{};
    size_t count = 0;
    for (void* pAt = apFrom; pAt && count < path.size(); pAt = At<void*>(pAt, kParentOffset))
    {
        path[count++] = pAt;
        if (pAt == apRoot)
            break;
    }
    if (count == 0 || path[count - 1] != apRoot)
        return 0;
    uint32_t stores = 0;
    for (size_t i = 0; i < count; ++i)
        stores += GrowBound(path[i], acSphere);
    return stores;
}

// Writes a world, and moves the object's bound with it (kept as a union with the bound the game left).
// aMoved collects every moved bound, for the bounds above.
void WriteWeaponWorld(HeldWeapons& aHeld, void* apNode, const HeldWeaponPlacement::Frame& acWorld, BoundSphere& aMoved, bool& aHaveMoved) noexcept
{
    NiTransform& world = At<NiTransform>(apNode, kWorldOffset);
#ifndef SKYRIMVR
    // A world still holding what the last pass wrote was not updated by the game since, nor was its bound, which is
    // already the grown one: moving and growing it again would widen it every frame.
    const size_t cIndex = aHeld.Written.size();
    const bool cUpdated =
        cIndex >= aHeld.LastWritten.size() || aHeld.LastWritten[cIndex].first != &world || std::memcmp(&aHeld.LastWritten[cIndex].second, &world, sizeof(NiTransform)) != 0;
    const BoundSphere cBound = At<BoundSphere>(apNode, BodyPoseBounds::kNodeWorldBound);
    if (cUpdated && BodyPoseBounds::Usable(cBound) && world.scale > 0.f && std::isfinite(world.scale))
    {
        const glm::vec3 cLocal = glm::transpose(ToGlm(world.rotate)) * ((glm::vec3{cBound[0], cBound[1], cBound[2]} - ToGlm(world.translate)) / world.scale);
        const glm::vec3 cCentre = acWorld.Position + acWorld.Rotation * (cLocal * acWorld.Scale);
        const BoundSphere cMoved{cCentre.x, cCentre.y, cCentre.z, cBound[3] * acWorld.Scale / world.scale};
        if (BodyPoseBounds::Usable(cMoved))
        {
            GrowBound(apNode, cMoved);
            BoundSphere both{};
            if (!aHaveMoved)
            {
                aMoved = cMoved;
                aHaveMoved = true;
            }
            else if (BodyPoseBounds::Union(aMoved, cMoved, both))
                aMoved = both;
        }
    }
#endif
    world = TransformOf(acWorld);
    aHeld.Written.emplace_back(&world, world);
}
} // namespace

uint32_t PlaceHeldWeapons(uint32_t aFormId, void* apRoot, const VRPose& acPose, const bool aFindAgain) noexcept
{
    if (!apRoot)
        return 0;

    const auto cStarted = std::chrono::steady_clock::now();
    std::scoped_lock heldLock(s_heldWeaponsLock);
    HeldWeapons& held = s_heldWeapons[aFormId];
    std::swap(held.LastWritten, held.Written);
    held.Written.clear();
    bool usable = !aFindAgain && held.Nodes.pRoot == apRoot && WeaponNodesIntact(held.Nodes);
    // A side found without its attach node (not made yet, or too much below it) is looked for again every 2 s.
    const bool cSideMissing = !held.Nodes.Attach[0].pNode || !held.Nodes.Attach[1].pNode;
    if ((!usable || cSideMissing) && (aFindAgain || held.Nodes.pRoot != apRoot || cStarted >= held.FindAgainAt))
    {
        FindWeaponNodes(apRoot, held.Nodes);
        const bool cStillMissing = !held.Nodes.Attach[0].pNode || !held.Nodes.Attach[1].pNode;
        held.FindAgainAt = cStarted + (cStillMissing ? std::chrono::milliseconds(2000) : std::chrono::milliseconds(250));
        usable = true;
    }

    // Both sides every frame, from their own locals: the attach node from its parent, or at the owner's grip on a held side,
    // then everything below it. The body's commit leaves all of it to this, so each is written once a frame.
    for (size_t side = 0; usable && side < 2; ++side)
    {
        const RigBone& attach = held.Nodes.Attach[side];
        if (!attach.pNode)
            continue;
        // The body's commit leaves the attach node to this: from its parent (the hand as the body has it now) and its own local.
        HeldWeaponPlacement::Frame attachWorld =
            HeldWeaponPlacement::Compose(FrameOf(At<NiTransform>(held.Nodes.AttachParents[side], kWorldOffset)), FrameOf(At<NiTransform>(attach.pNode, kLocalOffset)));
        if (!FrameIsFinite(attachWorld))
            continue;
        const RigBone& hand = held.Nodes.Hands[side];
        if (acPose.HasWeapons && acPose.WeaponHeld[side] && hand.pNode)
        {
            const NiTransform& handWorld = hand.World();
            const glm::vec3 cOffset{acPose.WeaponOffset[side][0], acPose.WeaponOffset[side][1], acPose.WeaponOffset[side][2]};
            const auto cHeld = HeldWeaponPlacement::InHand(ToGlm(handWorld.rotate), ToGlm(handWorld.translate), handWorld.scale, acPose.WeaponRotation[side], cOffset);
            const float cMoved = glm::distance(cHeld.Position, attachWorld.Position);
            if (std::isfinite(cMoved) && FrameIsFinite({cHeld.Rotation, cHeld.Position, attachWorld.Scale}) && cMoved <= kWeaponGripReach)
            {
                attachWorld.Rotation = cHeld.Rotation;
                attachWorld.Position = cHeld.Position;
            }
        }
        BoundSphere moved{};
        bool haveMoved = false;
        WriteWeaponWorld(held, attach.pNode, attachWorld, moved, haveMoved);

        const auto& below = held.Nodes.Below[side];
        const auto& parents = held.Nodes.BelowParent[side];
        for (size_t i = 0; i < below.size(); ++i)
        {
            const HeldWeaponPlacement::Frame cParent = parents[i] < 0 ? attachWorld : FrameOf(below[static_cast<size_t>(parents[i])].World());
            const HeldWeaponPlacement::Frame cWorld = HeldWeaponPlacement::Compose(cParent, FrameOf(At<NiTransform>(below[i].pNode, kLocalOffset)));
            if (!FrameIsFinite(cWorld))
                continue;
            struct NodeWrite
            {
                HeldWeapons& Held;
                void* Node;
                const HeldWeaponPlacement::Frame& Frame;
                const glm::vec3& Hand;
                BoundSphere& Moved;
                bool& HaveMoved;
                bool Written{};
            } write{held, below[i].pNode, cWorld, attachWorld.Position, moved, haveMoved};
            GameExtension::PresentNode(
                aFormId, static_cast<uint32_t>(side), apRoot, write.Node, &write,
                [](void* apContext, const float aPosition[3], const float aRotation[4]) -> uint32_t
                {
                    auto& output = *static_cast<NodeWrite*>(apContext);
                    if (output.Written || !aPosition || !aRotation)
                        return 0;
                    glm::quat rotation{aRotation[3], aRotation[0], aRotation[1], aRotation[2]};
                    if (!(std::abs(glm::dot(rotation, rotation) - 1.f) <= .01f))
                        return 0;
                    HeldWeaponPlacement::Frame pose{glm::mat3_cast(rotation), {aPosition[0], aPosition[1], aPosition[2]}, output.Frame.Scale};
                    if (!FrameIsFinite(pose) || !(glm::distance(pose.Position, output.Hand) <= 256.f))
                        return 0;
                    WriteWeaponWorld(output.Held, output.Node, pose, output.Moved, output.HaveMoved);
                    output.Written = true;
                    return 1;
                });
            if (!write.Written)
                WriteWeaponWorld(held, write.Node, cWorld, moved, haveMoved);
        }
        // Each moved bound is already in its own object's; every node above, from the attach node up, holds them all.
        if (haveMoved)
            GrowBoundsUp(attach.pNode, apRoot, moved);
    }

    const auto cStores = static_cast<uint32_t>(held.Written.size());

    CopyArrow arrow;
    {
        std::scoped_lock lock(s_nockedLock);
        const auto it = s_nocked.find(aFormId);
        if (it == s_nocked.end())
            return cStores;
        arrow = it->second;
    }
    const size_t cBowSide = HeldWeaponPlacement::BowSide(arrow.StringSide, acPose.HasWeapons, acPose.WeaponHeld[0], acPose.WeaponHeld[1]);
    void* pArrow = nullptr;
    PlaceCopyArrow(aFormId, arrow, apRoot, cBowSide, &pArrow);
#ifndef SKYRIMVR
    if (pArrow)
    {
        // The arrow runs from its head (its origin) about kArrowNockDistance back along -y: a sphere over that length.
        const NiTransform& cWorld = At<NiTransform>(pArrow, kWorldOffset);
        const glm::vec3 cMiddle = ToGlm(cWorld.translate) - ToGlm(cWorld.rotate) * (kArrowForward * (0.5f * kArrowNockDistance * cWorld.scale));
        const BoundSphere cArrow{cMiddle.x, cMiddle.y, cMiddle.z, (0.5f * kArrowNockDistance + 8.f) * cWorld.scale};
        if (BodyPoseBounds::Usable(cArrow))
        {
            std::vector<void*> nodes;
            CollectNodeDescendants(pArrow, nodes);
            for (size_t i = 0; i < nodes.size() && i < 64; ++i)
                GrowBound(nodes[i], cArrow);
            GrowBoundsUp(pArrow, apRoot, cArrow);
        }
    }
#endif
    return cStores;
}

uint32_t ReleaseHeldWeapons(uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_heldWeaponsLock);
    const auto it = s_heldWeapons.find(aFormId);
    if (it == s_heldWeapons.end() || it->second.Written.empty())
        return 0;
    // Nothing is written back: the game composes these nodes again from their locals, as it does the rest of the body.
    // Only counted, as the presenter does its rigid stores: a world still holding what was placed means it was not.
    uint32_t stale = 0;
    for (const auto& [pWorld, cPlaced] : it->second.Written)
        stale += IsReadable(pWorld, sizeof(NiTransform)) && std::memcmp(pWorld, &cPlaced, sizeof(NiTransform)) == 0 ? 1u : 0u;
    it->second.Written.clear();
    return stale;
}

void SetArrowNocked(uint32_t aFormId, bool aNocked, const ArrowHold& acHold) noexcept
{
    const bool cWanted = aNocked || acHold.InHand;
    {
        // An arrow already there only changes how it is held: the nodes from before it was attached still tell it apart.
        std::scoped_lock lock(s_nockedLock);
        if (const auto it = s_nocked.find(aFormId); it != s_nocked.end() && cWanted)
        {
            it->second.Hold = aNocked ? ArrowHold{} : acHold;
            if (acHold.InHand)
                it->second.StringSide = acHold.Side & 1;
            return;
        }
    }
    std::vector<void*> before;
    if (cWanted)
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId)); pActor && pActor->GetNiNode())
            before = ArrowNodesOf(pActor->GetNiNode());
    std::scoped_lock lock(s_nockedLock);
    if (cWanted)
        s_nocked[aFormId] = CopyArrow{std::move(before), aNocked ? ArrowHold{} : acHold, acHold.InHand ? acHold.Side & 1 : -1};
    else
        s_nocked.erase(aFormId);
}

bool HasCopyArrow(uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_nockedLock);
    return s_nocked.contains(aFormId);
}

void OnFrameEnd() noexcept
{
    PerfCounterScope frameEndScope(PerfCounter::kVRFrameEnd);

    // The places the interpolation kept so far are this frame end's; one kept while it runs waits for the next (PosedCopyPlacement).
    PosedCopyPlacement::StartFrameEnd();

    /**
     * Gives, on the way out whichever way this returns, the places taken for copies not posed in this frame.
     */
    struct PlacesLast
    {
        /**
         * Give the places left (PosedCopyPlacement::FinishFrameEnd).
         */
        ~PlacesLast() { PosedCopyPlacement::FinishFrameEnd(); }
    } placesLast;

#ifdef SKYRIMVR
    s_frameEnds.fetch_add(1, std::memory_order_relaxed);
    // The nocked arrows go last, once every hand is where it ends up this frame, whichever way this returns.
    struct ArrowsLast
    {
        ~ArrowsLast() { PlaceNockedArrows(); }
    } arrowsLast;
#endif

    TiltedPhoques::Vector<std::pair<uint32_t, NativeRemotePose>> poses;
    {
        std::shared_lock lock(s_posesLock);
        const auto cNow = std::chrono::steady_clock::now();
        // Keep expired entries as source-tick watermarks: replaying a held sample must not revive them.
        // A flat player's aim-only entry stays in the cache for spell hooks, never in the body drawer.
        for (const auto& [formId, pose] : s_poses)
            if (pose.Available(cNow) && SpellAimRules::NeedsBodyPresentation(pose.NoBones, pose.HasRootPosition))
                poses.emplace_back(formId, pose);
    }

    // Skeletons of players who left.
    for (auto it = s_rigs.begin(); it != s_rigs.end();)
    {
        const uint32_t formId = it->first;
        const bool stillPosed = std::any_of(poses.begin(), poses.end(), [formId](const auto& acEntry) { return acEntry.first == formId; });
        it = stillPosed ? std::next(it) : s_rigs.erase(it);
    }
    // And which hkbCharacter each had.
    std::erase_if(
        s_characterLinks,
        [&poses](const auto& acLink) { return std::none_of(poses.begin(), poses.end(), [&acLink](const auto& acEntry) { return acEntry.first == acLink.first; }); });

    s_copyBlades.clear();
    if (poses.empty())
    {
#ifdef SKYRIMVR
        StopLocalBlades(std::chrono::steady_clock::now());
        ReleaseUnseenWeaponBodies();
#endif
        return;
    }

    PerfCounterScope perfScope(PerfCounter::kVRPoseApply);
    const auto now = std::chrono::steady_clock::now();

    // TEMPORARY, see WatchPoseSteps: for each remote player's copy, the frame ends it was posed at, and why not at the others.
    // Logged every 10 s for a copy posed at some frame ends and not at others.
    struct FrameEndCount
    {
        uint32_t Frames = 0;
        uint32_t Posed = 0;
        uint32_t NoBody = 0;
        uint32_t Down = 0;
        uint32_t Skeleton = 0;
        uint32_t OutOfView = 0;
        uint32_t Unusable = 0;
        // How often it went from posed to not posed or back, and at how many frame ends the view check at its bones would have decided otherwise.
        uint32_t Switches = 0;
        uint32_t BonesViewDiffers = 0;
        bool WasPosed = false;
        std::chrono::steady_clock::time_point NextLog{};
    };
    // TEMPORARY: counts a switch whichever way the frame end below leaves the copy.
    struct SwitchCount
    {
        FrameEndCount* pCount = nullptr;
        bool Posed = false;
        ~SwitchCount()
        {
            if (!pCount)
                return;
            if (pCount->Frames > 1 && Posed != pCount->WasPosed)
                ++pCount->Switches;
            pCount->WasPosed = Posed;
        }
    };
    static std::unordered_map<uint32_t, FrameEndCount> s_frameEndCounts;
    if (s_frameEndCounts.size() > 256)
        s_frameEndCounts.clear();
    // TEMPORARY (PoseKeepDiag): this frame end's number, so a copy is compared only with the frame end right before.
    static uint64_t s_frameEndNumber = 0;
    ++s_frameEndNumber;

    for (const auto& [formId, pose] : poses)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        FrameEndCount* pCount = nullptr;
        if (pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
        {
            pCount = &s_frameEndCounts[formId];
            if (pCount->NextLog == std::chrono::steady_clock::time_point{})
                pCount->NextLog = now + std::chrono::seconds(10);
            else if (now >= pCount->NextLog)
            {
                if (pCount->Posed && pCount->Posed < pCount->Frames)
                    spdlog::info("PoseFlipDiag: copy {:X} posed at {} of {} frame ends in 10 s; not posed: no 3D {}, dead or down {}, skeleton not matched {}, out of view {}, "
                                 "a bone the renderer cannot use {}; switched between posed and not {} times; the view check at its bones would have decided otherwise at {}",
                                 formId, pCount->Posed, pCount->Frames, pCount->NoBody, pCount->Down, pCount->Skeleton, pCount->OutOfView, pCount->Unusable, pCount->Switches,
                                 pCount->BonesViewDiffers);
                const bool cWasPosed = pCount->WasPosed;
                *pCount = FrameEndCount{};
                pCount->WasPosed = cWasPosed;
                pCount->NextLog = now + std::chrono::seconds(10);
            }
            ++pCount->Frames;
        }
        SwitchCount switchCount{pCount};
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        if (!pRoot)
        {
            if (pCount)
                ++pCount->NoBody;
            continue;
        }

        // A dead or downed player copy belongs to its ragdoll: posing it fights the death animation and stands it in the air.
        // A dead NPC's owner sends bones only while moving it (dragged, thrown, shoved), so those bones are applied.
        // isBody marks a dead body moved by its owner; for one, each reason the pose is skipped below is logged.
        if (pose.HasProvenance && !pose.Live && !SparseBodyEligible(pActor))
        {
            VRBodySync::ClearRemotePoseForGeneration(formId, pose.Generation);
            continue; // A recovered NPC is driven by its animation, including between main-thread updates.
        }
        bool isBody = false;
        const auto whyNotPosed = [&isBody, &now, formId](const char* acpWhy, const float aValue = 0.f)
        {
            if (!isBody)
                return;
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextWhy;
            if (s_nextWhy.size() > 256)
                s_nextWhy.clear();
            auto& next = s_nextWhy[formId];
            if (now < next)
                return;
            next = now + std::chrono::seconds(5);
            spdlog::info("VRBodySync: body {:X} not posed this time: {} ({:.0f})", formId, acpWhy, aValue);
        };

        // An NPC knocked into its ragdoll is a body too while its owner carries it (AnimationSystem::SerializeMovement).
        const ActorExtension* pBodyExtension = pActor->GetExtension();
        const bool cKnockedNpc = pBodyExtension && !pBodyExtension->IsRemotePlayer() && !pBodyExtension->IsLocalPlayer() && pActor->actorState.IsInKnockRagdoll() &&
                                 !pActor->actorState.IsBleedingOut();
#ifndef SKYRIMVR
        // On flat only remote players are posed: an NPC's body sent by its owner is placed through VR's physics (PlaceCorpse), which flat lacks.
        // The copies SEBodyPresentation commits natively are posed there.
        if (!pBodyExtension || !pBodyExtension->IsRemotePlayer() || SEBodyPresentation::Presents(formId))
            continue;
#endif
        if (pActor->actorState.IsDeadOrDying() || pActor->actorState.IsBleedingOut() || cKnockedNpc)
        {
            if (pCount)
                ++pCount->Down;
            if (!pBodyExtension || pBodyExtension->IsRemotePlayer() || pBodyExtension->IsLocalPlayer())
                continue;

            // Only a properly dead body or one lying in its ragdoll: a downed one gets back up, and a dying one is playing its death animation.
            // Writing bones into either fights the game. The sender does not send for those, and this refuses them from an older client.
            if (!pActor->actorState.IsDead() && !cKnockedNpc)
                continue;
            isBody = true;
            {
                std::scoped_lock lock(s_bodyPosedLock);
                s_bodyPosedByOwnerAt[formId] = now;
            }

            // The other half of the sender's line, so one log shows the whole chain.
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextSaid;
            if (s_nextSaid.size() > 256)
                s_nextSaid.clear();
            auto& next = s_nextSaid[formId];
            if (now >= next)
            {
                next = now + std::chrono::seconds(5);
                spdlog::info("VRBodySync: posing body {:X} from its owner's bones (someone is moving it over there)", formId);
            }
        }

        Rig& rig = s_rigs[formId];
        const bool cGone = rig.pRoot != pRoot || !rig.Valid || !RigIntact(rig);
        // Only worth asking once the rig is known good, and not more than a few times a second.
        const bool cRestructured = !cGone && now >= rig.RestructureAt && !RigStructureUnchanged(rig);

        // A body with no readable skeleton is sent as a position only, and needs no rig.
        // The whole tree is shifted by however far the owner has it from where this side does.
        if (pose.NoBones)
        {
            // Its ragdoll is set where its owner has each of its bodies (FollowCarriedBodies), and the tree follows it.
            if (!pose.HasRootPosition || FollowedNow(formId))
                continue;

            const glm::vec3 wanted{pose.RootPosition[0], pose.RootPosition[1], pose.RootPosition[2]};
            if (!IsFiniteVector(wanted))
                continue;

            const glm::vec3 offset = wanted - ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
            // Half a cell apart is a desync, not a drag.
            if (glm::dot(offset, offset) > 2048.f * 2048.f || glm::dot(offset, offset) < 0.0001f)
                continue;

            ShiftTree(pRoot, offset);

            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextBoneless;
            if (s_nextBoneless.size() > 256)
                s_nextBoneless.clear();
            auto& nextBoneless = s_nextBoneless[formId];
            if (now >= nextBoneless)
            {
                nextBoneless = now + std::chrono::seconds(5);
                spdlog::info("VRBodySync: body {:X} has no humanoid skeleton; moved its whole tree {:.1f} units to where its owner has it", formId, glm::length(offset));
            }
            continue;
        }

        if (cGone || cRestructured)
        {
            if (cGone && rig.pRoot == pRoot && now < rig.RetryAt)
            {
                if (pCount)
                    ++pCount->Skeleton;
                continue;
            }
            if (!ResolveRig(pRoot, rig, cGone))
            {
                if (pCount)
                    ++pCount->Skeleton;
                rig.RetryAt = now + std::chrono::seconds(1);
                whyNotPosed("its skeleton could not be matched to the bones being sent");
                continue;
            }
            rig.RestructureAt = now + std::chrono::milliseconds(200);
        }

        // A body being moved over there is checked for view where its owner has it, not at its own bones.
        // Its bones are still where its ragdoll left it here, so it would vanish whenever that old spot is out of view.
        // A live copy is checked where it is drawn (DrawnChest).
        const glm::vec3 cBonesChest = ToGlm(rig.Bones[VRPose::kSpine2].World().translate);
        glm::vec3 viewPoint = isBody ? cBonesChest : DrawnChest(pRoot, pose);
        if (isBody && pose.HasRootPosition && IsFiniteVector(pose.RootPosition))
            viewPoint += pose.RootPosition - ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
        // Once in view, a copy stays posed until it is 10 degrees past the cone's edge.
        // Without that margin, a copy at the edge is posed and let go at each small turn of the head.
        constexpr float cLeaveMarginDegrees = 10.f;
        const bool cInView = IsInView(viewPoint, rig.InView ? cLeaveMarginDegrees : 0.f);
        // TEMPORARY, see FrameEndCount: what the check at the copy's bones, with no margin, would have said.
        if (pCount && !isBody && IsInView(cBonesChest) != cInView)
            ++pCount->BonesViewDiffers;
        rig.InView = cInView;
        if (!cInView)
        {
#ifdef SKYRIMVR
            // Its weapon body stays in the world out of view too, where the game's animation has it.
            // Taking it out and back in each time the copy leaves view would be churn for nothing.
            UpdateWeaponBodies(formId, pActor, rig);
#endif
            whyNotPosed("out of view");
            if (pCount)
                ++pCount->OutOfView;
            continue;
        }

        // A body whose bones are no longer usable is not drawn, and writing our transforms into it keeps it that way.
        // So it is let go: the game's animation rewrites those bones within a frame and the body comes back.
        // Logged once every ten seconds per actor.
        UnusableBone unusable;
        if (!IsBodyUsable(rig, unusable))
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextComplaint;
            if (s_nextComplaint.size() > 256)
                s_nextComplaint.clear();
            auto& next = s_nextComplaint[formId];
            if (now >= next)
            {
                next = now + std::chrono::seconds(10);
                const auto& world = unusable.World;
                const glm::mat3 rotation = ToGlm(world.rotate);
                // A real rotation's columns are all 1 long.
                const glm::vec3 columnLengths(glm::length(rotation[0]), glm::length(rotation[1]), glm::length(rotation[2]));
                const char* boneName = unusable.Name ? unusable.Name : "none";

                spdlog::debug(
                    "VRBodySync: actor {:X} has a bone the renderer cannot use "
                    "({}: scale {:.3f}, "
                    "rotation column lengths ({:.4f}), "
                    "at ({:.1f})); "
                    "not posing it until the game puts it right",
                    formId, boneName, world.scale, columnLengths, world.translate);
            }
            whyNotPosed("a bone the renderer cannot use");
            if (pCount)
                ++pCount->Unusable;
            continue;
        }

        // TEMPORARY (PoseKeepDiag): whether the bones are still where the last frame end put them.
        // A pelvis that moved less than this since the last frame end was left alone.
        constexpr float cLeftAloneWithin = 0.5f;
        const RigBone& keptPelvis = rig.Bones[VRPose::kPelvis];
        const bool cWatchKeep = pCount && !isBody && (keptPelvis.pNode || keptPelvis.pEntry);
        const bool cPosedJustBefore = cWatchKeep && rig.PelvisLeftAt != 0 && rig.PelvisLeftAt + 1 == s_frameEndNumber;
        const bool cStillWhereLeft = cPosedJustBefore && glm::distance(ToGlm(keptPelvis.World().translate), rig.PelvisLeft) < cLeftAloneWithin;

        // Its place for this frame, kept by the interpolation, is given now, so the pose below is the last thing written to it before it is drawn.
        PosedCopyPlacement::GiveKeptPlace(pActor);

        // The root is placed first, so the bone world transforms below are built on the right origin.
        // Like the bones, it is written at the renderer's frame end, after the local ragdoll's turn, so the ragdoll cannot drag it back.
        // worldOffset is how far the whole body moves to be where its owner has it.
        // PoseActor applies it to every bone, because the renderer follows the bones and not the root.
        glm::vec3 worldOffset{};
        if (pose.HasRootPosition)
        {
            const glm::vec3& wanted = pose.RootPosition;
            if (IsFiniteVector(wanted))
            {
                NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                worldOffset = wanted - ToGlm(rootWorld.translate);
                // A body half a cell away is a desync, not a drag; hauling the skeleton that far would look worse than leaving it.
                if (glm::dot(worldOffset, worldOffset) > 2048.f * 2048.f)
                {
                    // Bent but not moved. Each side's ragdoll drops a corpse on its own, and the two can land far apart.
                    whyNotPosed("more than 2048 units from where its owner has it, so bent in place and not moved; units apart",
                                glm::length(worldOffset));
                    worldOffset = glm::vec3{};
                }
                else
                    rootWorld.translate = wanted; // NiPoint3 derives from glm::vec3
            }
        }

        // Hips. The pose is rotations, so a crouch or a lean, which moves the body against its root, needs this offset too.
        // Moving every bone puts the hip joint where the sender's is, and the leg rotations then swing the feet from there.
        // The whole body moves: the spine hangs off NPC COM beside the pelvis, so moving only the pelvis would pull the legs off the torso.
        if (pose.HasHips)
        {
            const RigBone& pelvis = rig.Bones[VRPose::kPelvis];
            if (pelvis.pNode || pelvis.pEntry)
            {
                const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                const glm::vec3 wanted = ToGlm(rootWorld.translate) + ToGlm(rootWorld.rotate) * pose.HipOffset;
                glm::vec3 delta = wanted - ToGlm(pelvis.World().translate);
                // Without leg trackers, only where the body stands is used, not its height.
                // The legs are this copy's walk animation, so lowering it to the owner's crouch would sink the feet.
                // The owner's VRIK body stands behind its root, so this also puts the copy where it can be hit.
                if (!pose.HasLegs)
                {
                    const glm::mat3 rootRotation = ToGlm(rootWorld.rotate);
                    glm::vec3 local = glm::transpose(rootRotation) * delta;
                    local.z = 0.f;
                    delta = rootRotation * local;
                }
                // A hip a body-length away from where this copy has it is a bad read, not a crouch.
                if (IsFiniteVector(delta) && glm::dot(delta, delta) < 256.f * 256.f)
                    worldOffset += delta;
            }
        }

#ifdef SKYRIMVR
        const std::array<BladeExtent, 2> bladeExtents = MeasureCopyBlades(rig);
#endif
        PoseActor(rig, pose, worldOffset);
        if (pCount)
            ++pCount->Posed;
        if (cWatchKeep)
        {
            if (cPosedJustBefore)
                PoseKeepDiag::NotePosed(formId, cStillWhereLeft, glm::length(glm::vec2(worldOffset.x, worldOffset.y)));
            rig.PelvisLeft = ToGlm(keptPelvis.World().translate);
            rig.PelvisLeftAt = s_frameEndNumber;
        }
        switchCount.Posed = true;
#ifdef SKYRIMVR
        NotePosedHands(formId, ReachHands(rig, pose));
#else
        ReachHands(rig, pose);
#endif
        PlaceWeapons(rig, pose);
        // The weapons' physics bodies, blade contact and the drawn pose for the hit rule exist only in VR builds.
#ifdef SKYRIMVR
        LogPhysicsOnce(formId, rig);
        UpdateWeaponBodies(formId, pActor, rig);
        RecordCopyBlades(formId, pActor, bladeExtents);
        if (!isBody && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
            RecordDrawnPose(formId, pActor, rig);
#endif
        // Where the drawn weapon is, with its axes, for the rig's physics tests (live-check "DO drop ... onto copy weapon").
        if (rig.Attach[1].pNode && !rig.AttachBelow[1].empty())
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextWeaponLog;
            if (s_nextWeaponLog.size() > 256)
                s_nextWeaponLog.clear();
            auto& nextWeaponLog = s_nextWeaponLog[formId];
            if (now >= nextWeaponLog)
            {
                nextWeaponLog = now + std::chrono::seconds(5);
                const NiTransform& weapon = rig.Attach[1].World();
                const glm::mat3 axes = ToGlm(weapon.rotate);
                spdlog::debug(
                    "PhysicsProbe: actor {:X} right weapon drawn at ({:.1f}); its x axis ({:.2f}), y ({:.2f}), z ({:.2f})", formId, weapon.translate, axes[0], axes[1], axes[2]);
            }
        }

        // Hands as shown, measured after posing against the owner's own numbers, both relative to the 3D root in its own space.
        // A difference in z is a hand at the wrong height; in x or y, one too far forward or out to the side.
        if (pose.HasHandCheck && (rig.Bones[VRPose::kLeftHand].pNode || rig.Bones[VRPose::kLeftHand].pEntry))
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextHandLog;
            if (s_nextHandLog.size() > 256)
                s_nextHandLog.clear();
            auto& nextHandLog = s_nextHandLog[formId];
            if (now >= nextHandLog)
            {
                nextHandLog = now + std::chrono::seconds(5);
                const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
                const glm::mat3 inverseRoot = glm::transpose(ToGlm(rootWorld.rotate));
                const glm::vec3 rootAt = ToGlm(rootWorld.translate);
                const glm::vec3 copyLeft = inverseRoot * (ToGlm(rig.Bones[VRPose::kLeftHand].World().translate) - rootAt);
                const glm::vec3 copyRight = inverseRoot * (ToGlm(rig.Bones[VRPose::kRightHand].World().translate) - rootAt);
                // How high the body is drawn against where the reference stands: a lying or sinking copy shows here.
                spdlog::info("VRBodySync: actor {:X} body -- reference z {:.0f}, 3D root z {:.0f}, head {:.0f} above the root, knock state {}", formId, pActor->position.z, rootAt.z,
                             rig.Bones[VRPose::kHead].pNode || rig.Bones[VRPose::kHead].pEntry ? ToGlm(rig.Bones[VRPose::kHead].World().translate).z - rootAt.z : 0.f,
                             (pActor->actorState.flags1 >> 25) & 0x7);
                spdlog::debug(
                    "VRBodySync: actor {:X} hands -- owner L({:.1f}) R({:.1f}); copy L({:.1f}) R({:.1f})", formId, pose.HandOffsets[0], pose.HandOffsets[1], copyLeft, copyRight);
            }
        }
    }

#ifdef SKYRIMVR
    StopLocalBlades(now);
    ReleaseUnseenWeaponBodies();
#endif
}

/**
 * Work out how far a position is from where the player is looking, to tell whether they are looking at it.
 *
 * @param acPosition A position in the game's world.
 * @return The angle in degrees, 0 dead centre; -1 without the headset's node; 180, straight behind, when its numbers are broken.
 */
float HeadsetAngleTo(const NiPoint3& acPosition) noexcept
{
    if (!VRControllers::HeadsetTransform())
        return -1.f;
    // Broken numbers count as looking away, so nothing is taken as looked at.
    const auto cHead = VRControllers::HeadSpaceNow();
    if (!cHead)
        return 180.f;

    // Closer than a game unit, the head is on the point: it counts as looked at.
    if (glm::distance(ToGlm(acPosition), cHead->Position) < 1.f)
        return 0.f;
    return glm::degrees(cHead->AngleFromGaze(ToGlm(acPosition)));
}

/**
 * Find where the player's head is and which way it faces, to place other players' voices around it.
 *
 * @param aPosition Set to where the head is in the game's world.
 * @param aForward Set to which way it faces.
 * @param aRight Set to which way is its right.
 * @return False when the headset cannot be read; the three values are then left as they were.
 */
bool HeadsetPose(glm::vec3& aPosition, glm::vec3& aForward, glm::vec3& aRight) noexcept
{
    const auto cHead = VRControllers::HeadSpaceNow();
    if (!cHead)
        return false;

    aPosition = cHead->Position;
    aForward = cHead->Ahead();
    aRight = cHead->Right();
    return true;
}

bool IsRigParked(const PlayerCharacter* apPlayer) noexcept
{
    // Measured: 5 units with the map shut, 107116 the frame after it opened. A play space is a room, so 200 is generous.
    constexpr float cMaxPlaySpace = 200.f;

    const NiTransform* pHmdTransform = VRControllers::HeadsetTransform();
    if (!apPlayer || !pHmdTransform)
        return false;

    const glm::vec2 cOffset(pHmdTransform->translate.x - apPlayer->position.x, pHmdTransform->translate.y - apPlayer->position.y);
    return glm::length(cOffset) > cMaxPlaySpace;
}

std::string DescribeBody(Actor* apActor) noexcept
{
    // Every parent and bone is checked with IsReadable, so the answers are cached for this one description.
    // Uncached, one description costs 10 to 80 ms.
    ReadableMemory::CacheScope regions;
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
        return "no 3D";
    void* pSkeletonRoot = FindSkeletonRoot(pRoot);
    const uint32_t fingerprint = ChildFingerprintOf(AsNode(pRoot));
    const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
    const NiTransform& skeletonWorld = At<NiTransform>(pSkeletonRoot, kWorldOffset);
    // Two things can hide an otherwise healthy body, and this tells them apart.
    // One: its 3D hangs in a subtree that is no longer in the scene, so walking up the parents never reaches the world.
    // Two: a posed bone has collapsed (world scale near zero, or a rotation that is no longer a rotation).
    // A collapsed bone hides the skinned body while everything attached to the actor still draws.
    // The scale of two posed bones and the length of a spine rotation row (1.000 for a true rotation) show that.
    void* pWalk = pRoot;
    uint32_t depth = 0;
    const char* pTopName = "?";
    while (depth < 64)
    {
        void* pParent = At<void*>(pWalk, kParentOffset);
        if (!pParent || !ReadableMemory::IsReadable(pParent, kNiAVObjectSize))
            break;
        pWalk = pParent;
        ++depth;
        if (const char* pName = GetName(pWalk); pName && ReadableMemory::IsReadable(pName, 2))
            pTopName = pName;
    }
    // The player is always drawn, so whatever tree the player hangs from is the one the world draws.
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    void* pPlayerRoot = pPlayer ? pPlayer->GetNiNode() : nullptr;
    const char* pSharesScene = pPlayerRoot ? (SceneTopOf(pPlayerRoot) == pWalk ? "yes" : "NO") : "unknown";

    BoneNodes nodes;
    float spineScale = -1.f, headScale = -1.f, spineRowLength = -1.f;
    if (FindBones(pRoot, nodes))
    {
        if (nodes[VRPose::kSpine2])
        {
            const NiTransform& spine = At<NiTransform>(nodes[VRPose::kSpine2], kWorldOffset);
            spineScale = spine.scale;
            spineRowLength = glm::length(ToGlm(spine.rotate)[0]);
        }
        if (nodes[VRPose::kHead])
            headScale = At<NiTransform>(nodes[VRPose::kHead], kWorldOffset).scale;
    }

    // Why the other player's hand does not land where the sender had it.
    // VRPose sends bone rotations only and PoseActor keeps each bone's own translation, so this skeleton decides where the hand lands.
    // Printing the copy's numbers next to the player's tells the three causes apart:
    //   1. A skeleton root at a different height (VRIK height calibration) shows as a different root height.
    //   2. Bones of different lengths show as different arm lengths.
    //   3. VRIK moving the hand by translation shows as the same root and arms but a different hand height.
    // Heights are measured from the 3D root, so the actor's world position does not enter into it.
    const auto describeReach = [](void* apRoot, void* apSkeletonRoot) -> std::string
    {
        if (!apRoot)
            return "none";

        BoneNodes reachNodes;
        if (!FindBones(apRoot, reachNodes))
            return "bones not found";

        const NiTransform& rootTransform = At<NiTransform>(apRoot, kWorldOffset);
        const glm::vec3 rootPos = ToGlm(rootTransform.translate);

        const auto heightOf = [&](void* apNode) -> float { return apNode ? ToGlm(At<NiTransform>(apNode, kWorldOffset).translate).z - rootPos.z : -999.f; };
        const auto lengthBetween = [&](void* apA, void* apB) -> float
        {
            if (!apA || !apB)
                return -1.f;
            return glm::length(ToGlm(At<NiTransform>(apA, kWorldOffset).translate) - ToGlm(At<NiTransform>(apB, kWorldOffset).translate));
        };

        const float skeletonLocalZ = apSkeletonRoot ? At<NiTransform>(apSkeletonRoot, kLocalOffset).translate.z : -999.f;

        return fmt::format("skelRootLocalZ {:.1f}, head {:.1f}, Lhand {:.1f}, Rhand {:.1f}, upperarm {:.1f}, forearm {:.1f}", skeletonLocalZ, heightOf(reachNodes[VRPose::kHead]),
                           heightOf(reachNodes[VRPose::kLeftHand]), heightOf(reachNodes[VRPose::kRightHand]),
                           lengthBetween(reachNodes[VRPose::kLeftUpperArm], reachNodes[VRPose::kLeftForearm]),
                           lengthBetween(reachNodes[VRPose::kLeftForearm], reachNodes[VRPose::kLeftHand]));
    };

    // Render state: a healthy actor is still not drawn when the renderer fades its node out.
    // CommonLibVR puts BSFadeNode's data at +0x128, but VR's NiNode keeps its children at +0x138, so +0x128 is not the fade here.
    // The words just past a VR NiNode are printed for the copy and for the always-drawn player; the word that differs is the fade.
    std::string renderWords;
    std::string playerWords;
    const auto dumpFloats = [](void* apNode, std::string& aOut)
    {
        if (!apNode || !ReadableMemory::IsReadable(apNode, 0x170))
        {
            aOut = "unreadable";
            return;
        }
        for (uint32_t offset = 0x140; offset < 0x160; offset += 4)
        {
            const float value = At<float>(apNode, offset);
            aOut += fmt::format("{}{:X}={:.3f}", aOut.empty() ? "" : " ", offset, std::isfinite(value) ? value : -999.f);
        }
    };
    dumpFloats(pRoot, renderWords);
    dumpFloats(pPlayerRoot, playerWords);

    return fmt::format("3D root {} with {} of {} child slots filled, root world scale {:.3f} at ({:.0f}, {:.0f}, {:.0f}), skeleton root {} world scale {:.3f}; "
                       "{} parents up to '{}' (same scene as the player: {}); spine scale {:.3f} rotation row {:.4f}, head scale {:.3f}; "
                       "copy words [{}]; player words [{}]; copy reach [{}]; player reach [{}]",
                       pRoot, fingerprint & 0xFFFF, fingerprint >> 16, rootWorld.scale, rootWorld.translate.x, rootWorld.translate.y, rootWorld.translate.z,
                       pSkeletonRoot == pRoot ? "missing" : "found", skeletonWorld.scale, depth, pTopName, pSharesScene, spineScale, spineRowLength, headScale, renderWords, playerWords,
                       describeReach(pRoot, pSkeletonRoot == pRoot ? nullptr : pSkeletonRoot), describeReach(pPlayerRoot, pPlayerRoot ? FindSkeletonRoot(pPlayerRoot) : nullptr));
}

// The local player's first-person hand, left (0) or right (1), or null when this build's PlayerCharacter does not hold it there.
// The name is checked the first time; after that, the same pooled name pointer shows it is still the same node.
void* FirstPersonHand(PlayerCharacter* apPlayer, const size_t aSide) noexcept
{
    static std::array<int, 2> s_state{}; // 0 unchecked, 1 the hand, -1 not the hand on this build
    static std::array<const char*, 2> s_name{};
    if (!apPlayer || s_state[aSide] < 0)
        return nullptr;

    void* pHand = At<void*>(apPlayer, kFirstPersonHandOffsets[aSide]);
    if (!pHand)
        return nullptr;

    if (s_state[aSide] == 0)
    {
        const char* pName = ReadableMemory::IsReadable(pHand, kNiAVObjectSize) ? GetName(pHand) : nullptr;
        const bool cReadable = pName && ReadableMemory::IsReadable(pName, 8);
        s_state[aSide] = cReadable && std::strcmp(pName, kHandNames[aSide]) == 0 ? 1 : -1;
        s_name[aSide] = pName;
        if (s_state[aSide] < 0)
        {
            spdlog::warn("VRBodySync: PlayerCharacter+0x{:X} is not the first-person {} hand (it is {}); where this player holds weapons is not sent", kFirstPersonHandOffsets[aSide],
                         aSide ? "right" : "left", cReadable ? pName : "unreadable");
            return nullptr;
        }
    }

    return GetName(pHand) == s_name[aSide] ? pHand : nullptr;
}

bool LocalHandPosition(const uint8_t aSide, glm::vec3& aOut) noexcept
{
    void* pHand = aSide <= 1 ? FirstPersonHand(PlayerCharacter::Get(), aSide) : nullptr;
    if (!pHand)
        return false;
    aOut = ToGlm(At<NiTransform>(pHand, kWorldOffset).translate);
    return true;
}

// Whether this hand holds something that parries: a weapon, or for the left hand a worn shield.
// The game keeps a shield as worn armour, not as the hand's object.
bool HandParries(const uint8_t aSide) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || aSide > 1)
        return false;
    if (const TESForm* pHeld = pPlayer->GetEquippedWeapon(aSide); pHeld && pHeld->formType == FormType::Weapon)
        return true;
    if (aSide != 0)
        return false;
    // Read at most once a second: it walks the whole inventory, and the blade stop asks ten times a second.
    static bool s_shield = false;
    static std::chrono::steady_clock::time_point s_readAt{};
    if (const auto cNow = std::chrono::steady_clock::now(); cNow - s_readAt >= std::chrono::seconds(1))
    {
        s_readAt = cNow;
        const Inventory shields = pPlayer->GetInventory(
            [](TESForm& aForm)
            {
                return aForm.formType == FormType::Armor &&
                       (static_cast<TESObjectARMO&>(aForm).slotType & static_cast<uint32_t>(BGSBipedObjectForm::Part::Shield)) != 0;
            });
        s_shield = std::any_of(shields.Entries.begin(), shields.Entries.end(), [](const Inventory::Entry& acEntry) { return acEntry.IsWorn(); });
    }
    return s_shield;
}

// ---------------------------------------------------------------------------------------------------------------
// The blade stop: the player's blade cannot pass through the other player's.
//
// Both swords are keyframed to hands, so Havok sees them touch (the clash) but neither can push the other.
// At the frame end, once every copy is posed, each drawn blade is a segment (Blade).
// When the player's blade crosses one of his, the drawn weapon is turned about the grip to rest on his blade on the side it came from.
// It lets go when the hand comes back, when the touch slides off either blade's end or reaches the hand, or past a 60 degree turn.
// Only world transforms are written, which the game redraws from the hand every frame; the controller and hand are untouched.
// The turned grip is also sent (DrawnGripRotation), so the other player sees the blade rest on his too.
// A hit from a stopped blade is dropped (IsBladeStoppedAt), since the blade he sees never reached him.
constexpr float kBladeMaxTurnDegrees = 60.f;
constexpr float kBladeGuard = 8.f; // units from the grip: nearer than that, his blade is at the player's hand
constexpr float kBladeRest = 1.5f; // units ours is held off his blade's line, about half of two blades' thickness
constexpr float kBladeSlack = 2.f; // units past the end of either blade a touch still counts

// Whether our blade, turned into the plane of his (SideOf's normal and side), touches his within the length of both.
// Returns null when it does, otherwise the reason; aT and aV are the distances along ours and his.
// Meeting and holding share this rule, so a blade is never caught under one rule and let go under the other.
const char* WhyNotTouching(const Blade& acOurs, const Blade& acHis, const float aSideNow, const glm::vec3& acNormal, float& aT, float& aV) noexcept
{
    if (!ClosestPointsOnLines(acOurs.Grip, glm::normalize(acOurs.Axis - aSideNow * acNormal), acHis.Grip, acHis.Axis, aT, aV))
        return "the blades lie along each other";
    if (aT < kBladeGuard)
        return "his blade is at the hand";
    if (aT > acOurs.Length + kBladeSlack)
        return "his blade is beyond the end of ours";
    if (aV < -kBladeSlack || aV > acHis.Length + kBladeSlack)
        return "ours is beyond the end of his";
    return nullptr;
}

std::array<BladeExtent, 2> MeasureCopyBlades(const Rig& acRig) noexcept
{
    std::array<BladeExtent, 2> extents{};
    for (size_t side = 0; side < 2; ++side)
        if (void* pAttach = acRig.Attach[side].pNode)
            for (const RigBone& below : acRig.AttachBelow[side])
                if (below.pNode && At<void*>(below.pNode, kParentOffset) == pAttach && MeasureBlade(below.pNode, extents[side]))
                    break;
    return extents;
}

void RecordCopyBlades(const uint32_t aFormId, Actor* apActor, const std::array<BladeExtent, 2>& acExtents) noexcept
{
    // Drawn, asked to sheathe or sheathing, as for its weapon bodies (UpdateWeaponBodies).
    if (!apActor->actorState.IsWeaponInHand())
        return;
    for (uint32_t side = 0; side < 2; ++side)
    {
        if (!acExtents[side].pNode)
            continue;
        const CopyBlade blade{(static_cast<uint64_t>(aFormId) << 1) | side, BladeAt(acExtents[side])};
        if (!IsFiniteVector(blade.Segment.Grip) || !IsFiniteVector(blade.Segment.Axis))
            continue;
        s_copyBlades.push_back(blade);
    }
}

struct LocalStop
{
    // The first-person attach node (WEAPON, SHIELD), searched twice a second as CaptureWeapons does.
    void* pAttach = nullptr;
    void* pAttachVTable = nullptr;
    std::chrono::steady_clock::time_point SearchAt{};
    BladeExtent Extent{}; // measured on frames it was not turned
    // Which side of each copy blade ours was on at the last frame end, while free, to see it cross one.
    // Two lists reused frame after frame, so the frame end allocates nothing once warm.
    std::vector<std::pair<uint64_t, float>> LastSide;
    std::vector<std::pair<uint64_t, float>> Sides;
    // The blade it rests on (0: free), the side of it ours is held on (SideOf's sign), and what to say on letting go.
    uint64_t Key = 0;
    float HeldSide = 0.f;
    std::chrono::steady_clock::time_point Since{};
    float WorstDegrees = 0.f;
    uint32_t Frames = 0;
    uint32_t NotDrawnAnew = 0;
    // This frame end's turn, and the weapon node's rotation as written, to tell whether the game drew it anew since.
    bool Turned = false;
    NiMatrix3 WrittenRoot{};
};
std::array<LocalStop, 2> s_localStops{};

// What other threads read: the grip as sent (CaptureWeapons, in the game's update) and the hit check (in the game's damage code).
struct SharedStop
{
    bool Turned = false;
    glm::mat3 Turn{1.f};
    NiMatrix3 WrittenAttach{};
    Blade Real{}; // the blade where the hand holds it, not where it is drawn
    uint32_t On = 0;
    std::chrono::steady_clock::time_point DropHitsUntil{};
};
std::mutex s_sharedStopsLock;
std::array<SharedStop, 2> s_sharedStops{};

/**
 * Find the weapon drawn in one of the local player's hands, as the player sees it: the node under the first-person attach node.
 * It is read from the attach node's children every frame, so a weapon swapped between two searches is never a stale pointer.
 * The attach node itself is searched for again every half second, or at once when its class table changed.
 *
 * @param apPlayer The local player.
 * @param aStop That hand's blade stop, which keeps the attach node and when to search again.
 * @param aSide 0 for the left hand, 1 for the right.
 * @param aNow The time now.
 * @return The weapon's node, or null when the hand holds none.
 */
void* LocalWeaponNode(PlayerCharacter* apPlayer, LocalStop& aStop, const size_t aSide, const std::chrono::steady_clock::time_point aNow) noexcept
{
    const bool cLost = aStop.pAttach && !HasSameVTable(aStop.pAttach, aStop.pAttachVTable);
    if (aNow >= aStop.SearchAt || cLost)
    {
        aStop.SearchAt = aNow + std::chrono::milliseconds(500);
        aStop.pAttach = nullptr;
        aStop.pAttachVTable = nullptr;
        if (void* pHand = FirstPersonHand(apPlayer, aSide))
        {
            void* pForearm = At<void*>(pHand, kParentOffset);
            void* pAttach = FindShallowest(pForearm ? pForearm : pHand, kAttachNames[aSide]);
            if (pAttach && AsNode(pAttach))
            {
                aStop.pAttach = pAttach;
                aStop.pAttachVTable = *static_cast<void**>(pAttach);
            }
        }
    }
    if (!aStop.pAttach)
        return nullptr;
    // Not ForEachChild: only the first eight slots are looked at.
    const ChildSlots cChildren = ChildSlotsOf(aStop.pAttach);
    for (uint16_t i = 0; cChildren.pSlots && i < cChildren.Count && i < 8; ++i)
        if (cChildren.pSlots[i])
            return cChildren.pSlots[i];
    return nullptr;
}

const CopyBlade* FindCopyBlade(const uint64_t aKey) noexcept
{
    for (const CopyBlade& blade : s_copyBlades)
        if (blade.Key == aKey)
            return &blade;
    return nullptr;
}

void ReleaseBladeStop(const size_t aSide, LocalStop& aStop, const char* acpWhy, const std::chrono::steady_clock::time_point aNow) noexcept
{
    if (aStop.Key)
        spdlog::info("BladeStop: our {} blade let go of {:X}'s {} blade after {} ms: {}; it was held up to {:.0f} degrees from where the hand had it, over {} frame ends ({} "
                     "of them not drawn anew by the game)",
                     aSide ? "right" : "left", static_cast<uint32_t>(aStop.Key >> 1), aStop.Key & 1 ? "right" : "left",
                     std::chrono::duration_cast<std::chrono::milliseconds>(aNow - aStop.Since).count(), acpWhy, aStop.WorstDegrees, aStop.Frames, aStop.NotDrawnAnew);
    aStop.Key = 0;
    aStop.Turned = false;
    aStop.LastSide.clear();
}

// Turns the attach node and everything below it about the grip, only while a blade rests on another.
// The nodes are walked from the attach node each time; no node list is kept between frames.
void TurnDrawnWeapon(void* apAttach, const glm::vec3& acPivot, const glm::mat3& acTurn) noexcept
{
    static std::vector<void*> s_nodes;
    s_nodes.clear();
    s_nodes.push_back(apAttach);
    CollectNodeDescendants(apAttach, s_nodes);
    for (void* pNode : s_nodes)
    {
        NiTransform& world = At<NiTransform>(pNode, kWorldOffset);
        world.rotate = FromGlm(acTurn * ToGlm(world.rotate));
        world.translate = FromGlm(acPivot + acTurn * (ToGlm(world.translate) - acPivot));
    }
}

void StopLocalBlades(const std::chrono::steady_clock::time_point aNow) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const bool cDrawn = pPlayer && pPlayer->actorState.IsWeaponInHand() && !pPlayer->actorState.IsDeadOrDying();

    std::array<SharedStop, 2> shared{};
    for (size_t side = 0; side < 2; ++side)
    {
        LocalStop& stop = s_localStops[side];
        void* pNode = cDrawn ? LocalWeaponNode(pPlayer, stop, side, aNow) : nullptr;
        const TESForm* pHeld = pNode ? pPlayer->GetEquippedWeapon(static_cast<uint32_t>(side)) : nullptr;
        if (!pNode || !pHeld || pHeld->formType != FormType::Weapon)
        {
            ReleaseBladeStop(side, stop, "nothing is drawn in that hand", aNow);
            continue;
        }

        // The game has not redrawn it since our last turn (it still has the rotation we wrote), so that turn still holds.
        // Nothing is weighed until the game draws it from the hand again.
        const NiTransform& world = At<NiTransform>(pNode, kWorldOffset);
        if (stop.Turned && std::memcmp(&world.rotate, &stop.WrittenRoot, sizeof(NiMatrix3)) == 0)
        {
            ++stop.NotDrawnAnew;
            std::lock_guard lock(s_sharedStopsLock);
            shared[side] = s_sharedStops[side];
            continue;
        }
        stop.Turned = false;

        // Measured where nothing of ours has turned it: the game's transform and its bound agree.
        if (!stop.Key || stop.Extent.pNode != pNode)
        {
            if (!MeasureBlade(pNode, stop.Extent))
            {
                ReleaseBladeStop(side, stop, "the weapon has no blade to measure", aNow);
                stop.Extent = BladeExtent{};
                continue;
            }
        }
        const Blade ours = BladeAt(stop.Extent);
        if (!IsFiniteVector(ours.Grip) || !IsFiniteVector(ours.Axis))
        {
            ReleaseBladeStop(side, stop, "the weapon's transform is not usable", aNow);
            continue;
        }

        // Free: did it pass through one of his blades since the last frame end, within the length of both?
        if (!stop.Key)
        {
            stop.Sides.clear();
            for (const CopyBlade& his : s_copyBlades)
            {
                if (glm::distance(his.Segment.Grip, ours.Grip) > ours.Length + his.Segment.Length + 20.f)
                    continue;
                glm::vec3 normal{};
                const float sideNow = SideOf(ours, his.Segment, normal);
                const float sign = sideNow > 0.f ? 1.f : sideNow < 0.f ? -1.f : 0.f;
                stop.Sides.emplace_back(his.Key, sign);
                const auto before = std::find_if(stop.LastSide.begin(), stop.LastSide.end(), [&his](const auto& acEntry) { return acEntry.first == his.Key; });
                if (stop.Key || sign == 0.f || before == stop.LastSide.end() || before->second == 0.f || before->second == sign)
                    continue;
                float t = 0.f, v = 0.f;
                if (const char* pNotHeld = WhyNotTouching(ours, his.Segment, sideNow, normal, t, v))
                {
                    // Crossings that are not held are logged at most four times a second, with the geometry of both blades.
                    static std::chrono::steady_clock::time_point s_nextNotHeld{};
                    if (aNow >= s_nextNotHeld)
                    {
                        s_nextNotHeld = aNow + std::chrono::milliseconds(250);
                        spdlog::info("BladeStop: our {} blade crossed the line of {:X}'s {} blade, not held: {} ({:.0f} along ours of {:.0f}, {:.0f} along his of {:.0f}; ours "
                                     "from ({:.0f}) along ({:.2f}), his from ({:.0f}) along ({:.2f}))",
                                     side ? "right" : "left", static_cast<uint32_t>(his.Key >> 1), his.Key & 1 ? "right" : "left", pNotHeld, t, ours.Length, v,
                                     his.Segment.Length, ours.Grip, ours.Axis, his.Segment.Grip, his.Segment.Axis);
                    }
                    continue;
                }
                stop.Key = his.Key;
                stop.HeldSide = before->second;
                stop.Since = aNow;
                stop.WorstDegrees = 0.f;
                stop.Frames = 0;
                stop.NotDrawnAnew = 0;
                const glm::vec3 at = his.Segment.Grip + his.Segment.Axis * v;
                spdlog::debug(
                    "BladeStop: our {} blade met {:X}'s {} blade at ({:.0f}), {:.0f} units from our grip and {:.0f} from his; held on this side of it", side ? "right" : "left",
                    static_cast<uint32_t>(his.Key >> 1), his.Key & 1 ? "right" : "left", at, t, v);
            }
            stop.LastSide.swap(stop.Sides);
        }

        // Held: turned about the grip into the plane of his blade, resting on it on the side it came from.
        if (stop.Key)
        {
            const char* pWhy = nullptr;
            glm::mat3 turn{1.f};
            float degrees = 0.f;
            const CopyBlade* pHis = FindCopyBlade(stop.Key);
            glm::vec3 normal{};
            const float sideNow = pHis ? SideOf(ours, pHis->Segment, normal) : 0.f;
            float t = 0.f, v = 0.f;
            if (!pHis)
                pWhy = "his blade is no longer drawn here";
            else if (sideNow == 0.f)
                pWhy = "the hand is on his blade's line";
            else if (sideNow * stop.HeldSide > 0.f)
                pWhy = "the hand came back";
            else if ((pWhy = WhyNotTouching(ours, pHis->Segment, sideNow, normal, t, v)) == nullptr)
            {
                const glm::vec3 held = glm::normalize(glm::normalize(ours.Axis - sideNow * normal) + stop.HeldSide * (kBladeRest / t) * normal);
                turn = RotationBetween(ours.Axis, held);
                degrees = DegreesOf(turn);
                if (!(degrees <= kBladeMaxTurnDegrees))
                    pWhy = "the hand pushed it past (more than 60 degrees)";
            }
            if (pWhy)
            {
                ReleaseBladeStop(side, stop, pWhy, aNow);
                continue;
            }

            TurnDrawnWeapon(stop.pAttach, ours.Grip, turn);
            stop.Turned = true;
            stop.WrittenRoot = world.rotate;
            stop.WorstDegrees = std::max(stop.WorstDegrees, degrees);
            ++stop.Frames;
            shared[side].Turned = true;
            shared[side].Turn = turn;
            shared[side].WrittenAttach = At<NiTransform>(stop.pAttach, kWorldOffset).rotate;
            shared[side].Real = ours;
            shared[side].On = static_cast<uint32_t>(stop.Key >> 1);
            shared[side].DropHitsUntil = aNow + std::chrono::milliseconds(300);
        }
    }

    std::lock_guard lock(s_sharedStopsLock);
    for (size_t side = 0; side < 2; ++side)
    {
        // A hit along a blade that has just been let go still counts as stopped for its last 300 ms.
        const auto cKeepHits = s_sharedStops[side].DropHitsUntil;
        const uint32_t cKeepOn = s_sharedStops[side].On;
        const Blade cKeepReal = s_sharedStops[side].Real;
        s_sharedStops[side] = shared[side];
        if (!shared[side].Turned && aNow < cKeepHits)
        {
            s_sharedStops[side].DropHitsUntil = cKeepHits;
            s_sharedStops[side].On = cKeepOn;
            s_sharedStops[side].Real = cKeepReal;
        }
    }
}

// The grip as drawn, from the first-person attach node's rotation read in the game's update.
// The blade stop's last turn is applied, unless the node still holds that turn (the game has not redrawn it).
glm::mat3 DrawnGripRotation(const size_t aSide, const NiMatrix3& acRead) noexcept
{
    std::lock_guard lock(s_sharedStopsLock);
    const SharedStop& stop = s_sharedStops[aSide];
    if (!stop.Turned || std::memcmp(&acRead, &stop.WrittenAttach, sizeof(NiMatrix3)) == 0)
        return ToGlm(acRead);
    return stop.Turn * ToGlm(acRead);
}

bool IsBladeStoppedAt(const uint32_t aCopyFormId, const glm::vec3& acPoint) noexcept
{
    const auto cNow = std::chrono::steady_clock::now();
    // No point to go by (none written, or not a number): any blade resting on his counts.
    const bool cNoPoint = !IsFiniteVector(acPoint) || glm::dot(acPoint, acPoint) < 1.f;
    std::lock_guard lock(s_sharedStopsLock);
    for (const SharedStop& stop : s_sharedStops)
        if (stop.On == aCopyFormId && cNow < stop.DropHitsUntil && (cNoPoint || DistanceToBlade(acPoint, stop.Real) <= 20.f))
            return true;
    return false;
}

bool TakeClash(Clash& aOut) noexcept
{
    PendingClash pending{};
    {
        std::lock_guard lock(s_clashesLock);
        if (s_clashes.empty())
            return false;
        pending = s_clashes.front();
        s_clashes.erase(s_clashes.begin());
    }
    aOut.FormId = pending.FormId;
    aOut.Side = pending.Side;
    aOut.Point = pending.Point;
    aOut.Speed = pending.Speed;
    aOut.Start = pending.Start;
    aOut.Heard = false;

    // Which local hand it was: the one nearer the HIGGS body (the hand, or what the hand holds).
    // Every HIGGS body belongs to one of the two hands, so there is no distance limit: a fast hand can leave its body 40+ units away.
    aOut.OwnSide = 2;
    float nearest = std::numeric_limits<float>::max();
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        for (uint8_t side = 0; side < 2; ++side)
            if (void* pHand = FirstPersonHand(pPlayer, side))
            {
                const float distance = glm::length(ToGlm(At<NiTransform>(pHand, kWorldOffset).translate) - pending.Hand);
                if (distance < nearest)
                {
                    nearest = distance;
                    aOut.OwnSide = side;
                    aOut.HandDistance = distance;
                }
            }
    // Only a weapon or a shield parries: a hand is a body part, so his blade on an empty hand is a hit.
    // Something held up with HIGGS's grab counts as held.
    aOut.Parries = pending.Held || HandParries(aOut.OwnSide);
    if (!pending.Start || !aOut.Parries)
        return true;
    PulseClashHaptics(aOut.OwnSide);
    aOut.Heard = PlayClashSound(aOut.Point);
    PlayClashSparks(aOut.Point);
    return true;
}

void PulseClashHaptics(const uint8_t aSide) noexcept
{
    if (aSide < 2)
        VRHaptics::Pulse(aSide == 1, 3999);
}

/**
 * Forget what PlaceCorpse, the fall stop and the owner's posing knew of a copy, so a new copy under the same id starts afresh.
 *
 * @param aFormId The copy.
 */
void ForgetCorpse(const uint32_t aFormId) noexcept
{
    {
        std::scoped_lock lock(s_placedCorpsesLock);
        s_placedCorpses.erase(aFormId);
    }
    FallStop::Forget(aFormId);
    RagdollPoseService::ForgetRestBodies(aFormId);
#ifndef SKYRIMVR
    {
        // A move queued for the copy that went, or now runs here, is not made.
        std::scoped_lock lock(s_pendingRagdollMovesLock);
        std::erase_if(s_pendingRagdollMoves, [aFormId](const PendingRagdollMove& acMove) { return acMove.FormId == aFormId; });
    }
#endif
    {
        std::scoped_lock lock(s_followLock);
        std::erase_if(s_followRequests, [aFormId](const FollowRequest& acRequest) { return acRequest.FormId == aFormId; });
        s_followed.erase(aFormId);
    }
    {
        std::scoped_lock lock(s_heldWeaponsLock);
        s_heldWeapons.erase(aFormId);
    }
    std::scoped_lock lock(s_bodyPosedLock);
    s_bodyPosedByOwnerAt.erase(aFormId);
}

void ForgetAllCorpses() noexcept
{
    {
        std::scoped_lock lock(s_placedCorpsesLock);
        s_placedCorpses.clear();
    }
    RagdollPoseService::ForgetAllRestBodies();
#ifndef SKYRIMVR
    {
        std::scoped_lock lock(s_pendingRagdollMovesLock);
        s_pendingRagdollMoves.clear();
    }
#endif
    {
        std::scoped_lock lock(s_followLock);
        s_followRequests.clear();
        s_followed.clear();
    }
    {
        std::scoped_lock lock(s_diedHereLock);
        s_diedHere.clear();
    }
    {
        std::scoped_lock lock(s_heldWeaponsLock);
        s_heldWeapons.clear();
    }
    std::scoped_lock lock(s_bodyPosedLock);
    s_bodyPosedByOwnerAt.clear();
}

bool PlaceCorpse(Actor* apActor, const glm::vec3& acWanted, const CorpsePlacement& acHow) noexcept
{
    // A corpse is drawn from its ragdoll, and moving the reference (ForcePosition, MoveTo, SetPosition) leaves the ragdoll behind.
    // So each of the ragdoll's rigid bodies is moved by the same offset, as PLANCK warps a ragdoll.
    // PLANCK's main.cpp does it with `rb->getRigidMotion()->setTransform` and `updateMovedBodyInfo`, velocities zeroed.
    // Once the owner's bones stop coming, a dead body is instead laid out once in the owner's last pose (PoseTargets).
    // Covered by `live-corpse-place`, `live-corpse-fallen`, `live-copy-death-pose` and `live-copy-knockdown-place`.
    if (!apActor)
        return false;
#ifndef SKYRIMVR
    // Not moved on this flat game: nothing to measure either.
    if (!CanMoveRagdolls(nullptr))
        return false;
#endif
    // Not while a load is up or just over, nor for a copy whose cell is not attached. This runs in the client's update, which can run
    // beside the main thread during a load, and on VR it walks the copy's live 3D and writes its physics bodies here and now, while the
    // load may be freeing that cell. It is asked again on the next pass. The flat game queues its moves and checks again when it makes them.
    static thread_local CopyWork::LoadGate s_loadGate;
    if (!CopyWorkSettled(s_loadGate) || !apActor->parentCell || !apActor->parentCell->IsAttached())
        return false;
    using namespace CopyBodyRules;
    const auto cNow = std::chrono::steady_clock::now();
    const bool cDead = apActor->actorState.IsDead();

    std::scoped_lock corpsesLock(s_placedCorpsesLock);
    auto& placed = s_placedCorpses[apActor->formID];
    // Shown for someone else now, or made again: what was known of it, and decided for it, no longer holds.
    if (placed.Authority != acHow.Authority)
    {
        placed = PlacedCorpse{};
        placed.Authority = acHow.Authority;
    }
    placed.AuthoritySeenAt = cNow;
    if (placed.Fall != acHow.Fall)
    {
        placed.Fall = acHow.Fall;
        placed.KnockPlace = {};
    }

    // Not while its owner is moving it: its bones arrive (ApplyRemotePoses) and place it, and moving the ragdoll too would fight them.
    // The ragdoll stays behind meanwhile, so the moment the bones stop it is put where the owner left the body, or it would flash back.
    // The copy's own playback says so on both builds (acHow.OwnerBonesArrive); VR's drawer also knows from the poses it was given.
    const auto cPosedAt = BodyPosedAt(apActor->formID);
    bool bonesArrive = acHow.OwnerBonesArrive;
    if (cPosedAt && !bonesArrive)
    {
        std::shared_lock lock(s_posesLock);
        const auto it = s_poses.find(apActor->formID);
        bonesArrive = it != s_poses.end() && it->second.Available(cNow);
    }
    if (bonesArrive)
    {
        placed.BonesSeenAt = cNow;
        // The last of these is the pose the owner's body came to rest in, kept for when they stop: a new stream, a new settle.
        if (cDead && acHow.pOwnerPose && acHow.pOwnerPose->HasData && !acHow.pOwnerPose->NoBones)
        {
            placed.OwnerPose = *acHow.pOwnerPose;
            placed.HasOwnerPose = true;
            placed.Settle = {};
        }
    }

    // A resting pose that came before this copy could lie in it, as one sent with its spawn to a player who sees the body later:
    // once its ragdoll is here, it is its owner's last pose, settled into once. A creature's bodies wait in RagdollPoseService.
    const bool cHeldBones = acHow.pRestPose && acHow.pRestPose->HasData;
    if (cHeldBones || RagdollPoseService::HoldsRestBodies(apActor->formID))
    {
        void* pHeldRoot = cDead ? apActor->GetNiNode() : nullptr;
        TiltedPhoques::Vector<RagdollBody> heldFor;
        const bool cRagdollHere = pHeldRoot && !placed.MovePending && !placed.Settle.Pending && CollectRagdoll(pHeldRoot, heldFor);
        // Sent again with a routine spawn while the copy already lies so, as when it settled while its owner's bones came: not laid out again.
        const bool cReadHere = cRagdollHere && !bonesArrive;
        const bool cLiesInIt =
            cHeldBones ? (placed.HasOwnerPose && !BonesMoved(MostDegreesApart(placed.OwnerPose, *acHow.pRestPose))) || (cReadHere && LiesInPose(pHeldRoot, *acHow.pRestPose))
                       : cReadHere && LiesInRestBodies(apActor->formID, heldFor);
        switch (OnHeldRestPose(bonesArrive, cRagdollHere, cLiesInIt))
        {
        case HeldRestPose::Keep: break;
        case HeldRestPose::Settle:
            if (cHeldBones)
            {
                placed.OwnerPose = *acHow.pRestPose;
                acHow.pRestPose->HasData = false;
            }
            else
            {
                // No bones: the settle reads the held bodies (RestBodyTargets).
                placed.OwnerPose = VRPose{};
                placed.OwnerPose.HasData = true;
                placed.OwnerPose.NoBones = true;
                RagdollPoseService::HandRestBodies(apActor->formID);
            }
            placed.HasOwnerPose = true;
            placed.Settle = {};
            spdlog::debug("CorpseDiag: {:X} is laid out in the pose its body rested in at its owner's, sent with its spawn", apActor->formID);
            break;
        case HeldRestPose::Forget:
            if (cLiesInIt && !bonesArrive)
                spdlog::debug("CorpseDiag: {:X} already lies in the pose its body rested in at its owner's, sent again with its spawn", apActor->formID);
            if (cHeldBones)
                acHow.pRestPose->HasData = false;
            else
                RagdollPoseService::ForgetRestBodies(apActor->formID);
            break;
        }
    }

    // A living NPC knocked down here lies where this game's own fall put it (live-copy-knockdown), unless its owner just carried it elsewhere,
    // or it has come to rest while its owner's lies elsewhere (acHow.KnockedWhereOwnerFell), once per fall.
    BodyHere body{};
    body.Dead = cDead;
    body.Dying = apActor->actorState.IsDying();
    body.BleedingOut = apActor->actorState.IsBleedingOut();
    body.KnockedWhereOwnerFell = acHow.KnockedWhereOwnerFell && apActor->actorState.IsInKnockRagdoll() && MayStart(placed.KnockPlace);
    body.CarriedLately = cPosedAt && cNow - *cPosedAt < std::chrono::seconds(10);
#ifdef SKYRIMVR
    // A person is drawn from its owner's bones (OnFrameEnd); a creature's ragdoll is set into its owner's bodies (FollowCarriedBodies).
    const bool cFollows = bonesArrive && acHow.pOwnerPose && acHow.pOwnerPose->NoBones && RagdollPoseService::HasOwnerBodies(apActor->formID, acHow.Authority);
    constexpr bool cBonesPoseNpcsHere = true;
#else
    // A body flat sets into its owner's bones (FollowCarriedBodies) is not moved meanwhile.
    const bool cFollows = bonesArrive && acHow.pOwnerPose;
    const bool cBonesPoseNpcsHere = FollowedNow(apActor->formID);
#endif
    if (cFollows)
    {
        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        const CarriedBodyFollow::Asked cStamp{
            cNow, apActor->GetHandle().handle.iBits, apActor->parentCell ? apActor->parentCell->formID : 0, pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0};
        AskToFollow(apActor->formID, acHow.Authority, *acHow.pOwnerPose, acWanted, acHow.OwnerBonesMove, cStamp);
    }
    body.OwnerBonesArrive = OwnerBonesHoldBody(bonesArrive, cBonesPoseNpcsHere, body.KnockedWhereOwnerFell);
    if (!MayMoveRagdoll(body))
        return false;
    // A move already waits to run.
    if (placed.MovePending || placed.Settle.Pending || placed.KnockPlace.Pending)
        return false;

    // A drag that just ended starts over: the moves before it say nothing about this one.
    if (cPosedAt && *cPosedAt != placed.PosedAt)
    {
        placed.PosedAt = *cPosedAt;
        placed.MovedAt = {};
        placed.Streak = 0;
    }
    if (cNow < placed.NextAt)
        return false;
    placed.NextAt = cNow + std::chrono::milliseconds(500);

    const float cScale = HavokWorldScale();
    void* pRoot = apActor->GetNiNode();
    if (!(cScale > 0.f) || !pRoot)
        return false;

    TiltedPhoques::Vector<RagdollBody> bodies;
    if (!CollectRagdoll(pRoot, bodies))
        return false;

    const glm::vec3 cRagdollRoot = RagdollBodyPosition(bodies[0].pHavok, cScale);
    // A ragdoll that fell through the ground stays over its owner's corpse as it falls, so depth counts too.
    // The owner sends its body's 3D root, a few tens of units from its ragdoll's root, so a settled body is never that deep.
    constexpr float cFallenDepth = 256.f;
    const float cDepth = acWanted.z - cRagdollRoot.z;
    const float cApart = cDepth > cFallenDepth ? glm::distance(acWanted, cRagdollRoot) : glm::length(glm::vec2{acWanted.x - cRagdollRoot.x, acWanted.y - cRagdollRoot.y});

    // TEMPORARY: a copy's ragdoll found under the ground, with how far under it was when first measured, to tell a fall at death from a later one.
    if (placed.FirstSeenAt == std::chrono::steady_clock::time_point{})
    {
        placed.FirstSeenAt = cNow;
        placed.FirstDepth = cDepth;
    }
    if (cDepth > cFallenDepth && !placed.SaidFallen)
    {
        placed.SaidFallen = true;
        spdlog::info("CorpseFall: {:X}'s ragdoll lies {:.0f} units below where its owner's corpse is, {:.1f} s after it was first measured here, when it lay {:.0f} units below "
                     "(negative is above); {:.0f} units off across the ground; its cell {:X}, attached {}",
                     apActor->formID, cDepth, std::chrono::duration<float>(cNow - placed.FirstSeenAt).count(), placed.FirstDepth,
                     glm::length(glm::vec2{acWanted.x - cRagdollRoot.x, acWanted.y - cRagdollRoot.y}), apActor->parentCell ? apActor->parentCell->formID : 0,
                     apActor->parentCell && apActor->parentCell->IsAttached());
    }

    // A guard, not a fix: a copy made here whose ragdoll lies deep under the ground and still drops has nothing under it, and is switched off (FallStop).
    // Removed then, it is asked for again when this game announces its cell, and the copy made then starts afresh.
    if (CopyRegistry::IsOwnCopy(apActor->formID) && FallStop::NoteLookAndCheckFall(apActor->formID, cDepth, cRagdollRoot.z))
    {
        FrameStartSwitchOff::Queue(apActor);
        return false;
    }

    if (placed.SayAfter && cNow - placed.MovedAt >= std::chrono::seconds(1))
    {
        placed.SayAfter = false;
        spdlog::info("CorpseDiag: {:X}'s ragdoll now lies {:.0f} units from where its owner's corpse is", apActor->formID, cApart);
    }

    // Laid out once in the owner's last pose, wherever it lies: how it lies differs even when where does not.
    const bool cSettle = SettleIntoOwnerPose(cDead, placed.HasOwnerPose, placed.Settle);
    const MoveKind cKind = cSettle ? MoveKind::Settle : body.KnockedWhereOwnerFell && !cDead ? MoveKind::KnockPlace : MoveKind::Place;

    // The same 64 units as the reference: a settled ragdoll is not pulled around every frame. A blend under way finishes.
    if (cApart <= kApartUnits && !cSettle && placed.BlendSteps == 0)
        return false;
    // A knocked copy is put where its owner's fell only once its own fall has come to rest.
    if (cKind == MoveKind::KnockPlace)
    {
        const float* pVelocity = reinterpret_cast<const float*>(bodies[0].pHavok + kAngularVelocityOffset - 0x10);
        const float cSpeed = glm::length(glm::vec3{pVelocity[0], pVelocity[1], pVelocity[2]}) / cScale;
        if (!KnockedAtRest(KnockState(apActor->actorState.flags1), cSpeed))
            return false;
    }

    // The owner's corpse is somewhere else now: a new place, new tries.
    if (glm::distance(placed.WantedAt, acWanted) > 64.f)
    {
        placed.WantedAt = acWanted;
        placed.ApartBeforeMove = -1.f;
        placed.Fruitless = 0;
        placed.SaidInView = false;
        placed.BlendStep = 0;
        placed.BlendSteps = 0;
    }
    // A player would see the move as a jump, so it is blended in or waits until they see neither end of it.
    // Not a body whose owner's bones just stopped: the player sees it where its owner left it, and its ragdoll must be there too.
    const bool cJustPosed = cPosedAt && cNow - *cPosedAt < std::chrono::seconds(10);
    const bool cInView = !cJustPosed && PlayerHasView() && (IsInView(cRagdollRoot) || IsInView(acWanted));
    const bool cBlend = cInView;
    // Out of view, what is left of a blend is made at once.
    if (!cBlend)
    {
        placed.BlendStep = 0;
        placed.BlendSteps = 0;
    }
    if (cInView && !cBlend)
    {
        if (!placed.SaidInView)
            spdlog::info("CorpseDiag: {:X}'s ragdoll lies {:.0f} units from where its owner's corpse is, in the player's view; it waits until the player looks away",
                         apActor->formID, cApart);
        placed.SaidInView = true;
        return false;
    }
    // Never under this player's hand, nor while it may be the body this player loots.
    if (HeldHere(apActor, pRoot) || LootOpenNear(cRagdollRoot))
        return false;
    // Only a move's start backs off: the steps of a blend under way follow each other.
    if (cKind == MoveKind::Place && placed.BlendSteps == 0)
    {
        // Each move is a jump on screen, so a body that keeps moving is not pulled after it twice a second.
        // Moves in a row wait 1, 2, 4, 8, then 16 s, and 30 s without one start over.
        if (cNow - placed.MovedAt >= std::chrono::seconds(30))
            placed.Streak = 0;
        else if (cNow - placed.MovedAt < std::chrono::seconds(1ll << std::min(placed.Streak - 1, 4u)))
            return false;
        // A move that did not bring it nearer means the body settles back as far off, and repeating it makes the corpse jump.
        // After two such moves it is left where it lies until the owner's corpse moves.
        if (placed.ApartBeforeMove >= 0.f && cApart >= placed.ApartBeforeMove - 16.f && ++placed.Fruitless >= 2)
        {
            if (placed.Fruitless == 2)
                spdlog::info(
                    "CorpseDiag: {:X}'s ragdoll still lies {:.0f} units from where its owner's corpse is after its moves; left there until that moves", apActor->formID, cApart);
            return false;
        }
    }

    float blend = 1.f;
    if (cBlend)
    {
        if (placed.BlendSteps == 0)
        {
            placed.BlendSteps = BlendSteps(cSettle ? 0.f : cApart);
            placed.BlendStep = 0;
        }
        blend = BlendFraction(placed.BlendStep, placed.BlendSteps);
        placed.NextAt = cNow + std::chrono::milliseconds(kBlendStepMs);
    }
    const bool cBlendStep = blend < 1.f;

#ifdef SKYRIMVR
    // What the move ran beside, as for the other steps in a copy's life (CopyLife lines): once per move, not per blend step.
    if (placed.BlendStep == 0)
    {
        const uint32_t cThread = GetCurrentThreadId();
        spdlog::info(
            "CopyLife: ragdoll moved {:X}; thread {}{}, loading screen {}, {} loading screens so far, 3D {}", apActor->formID, cThread,
            cThread == GameMainThreadId() ? " (main)" : "", LoadingScreenUp() ? "up" : "down", UI::LoadingScreensQueued(), pRoot ? "yes" : "no");
    }
    const MoveOutcome cOutcome = MoveRagdoll(apActor, acWanted, cSettle ? &placed.OwnerPose : nullptr, cApart, blend);
    if (cBlendStep)
        FinishBlendStep(placed, cOutcome);
    else
        FinishMove(apActor->formID, placed, cKind, cOutcome, cApart, cNow, false);
    return BodyMoved(cOutcome);
#else
    // Made at the start of the next frame, outside the world's update, as the weapon bodies are put in the world; looked at again then.
    if (!CanMoveRagdolls(At<void*>(bodies[0].pHavok, kHkWorldOffset)))
        return false;
    {
        std::scoped_lock lock(s_pendingRagdollMovesLock);
        if (s_pendingRagdollMoves.size() >= 64)
            return false;
        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        s_pendingRagdollMoves.push_back(PendingRagdollMove{
            apActor->formID, acHow.Authority, cKind, acWanted, cApart, cSettle ? placed.OwnerPose : VRPose{}, cNow, apActor->parentCell ? apActor->parentCell->formID : 0,
            pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0, blend, cBlend});
    }
    // A blend step finishes no one-shot: only the blend's last step, a whole move, does.
    if (cBlendStep)
        placed.MovePending = true;
    else if (cKind == MoveKind::Settle)
        placed.Settle = OnQueued(placed.Settle);
    else if (cKind == MoveKind::KnockPlace)
        placed.KnockPlace = OnQueued(placed.KnockPlace);
    else
        placed.MovePending = true;
    return true;
#endif
}

bool MovesFlatRagdolls() noexcept
{
#ifdef SKYRIMVR
    return false;
#else
    return CanMoveRagdolls(nullptr);
#endif
}

// The bodies PlaceCorpse asked for, the nearest kMaxFollowed, set into their owners' bones under the world's write lock (main thread).
void FollowCarriedBodies() noexcept
{
    using namespace CarriedBodyFollow;
    std::scoped_lock lock(s_placedCorpsesLock, s_followLock);
    std::vector<FollowRequest> requests;
    requests.swap(s_followRequests);
    if (requests.empty() && s_followed.empty())
        return;

    // Nothing is looked up while the game exits or loads; what was followed is followed afresh after, as its skeleton may be made again.
    static const BSFixedString s_loading("Loading Menu");
    UI* pUI = IsProcessExiting() ? nullptr : UI::Get();
    if (!pUI || pUI->GetMenuOpen(s_loading))
    {
        s_followed.clear();
        return;
    }
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const float cScale = HavokWorldScale();
    const auto cNow = Clock::now();
    const glm::vec3 cPlayerAt = pPlayer ? glm::vec3{pPlayer->position.x, pPlayer->position.y, pPlayer->position.z} : glm::vec3{};
    const uint32_t cPlayerCell = pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0;

    std::vector<Candidate> candidates;
    std::vector<std::pair<const FollowRequest*, Actor*>> asked;
    for (const FollowRequest& request : requests)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(request.FormId));
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;
        // Still the copy it was asked for, as PlaceCorpse last saw it, lately, in the same cells, and the pose not a delayed frame's.
        const auto record = s_placedCorpses.find(request.FormId);
        if (!pRoot || !pPlayer || !(cScale > 0.f) || record == s_placedCorpses.end() || record->second.Authority != request.Authority || !request.Authority.Valid() ||
            cNow - record->second.AuthoritySeenAt >= std::chrono::seconds(1))
            continue;
        const Asked cNowStamp{cNow, pActor->GetHandle().handle.iBits, pActor->parentCell ? pActor->parentCell->formID : 0, cPlayerCell};
        if (!StillAsked(request.Stamp, cNowStamp))
            continue;
        BodyNow body{};
        body.RemoteNpc = pExtension && pExtension->IsRemote() && !pExtension->IsPlayer();
        body.Dead = pActor->actorState.IsDead();
        body.KnockRagdoll = pActor->actorState.IsInKnockRagdoll();
        body.Dying = pActor->actorState.IsDying();
        body.BleedingOut = pActor->actorState.IsBleedingOut();
        body.OwnerBonesMove = request.BonesMove;
        body.HeldHere = HeldHere(pActor, pRoot);
        if (!Follows(body))
            continue;
        const auto followed = s_followed.find(request.FormId);
        candidates.push_back(Candidate{
            request.FormId, glm::distance(ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate), cPlayerAt),
            followed != s_followed.end() && followed->second.Authority == request.Authority});
        asked.emplace_back(&request, pActor);
    }

    const std::vector<uint32_t> chosen = Choose(candidates);
    for (const auto& [pRequest, pActor] : asked)
    {
        if (std::find(chosen.begin(), chosen.end(), pRequest->FormId) == chosen.end())
            continue;
        void* pRoot = pActor->GetNiNode();
        TiltedPhoques::Vector<RagdollBody> bodies;
        void* pHkpWorld = CollectRagdoll(pRoot, bodies);
        if (!pHkpWorld || !CanMoveRagdolls(pHkpWorld))
            continue;

        const VRPose& pose = pRequest->Pose;
        const glm::vec3 cOwnerRoot{pose.RootPosition[0], pose.RootPosition[1], pose.RootPosition[2]};
        const bool cRootKnown = pose.HasRootPosition && std::isfinite(cOwnerRoot.x) && std::isfinite(cOwnerRoot.y) && std::isfinite(cOwnerRoot.z);
        const glm::vec3 cWanted = cRootKnown ? cOwnerRoot : pRequest->Wanted;
        const glm::vec3 cRagdollRoot = RagdollBodyPosition(bodies[0].pHavok, cScale);
        // A desync, not a carry: PlaceCorpse puts it right once the bones stop.
        if (glm::distance(cWanted, cRagdollRoot) > kMaxGapUnits)
            continue;

        WorldWriteLock worldLock(BhkWorldOf(pHkpWorld));
        if (!worldLock.Held())
            continue;
        FollowedBody& followed = s_followed[pRequest->FormId];
        if (followed.Authority != pRequest->Authority)
            followed = FollowedBody{pRequest->Authority};
        std::array<const void*, kMaxBodies> skeleton{};
        const size_t cKept = std::min<size_t>(bodies.size(), kMaxBodies);
        for (size_t i = 0; i < cKept; ++i)
            skeleton[i] = bodies[i].pHavok;
        const float cWeight = followed.State.Begin(cNow, bodies.size(), SkeletonOf(skeleton.data(), cKept));
        // The ragdoll root goes where the owner's root is, at the offset it had when the follow began.
        const glm::vec3 cRoot = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
        const glm::vec3 cShift = cWanted + followed.State.RootOffset(cRagdollRoot, cRoot) - cRagdollRoot;
        std::vector<BodyTarget> targets;
        // A creature goes where its owner has each body it sends, the others swing on their constraints; a person into its owner's bones.
        size_t sent = bodies.size();
        if (pose.NoBones ? !OwnerBodyTargets(pRequest->FormId, pRequest->Authority, bodies, cScale, targets, sent)
                         : !PoseTargets(pRoot, bodies, pose, cRoot + cShift, 0.f, cScale, targets))
        {
            OffsetTargets(bodies, cShift, cScale, targets);
            sent = bodies.size();
        }
        uint32_t written = 0;
        for (size_t i = 0; i < sent && i < kMaxBodies; ++i)
        {
            if (At<void*>(bodies[i].pHavok, kHkWorldOffset) != pHkpWorld)
                continue;
            const glm::vec3 cThere = glm::vec3{targets[i].Position[0], targets[i].Position[1], targets[i].Position[2]} / cScale;
            const glm::vec3 cAt = glm::mix(RagdollBodyPosition(bodies[i].pHavok, cScale), cThere, cWeight);
            const glm::quat cWantedTurn{targets[i].Rotation[3], targets[i].Rotation[0], targets[i].Rotation[1], targets[i].Rotation[2]};
            const glm::quat cTurn = glm::normalize(glm::slerp(RagdollBodyRotation(bodies[i].pHavok), cWantedTurn, cWeight));
            const glm::vec3 cVelocity = followed.State.Step(i, cAt);
            if (!std::isfinite(cAt.x) || !std::isfinite(cAt.y) || !std::isfinite(cAt.z) || !std::isfinite(cTurn.w))
                continue;
            BodyTarget set{};
            set.Position[0] = cAt.x * cScale;
            set.Position[1] = cAt.y * cScale;
            set.Position[2] = cAt.z * cScale;
            set.Rotation[0] = cTurn.x;
            set.Rotation[1] = cTurn.y;
            set.Rotation[2] = cTurn.z;
            set.Rotation[3] = cTurn.w;
            if (SetRagdollBody(bodies[i], set, cVelocity * cScale))
                ++written;
        }
        // Not followed: VR goes back to moving its tree.
        if (!written)
        {
            s_followed.erase(pRequest->FormId);
            continue;
        }
        // Its owner moves it (IsRemoteBodyGrabbedHere, BodyPelvisHeight, PlaceCorpse's CarriedLately).
        {
            std::scoped_lock posedLock(s_bodyPosedLock);
            s_bodyPosedByOwnerAt[pRequest->FormId] = cNow;
        }
        static std::unordered_map<uint32_t, Clock::time_point> s_nextSaid;
        if (s_nextSaid.size() > 256)
            s_nextSaid.clear();
        if (auto* pNext = spdlog::default_logger_raw()->should_log(spdlog::level::debug) ? &s_nextSaid[pRequest->FormId] : nullptr; pNext && cNow >= *pNext)
        {
            *pNext = cNow + std::chrono::seconds(5);
            spdlog::debug("CorpseDiag: {:X} follows its owner's bones ({} of {} bodies, weight {:.2f})", pRequest->FormId, written, bodies.size(), cWeight);
        }
    }

    for (auto it = s_followed.begin(); it != s_followed.end();)
    {
        if (!it->second.State.Lapsed(cNow))
        {
            ++it;
            continue;
        }
        spdlog::debug("CorpseDiag: {:X} is left to its own ragdoll", it->first);
        it = s_followed.erase(it);
    }
}

void MovePendingRagdolls() noexcept
{
#ifdef SKYRIMVR
    FollowCarriedBodies();
#else
    if (MovesFlatRagdolls())
        FollowCarriedBodies();
    using namespace CopyBodyRules;
    std::vector<PendingRagdollMove> moves;
    {
        std::scoped_lock lock(s_pendingRagdollMovesLock);
        moves.swap(s_pendingRagdollMoves);
    }
    if (moves.empty())
        return;

    static const BSFixedString s_loading("Loading Menu");
    UI* pUI = UI::Get();
    const bool cLoading = pUI && pUI->GetMenuOpen(s_loading);
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const uint32_t cPlayerCell = pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0;
    const auto cNow = std::chrono::steady_clock::now();

    std::scoped_lock corpsesLock(s_placedCorpsesLock);
    for (const PendingRagdollMove& move : moves)
    {
        // Looked at again a frame later: every gate it was decided under, under the same copy and owner.
        const auto record = s_placedCorpses.find(move.FormId);
        Actor* pActor = Cast<Actor>(TESForm::GetById(move.FormId));
        const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
        void* pRoot = pActor ? pActor->GetNiNode() : nullptr;

        QueuedMoveNow now{};
        now.Kind = move.Kind;
        now.AuthorityKnown = record != s_placedCorpses.end();
        now.SameAuthority = now.AuthorityKnown && record->second.Authority == move.Authority && move.Authority.Valid();
        now.AuthorityFresh = now.AuthorityKnown && cNow - record->second.AuthoritySeenAt < std::chrono::seconds(1);
        now.StillRemote = pExtension && pExtension->IsRemote() && !pExtension->IsPlayer();
        now.SameCell = pActor && pActor->parentCell && pActor->parentCell->formID == move.ActorCell && cPlayerCell == move.PlayerCell;
        now.Loading = cLoading;
        if (pActor)
        {
            now.Dead = pActor->actorState.IsDead();
            now.Dying = pActor->actorState.IsDying();
            now.BleedingOut = pActor->actorState.IsBleedingOut();
            now.KnockRagdoll = pActor->actorState.IsInKnockRagdoll();
            now.HeldHere = HeldHere(pActor, pRoot);
        }
        now.OwnerBonesSince = now.AuthorityKnown && record->second.BonesSeenAt > move.QueuedAt;
        now.Blended = move.Blended;
        if (pRoot && now.StillRemote && now.SameCell && !cLoading)
        {
            const float cScale = HavokWorldScale();
            TiltedPhoques::Vector<RagdollBody> bodies;
            if (cScale > 0.f && CollectRagdoll(pRoot, bodies))
            {
                const glm::vec3 cRoot = RagdollBodyPosition(bodies[0].pHavok, cScale);
                now.InView = PlayerHasView() && (IsInView(cRoot) || IsInView(move.Wanted));
                now.LootOpenNear = LootOpenNear(cRoot);
            }
        }

        MoveOutcome outcome = MoveOutcome::Canceled;
        if (StillAuthorized(now))
            outcome = MoveRagdoll(pActor, move.Wanted, move.Kind == MoveKind::Settle ? &move.Pose : nullptr, move.Apart, move.Blend);
        else
            spdlog::debug(
                "CorpseDiag: {:X}: {} was canceled before it ran (same copy {}, remote {}, same cells {}, loading {}, dead {}, knocked {}, its owner's bones since {}, "
                "in view {}, held {}, loot open {})",
                move.FormId, MoveKindName(move.Kind), now.SameAuthority && now.AuthorityFresh, now.StillRemote, now.SameCell, now.Loading, now.Dead, now.KnockRagdoll,
                now.OwnerBonesSince, now.InView, now.HeldHere, now.LootOpenNear);
        // Its record is the copy's only while it is the same copy: a forgotten or replaced one has nothing to finish.
        if (now.SameAuthority && move.Blend < 1.f)
            FinishBlendStep(record->second, outcome);
        else if (now.SameAuthority)
            FinishMove(move.FormId, record->second, move.Kind, outcome, move.Apart, cNow, true);
    }
#endif
}

/**
 * List the names of the nodes in an actor's 3D whose name contains a part, in any case, for checks that cannot look at the screen (the rig).
 *
 * @param apActor The actor.
 * @param acpPart The part of the name to look for.
 * @param aMax The most names listed.
 * @param aCount Receives how many nodes have such a name in all.
 * @return The names, comma separated; empty when there is none.
 */
std::string NodesNamedLike(Actor* apActor, const char* acpPart, size_t aMax, size_t& aCount) noexcept
{
    aCount = 0;
    std::string out;
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot || !acpPart)
        return out;
    std::string part(acpPart);
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    VisitTree(pRoot, 4096, [&](void* pObject) {
        const char* pName = GetName(pObject);
        if (!pName)
            return;
        std::string name(pName);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find(part) != std::string::npos)
        {
            if (aCount < aMax)
                out += (out.empty() ? "" : ", ") + name;
            ++aCount;
        }
    });
    return out;
}

// The VR nodes (bow aim, arrow snap and hold) hang below PlayerWorldNode, found up from the headset node.
void* LocalWorldNode(PlayerCharacter* apPlayer) noexcept
{
    if (!apPlayer || !VRControllers::HeadsetTransform())
        return nullptr;
    void* pNode = At<void*>(apPlayer, kHmdNodeOffset);
    for (int depth = 0; pNode && depth < 16; ++depth, pNode = At<void*>(pNode, kParentOffset))
        if (const char* pName = GetName(pNode); pName && std::strcmp(pName, "PlayerWorldNode") == 0)
            return pNode;
    return nullptr;
}

/**
 * Log the local player's nodes named like an arrow (VR, first and third person) when their names, parents or hidden flags change.
 * Measurement only: where the game keeps the arrow taken from the quiver and the one on the string.
 * It walks the nodes at most every 200 ms.
 *
 * @param apPlayer The local player.
 * @param aAttackState The player's attack state, written in the line.
 */
void LogLocalArrowNodes(PlayerCharacter* apPlayer, uint32_t aAttackState) noexcept
{
#ifdef SKYRIMVR
    constexpr uint32_t cFlagsOffset = 0x10C; // NiAVObject::flags on VR, bit 0 hidden (as SpawnReveal)
    constexpr size_t cMaxNodes = 8192;
    constexpr size_t cMaxListed = 24;

    // The walk below only feeds a log line, so a log that does not take debug lines is not worth its scene walk.
    if (!spdlog::default_logger_raw()->should_log(spdlog::level::debug))
        return;

    static std::chrono::steady_clock::time_point s_nextWalk{};
    static std::string s_last;
    const auto now = std::chrono::steady_clock::now();
    if (!apPlayer || now < s_nextWalk)
        return;
    s_nextWalk = now + std::chrono::milliseconds(200);

    void* pWorldNode = LocalWorldNode(apPlayer);

    // The first-person skeleton, from its root bone's parent, in case it does not hang below PlayerWorldNode.
    void* pFirstPerson = nullptr;
    {
        void* pNode = FirstPersonHand(apPlayer, 1);
        for (int depth = 0; pNode && depth < 32 && !pFirstPerson; ++depth, pNode = At<void*>(pNode, kParentOffset))
            if (const char* pName = GetName(pNode); pName && std::strcmp(pName, "NPC Root [Root]") == 0)
                pFirstPerson = At<void*>(pNode, kParentOffset);
    }

    std::array<glm::vec3, 2> hands{};
    std::array<bool, 2> hasHand{};
    for (size_t side = 0; side < 2; ++side)
        if (void* pHand = FirstPersonHand(apPlayer, side))
        {
            hands[side] = ToGlm(At<NiTransform>(pHand, kWorldOffset).translate);
            hasHand[side] = true;
        }

    // What decides a new line: names, parents and hidden flags. Where the nodes are is only reported.
    std::string signature;
    std::string details;
    size_t count = 0;
    // Its own walk, not VisitTree: three roots share one queue, and a node reached from two of them is listed once.
    std::unordered_set<void*> seen;
    std::vector<void*> queue;
    for (void* pRoot : {pWorldNode, pFirstPerson, static_cast<void*>(apPlayer->GetNiNode())})
        if (pRoot)
            queue.push_back(pRoot);
    for (size_t head = 0; head < queue.size() && head < cMaxNodes; ++head)
    {
        void* pObject = queue[head];
        if (!seen.insert(pObject).second)
            continue;
        if (const char* pName = GetName(pObject))
        {
            std::string lower(pName);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("arrow") != std::string::npos && ++count <= cMaxListed)
            {
                void* pParent = At<void*>(pObject, kParentOffset);
                const char* pParentName = pParent ? GetName(pParent) : nullptr;
                const bool cHidden = (At<uint32_t>(pObject, cFlagsOffset) & 1u) != 0;
                const NiTransform& world = At<NiTransform>(pObject, kWorldOffset);
                const std::string cEntry = fmt::format("'{}' under '{}'{}{}", pName, pParentName ? pParentName : "none", cHidden ? " hidden" : "", world.scale < 0.01f ? " scale 0" : "");
                signature += cEntry + ";";
                const glm::vec3 cAt = ToGlm(world.translate);
                details += fmt::format("{}[{}, right hand {:.0f}, left hand {:.0f}]", details.empty() ? "" : " ", cEntry, hasHand[1] ? glm::distance(cAt, hands[1]) : -1.f,
                                       hasHand[0] ? glm::distance(cAt, hands[0]) : -1.f);
            }
        }
        if (void* pNode = AsNode(pObject))
            ForEachChild(pNode, [&queue](void* pChild) { queue.push_back(pChild); });
    }

    signature += fmt::format("world node {}, first person {}", pWorldNode != nullptr, pFirstPerson != nullptr);
    if (signature == s_last)
        return;
    s_last = signature;
    spdlog::info("ArrowDiag: local attack state {}, world node {}, first-person skeleton {}, {} node(s) named like an arrow: {}", aAttackState, pWorldNode ? "found" : "missing",
                 pFirstPerson ? "found" : "missing", count, details.empty() ? "none" : details);
#else
    (void)apPlayer;
    (void)aAttackState;
#endif
}

// The heading the last pose capture sent: the body's facing, or the game's heading when the body was not found.
static float s_sentHeading = 0.f;
static bool s_hasSentHeading = false;

// Where a VRIK body faces, from its thighs: at right angles to the line from the left one to the right one.
// Skyrim's heading: 0 faces north (+y) and grows clockwise, towards east (+x).
static bool BodyFacing(void* apLeftThigh, void* apRightThigh, float& aOutHeading) noexcept
{
    if (!apLeftThigh || !apRightThigh)
        return false;
    const glm::vec3 cRight = ToGlm(At<NiTransform>(apRightThigh, kWorldOffset).translate) - ToGlm(At<NiTransform>(apLeftThigh, kWorldOffset).translate);
    if (!std::isfinite(cRight.x) || !std::isfinite(cRight.y) || cRight.x * cRight.x + cRight.y * cRight.y < 1.f)
        return false;
    aOutHeading = std::atan2(-cRight.y, cRight.x);
    return true;
}

void NoteHeading(PlayerCharacter* apPlayer) noexcept
{
#ifdef SKYRIMVR
    constexpr float cDegrees = 180.f / glm::pi<float>();
    constexpr auto cSampleEvery = std::chrono::milliseconds(100);
    constexpr auto cReportEvery = std::chrono::seconds(30);

    struct Totals
    {
        uint32_t Samples = 0;
        float HeadsetGap = 0.f, HeadsetGapMax = 0.f;
        float BodyGap = 0.f, BodyGapMax = 0.f;
        float HeadsetBodyGap = 0.f;
        float Turned = 0.f, TurnedBodyStill = 0.f;
    };
    static Totals s_totals;
    static std::chrono::steady_clock::time_point s_nextSample{}, s_nextReport{};
    static void* s_root = nullptr;
    static std::array<void*, 2> s_thighs{};
    static std::chrono::steady_clock::time_point s_searchAt{};
    static bool s_hasLast = false;
    static float s_lastSent = 0.f, s_lastBody = 0.f;

    const auto cNow = std::chrono::steady_clock::now();
    if (!apPlayer || cNow < s_nextSample)
        return;
    s_nextSample = cNow + cSampleEvery;
    if (s_nextReport == std::chrono::steady_clock::time_point{})
        s_nextReport = cNow + cReportEvery;

    // A difference of two headings, in degrees from -180 to 180.
    const auto gap = [](float aFrom, float aTo)
    {
        float d = std::fmod((aTo - aFrom) * cDegrees, 360.f);
        if (d > 180.f)
            d -= 360.f;
        else if (d < -180.f)
            d += 360.f;
        return d;
    };
    // Skyrim's heading: 0 faces north (+y) and grows clockwise, towards east (+x).
    const auto headingOf = [](const glm::vec3& acForward) { return std::atan2(acForward.x, acForward.y); };

    // The thighs of the body VRIK poses, found once per 3D (and looked for again every five seconds while missing).
    void* pRoot = apPlayer->GetNiNode();
    if (pRoot != s_root || (!s_thighs[0] && cNow >= s_searchAt))
    {
        s_root = pRoot;
        s_thighs = {FindShallowest(pRoot, "NPC L Thigh [LThg]"), FindShallowest(pRoot, "NPC R Thigh [RThg]")};
        s_searchAt = cNow + std::chrono::seconds(5);
        s_hasLast = false;
    }

    glm::vec3 headAt, headForward, headRight;
    if (!s_thighs[0] || !s_thighs[1] || !HeadsetPose(headAt, headForward, headRight))
        return;

    float bodyFacing = 0.f;
    if (!BodyFacing(s_thighs[0], s_thighs[1], bodyFacing))
        return;
    const float cHeadset = headingOf(headForward);
    const float cSent = s_hasSentHeading ? s_sentHeading : apPlayer->rotation.z;

    Totals& t = s_totals;
    ++t.Samples;
    const float cHeadsetGap = std::abs(gap(cSent, cHeadset));
    const float cBodyGap = std::abs(gap(cSent, bodyFacing));
    t.HeadsetGap += cHeadsetGap;
    t.HeadsetGapMax = std::max(t.HeadsetGapMax, cHeadsetGap);
    t.BodyGap += cBodyGap;
    t.BodyGapMax = std::max(t.BodyGapMax, cBodyGap);
    t.HeadsetBodyGap += std::abs(gap(bodyFacing, cHeadset));
    if (s_hasLast)
    {
        // The turns of the sent heading, and the part of them made while the body itself stayed put: those are head turns.
        const float cTurn = std::abs(gap(s_lastSent, cSent));
        t.Turned += cTurn;
        if (std::abs(gap(s_lastBody, bodyFacing)) < 1.f)
            t.TurnedBodyStill += cTurn;
    }
    s_hasLast = true;
    s_lastSent = cSent;
    s_lastBody = bodyFacing;

    if (cNow < s_nextReport)
        return;
    s_nextReport = cNow + cReportEvery;
    const float n = static_cast<float>(t.Samples);
    spdlog::info("HeadingDiag: {} samples in 30 s; sent heading off the headset by {:.0f} deg on average (most {:.0f}), off the body (thighs) by {:.0f} (most {:.0f}); "
                 "headset off the body by {:.0f}; the sent heading turned {:.0f} deg, {:.0f} of it while the body stayed still; now sent {:.0f}, headset {:.0f}, body {:.0f}",
                 t.Samples, t.HeadsetGap / n, t.HeadsetGapMax, t.BodyGap / n, t.BodyGapMax, t.HeadsetBodyGap / n, t.Turned, t.TurnedBodyStill, cSent * cDegrees, cHeadset * cDegrees,
                 bodyFacing * cDegrees);
    s_totals = {};
#else
    (void)apPlayer;
#endif
}

bool IsWeaponTouchAt(const glm::vec3& acPoint) noexcept
{
    POINTER_SKYRIMSE(float, s_havokScale, 231896, 231896);
    if (!s_havokScale.Get() || !(*s_havokScale.Get() > 0.f))
        return false;
    const float cScale = *s_havokScale.Get();
    const glm::vec3 cHavokPoint = acPoint * cScale;
    // PLANCK reads the same contact point, so the two match up to rounding; 3 units of slack.
    const float cTolerance = 3.f * cScale;
    const auto cNow = std::chrono::steady_clock::now();
    std::lock_guard lock(s_contactsLock);
    for (const auto& touch : s_recentTouches)
        if (touch.At.time_since_epoch().count() && cNow - touch.At < std::chrono::milliseconds(500) && glm::distance(touch.Point, cHavokPoint) <= cTolerance)
            return true;
    return false;
}

bool PlayClashSparks(const glm::vec3& acPoint) noexcept
{
    // The game's own sparks for a one-handed blade on metal (Skyrim.esm WPNBlade1HandVsMetaImpact, 0x4BB52), shown for a second.
    using TSpawnParticle = void*(TESObjectCELL* apCell, float aLifetime, const char* apModel, const NiPoint3& acRotation, const NiPoint3& acPosition, float aScale,
                                 uint32_t aFlags, void* apTarget);
    POINTER_SKYRIMSE(TSpawnParticle, s_spawnParticle, 30071, 29218); // BSTempEffectParticle::Spawn (CommonLibVR-NG)
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!s_spawnParticle.Get() || !pCell)
        return false;
    NiPoint3 rotation;
    rotation.x = rotation.y = rotation.z = 0.f;
    NiPoint3 position;
    position.x = acPoint.x;
    position.y = acPoint.y;
    position.z = acPoint.z;
    const bool cSpawned = s_spawnParticle.Get()(pCell, 1.f, "Effects\\ImpactEffects\\FXMetalSparkImpactSlice.nif", rotation, position, 1.f, 7, nullptr) != nullptr;
    static std::chrono::steady_clock::time_point s_nextSaid{};
    if (const auto cNow = std::chrono::steady_clock::now(); cNow >= s_nextSaid)
    {
        s_nextSaid = cNow + std::chrono::seconds(5);
        spdlog::debug("Clash: sparks at ({:.0f}){}", acPoint, cSpawned ? "" : " not shown (the game made no effect)");
    }
    return cSpawned;
}

bool PlayClashSound(const glm::vec3& acPoint) noexcept
{
    // Skyrim.esm WPNBlockBlade1HandVsOtherSD: the game's own sound for a one-handed blade blocked by a weapon.
    // It is what the sound marker WPNBlockBladeVsOther (0x137D0) plays.
    constexpr uint32_t kBladeBlockSound = 0x3C73C;
    NiPoint3 position;
    position.x = acPoint.x;
    position.y = acPoint.y;
    position.z = acPoint.z;
    return GameSound::Play(kBladeBlockSound, &position);
}

// The pose's kept fields (weapons, fingers, scale) are sent when they change and again this often.
// Movement and the pose travel unreliable, so a change lost on the way is made good within a quarter second.
// The resend also gives them to a player who arrives later.
constexpr auto kKeptFieldsResend = std::chrono::milliseconds(250);

// Where this player's weapons are held, relative to the third-person hands the pose is read from (VRPose::HasWeapons).
// In VR the visible weapon hangs off the first-person hand at the angle VR holds it, not at the body's own attach node.
// The attach nodes are searched twice a second, as ResolveHolding does, and read in between.
void CaptureWeapons(PlayerCharacter* apPlayer, void* apRoot, const BoneNodes& acNodes, VRPose& aOutPose, const std::chrono::steady_clock::time_point aNow) noexcept
{
    struct Side
    {
        void* pAttach = nullptr; // first person: the one that is seen and swung
        void* pAttachVTable = nullptr;
    };
    static std::array<Side, 2> s_sides{};
    static std::chrono::steady_clock::time_point s_searchAt{};
    static void* s_searchedRoot = nullptr;

    if (aNow >= s_searchAt || s_searchedRoot != apRoot)
    {
        s_searchAt = aNow + std::chrono::milliseconds(500);
        s_searchedRoot = apRoot;
        for (size_t side = 0; side < 2; ++side)
        {
            Side& found = s_sides[side];
            found = Side{};
            // Searched from the forearm, not the hand: the shield node hangs off the forearm's twist bone.
            if (void* pHand = FirstPersonHand(apPlayer, side))
            {
                void* pForearm = At<void*>(pHand, kParentOffset);
                void* pAttach = FindShallowest(pForearm ? pForearm : pHand, kAttachNames[side]);
                if (pAttach && AsNode(pAttach))
                {
                    found.pAttach = pAttach;
                    found.pAttachVTable = *static_cast<void**>(pAttach);
                }
            }
        }
    }

    std::array<bool, 2> held{};
    std::array<Quaternion_NetQuantize, 2> rotation{};
    std::array<glm::vec3, 2> offset{};
    for (size_t side = 0; side < 2; ++side)
    {
        const Side& found = s_sides[side];
        void* pHand = acNodes[side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        // Nothing hanging off the attach node: nothing held in that hand.
        if (!pHand || !HasSameVTable(found.pAttach, found.pAttachVTable) || (ChildFingerprintOf(found.pAttach) & 0xFFFF) == 0)
            continue;

        const NiTransform& hand = At<NiTransform>(pHand, kWorldOffset);
        const NiTransform& grip = At<NiTransform>(found.pAttach, kWorldOffset);
        // The grip as drawn: a blade held still by the blade stop is sent where it rests, so the other player sees it stopped too.
        const glm::mat3 gripRotation = DrawnGripRotation(side, grip.rotate);
        const glm::mat3 inverseHand = glm::transpose(ToGlm(hand.rotate));
        const glm::vec3 at = inverseHand * (ToGlm(grip.translate) - ToGlm(hand.translate)) / std::max(hand.scale, 0.05f);
        // The first-person hand is the controller; the body's hand is where VRIK got the arm to.
        // More than 40 units apart means the arm did not reach, and the grip means nothing for the copy.
        if (!IsFiniteVector(at) || glm::length(at) > 40.f)
            continue;

        held[side] = true;
        rotation[side] = glm::normalize(glm::quat_cast(inverseHand * gripRotation));
        offset[side] = at;
    }

    // Sent when the grip changes or every kKeptFieldsResend, like the fingers; the receiver keeps the last.
    static std::array<bool, 2> s_lastHeld{};
    static std::array<Quaternion_NetQuantize, 2> s_lastRotation{};
    static std::array<glm::vec3, 2> s_lastOffset{};
    static std::chrono::steady_clock::time_point s_lastSentAt{};
    bool changed = held != s_lastHeld;
    for (size_t side = 0; side < 2 && !changed; ++side)
        changed = held[side] && (rotation[side] != s_lastRotation[side] || glm::distance(offset[side], s_lastOffset[side]) > 0.05f);

    aOutPose.HasWeapons = false;
    if (!changed && aNow - s_lastSentAt < kKeptFieldsResend)
        return;

    s_lastHeld = held;
    s_lastRotation = rotation;
    s_lastOffset = offset;
    s_lastSentAt = aNow;
    aOutPose.HasWeapons = true;
    aOutPose.WeaponHeld = held;
    aOutPose.WeaponRotation = rotation;
    for (size_t side = 0; side < 2; ++side)
        for (int axis = 0; axis < 3; ++axis)
            aOutPose.WeaponOffset[side][axis] = offset[side][axis];
}

// What LastLocalPose sends again, without the fields sent only when they change, which the receiver keeps, or only while a spell is held.
static VRPose s_lastLocalPose{};
static void* s_lastLocalRoot = nullptr;

bool LastLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept
{
    if (!s_lastLocalPose.HasData || apPlayer->GetNiNode() == s_lastLocalRoot)
        return false;
    aOutPose = s_lastLocalPose;
    return true;
}

// The arrow the local player holds in a hand, as CaptureArrowHold last found it, and how far apart the hands were then.
std::mutex s_arrowHoldLock;
ArrowHold s_arrowHold;
float s_handsApart = -1.f;
std::chrono::steady_clock::time_point s_arrowReadAt{};
constexpr auto kArrowReadLifetime = std::chrono::milliseconds(250);

// The game holds the arrow taken from the quiver in "ArrowNode" below PlayerWorldNode, at "ArrowHoldOffsetNode" of the hand's wand node.
// Its model "Arrow:0" is shown while the arrow is in the hand; far from every hand, or hidden, there is none.
// Its place goes in the space of the hand's third-person bone, which the other players' copies pose from this player's.
void CaptureArrowHold(PlayerCharacter* apPlayer, const BoneNodes& acNodes, const std::chrono::steady_clock::time_point aNow) noexcept
{
    constexpr uint32_t cFlagsOffset = 0x10C; // NiAVObject::flags on VR, bit 0 hidden (as SpawnReveal)
    static std::chrono::steady_clock::time_point s_nextLook{};
    if (aNow < s_nextLook)
        return;
    s_nextLook = aNow + std::chrono::milliseconds(100);

    ArrowHold hold{};
    const auto cFound = [&]() {
        void* pWorldNode = LocalWorldNode(apPlayer);
        void* pArrowNode = pWorldNode ? FindShallowest(pWorldNode, "ArrowNode") : nullptr;
        void* pModel = pArrowNode ? FindShallowest(pArrowNode, "Arrow:0") : nullptr;
        void* pHoldNode = pWorldNode ? FindShallowest(pWorldNode, "ArrowHoldOffsetNode") : nullptr;
        if (!pModel || !pHoldNode || (At<uint32_t>(pArrowNode, cFlagsOffset) & 1u) || (At<uint32_t>(pModel, cFlagsOffset) & 1u))
            return false;
        const NiTransform& model = At<NiTransform>(pModel, kWorldOffset);
        if (model.scale < 0.01f)
            return false;
        // The hand is the one whose wand node holds the hold node.
        void* pHoldParent = At<void*>(pHoldNode, kParentOffset);
        void* pWand = pHoldParent ? At<void*>(pHoldParent, kParentOffset) : nullptr;
        const char* pWandName = pWand ? GetName(pWand) : nullptr;
        if (!pWandName)
            return false;
        hold.Side = std::strcmp(pWandName, "LeftWandNode") == 0 ? 0 : 1;
        // The arrow sits at the hold node while it is in the hand, and is sent far away when it is not.
        if (glm::distance(ToGlm(At<NiTransform>(pArrowNode, kWorldOffset).translate), ToGlm(At<NiTransform>(pHoldNode, kWorldOffset).translate)) > 20.f)
            return false;
        void* pHand = acNodes[hold.Side == 0 ? VRPose::kLeftHand : VRPose::kRightHand];
        if (!pHand)
            return false;
        const NiTransform& hand = At<NiTransform>(pHand, kWorldOffset);
        const glm::mat3 cInverseHand = glm::transpose(ToGlm(hand.rotate));
        hold.Offset = cInverseHand * (ToGlm(model.translate) - ToGlm(hand.translate)) / std::max(hand.scale, 0.05f);
        hold.Rotation = glm::normalize(glm::quat_cast(cInverseHand * ToGlm(model.rotate)));
        // The head lies about an arrow's length from the hand; farther, the body's hand did not reach the controller.
        return IsFiniteVector(hold.Offset) && glm::length(hold.Offset) < 150.f;
    };
    hold.InHand = cFound();
    // The body's hands, as the other players' copies pose them: the nock is where those meet.
    void* pLeft = acNodes[VRPose::kLeftHand];
    void* pRight = acNodes[VRPose::kRightHand];
    const float cApart = pLeft && pRight ? glm::distance(ToGlm(At<NiTransform>(pLeft, kWorldOffset).translate), ToGlm(At<NiTransform>(pRight, kWorldOffset).translate)) : -1.f;
    std::scoped_lock lock(s_arrowHoldLock);
    s_arrowHold = hold;
    s_handsApart = std::isfinite(cApart) ? cApart : -1.f;
    s_arrowReadAt = aNow;
}

float LocalArrowHandsApart() noexcept
{
    std::scoped_lock lock(s_arrowHoldLock);
    return std::chrono::steady_clock::now() - s_arrowReadAt <= kArrowReadLifetime ? s_handsApart : -1.f;
}

ArrowHold LocalArrowHold() noexcept
{
    std::scoped_lock lock(s_arrowHoldLock);
    return std::chrono::steady_clock::now() - s_arrowReadAt <= kArrowReadLifetime ? s_arrowHold : ArrowHold{};
}

// Where this player's held spell went last.
// A held spell turns to its hand each frame, so one that has not for this long has ended.
std::mutex s_spellAimLock;
SpellAimRules::LocalAim s_spellAim;

static double SpellAimTime(const std::chrono::steady_clock::time_point aNow) noexcept
{
    return std::chrono::duration<double, std::milli>(aNow.time_since_epoch()).count();
}

void NoteSpellAim(const float aZAngle, const float aXAngle) noexcept
{
    std::scoped_lock lock(s_spellAimLock);
    s_spellAim.Note(aZAngle, aXAngle, SpellAimTime(std::chrono::steady_clock::now()));
}

// The pose's spell aim, from the last moments.
void FillSpellAim(VRPose& aOutPose, const std::chrono::steady_clock::time_point aNow) noexcept
{
    std::scoped_lock lock(s_spellAimLock);
    s_spellAim.Fill(aOutPose, SpellAimTime(aNow));
}

void ResetLocalSpellAim() noexcept
{
    std::scoped_lock lock(s_spellAimLock);
    s_spellAim.Reset();
}

bool CaptureSpellAimOnly(VRPose& aOutPose) noexcept
{
    std::scoped_lock lock(s_spellAimLock);
    return s_spellAim.Capture(aOutPose, SpellAimTime(std::chrono::steady_clock::now()));
}

bool HasBodySolver() noexcept
{
    static BodySolver::Watch s_watch;
    const auto cNow = std::chrono::steady_clock::now();
    if (!s_watch.Due(cNow))
        return s_watch.Loaded();
    const bool cLoaded = GetModuleHandleW(L"vrik.dll") != nullptr;
    if (s_watch.Take(cLoaded, cNow) == BodySolver::Watch::Said::kChanged)
    {
        if (cLoaded)
            spdlog::info("VRBodySync: VRIK found; the other players see this body's pose");
        else
            spdlog::info("VRBodySync: no VRIK, so the body is not fitted to the headset and controllers; while a weapon or spell is "
                         "out the other players see its own drawn stance and the held spell's aim, not its pose");
    }
    return cLoaded;
}

bool CopySpellAim(const uint32_t aFormId, float& aZAngle, float& aXAngle) noexcept
{
    std::shared_lock lock(s_posesLock);
#ifndef SKYRIMVR
    if (const auto cAim = s_spellAims.find(aFormId); cAim != s_spellAims.end() && SpellAimRules::Finite(cAim->second[0], cAim->second[1]))
    {
        aZAngle = cAim->second[0];
        aXAngle = cAim->second[1];
        return true;
    }
#endif
    const auto it = s_poses.find(aFormId);
    if (it == s_poses.end() || !it->second.Available(std::chrono::steady_clock::now()) || !it->second.HasSpellAim ||
        !SpellAimRules::Finite(it->second.SpellAim[0], it->second.SpellAim[1]))
        return false;
    aZAngle = it->second.SpellAim[0];
    aXAngle = it->second.SpellAim[1];
    return true;
}

#ifndef SKYRIMVR
void SetRemoteSpellAim(const uint32_t aFormId, const VRPose& acPose) noexcept
{
    std::unique_lock lock(s_posesLock);
    if (!acPose.HasSpellAim)
        s_spellAims.erase(aFormId);
    else
        s_spellAims[aFormId] = {acPose.SpellAim[0], acPose.SpellAim[1]};
}
#endif

// TEMPORARY diagnostic for a player copy seen flipping between two poses several times a second; remove once understood.
// A pose that steps far from the one before and comes back within three poses is a body read in another state, not a movement.
// Watched where poses are read (the local player) and where they arrive (each remote player), so one log tells which side flips.
// The frame-end counts in OnFrameEnd belong to it.
namespace
{
constexpr std::array<const char*, VRPose::kUpperBoneCount> kUpperBoneNames{"spine1", "spine2", "neck",       "head",        "left clavicle",  "left upper arm",
                                                                             "left forearm", "left hand", "right clavicle", "right upper arm", "right forearm", "right hand"};

// A body's upper-body bone rotations and the hips' horizontal place, in one frame of reference, to tell how far it moved between two reads.
struct PoseReading
{
    glm::vec2 Hips{};
    bool HasHips = false;
    std::array<glm::quat, VRPose::kUpperBoneCount> Bones{};
};

struct PoseStepWatch
{
    PoseReading Last{};
    bool HasLast = false;
    // The reading before the last big step, while the stream may still come back to it.
    PoseReading Before{};
    uint32_t SinceStep = 0;
    bool Watching = false;
    void* pLastRoot = nullptr;
    uint64_t LastTick = 0;
    uint32_t Poses = 0;
    uint32_t Steps = 0;
    uint32_t Returns = 0;
    uint32_t RootChanges = 0;
    uint32_t Late = 0;
    float WorstHips = 0.f;
    float WorstBone = 0.f;
    uint32_t WorstBoneIndex = 0;
    std::chrono::steady_clock::time_point NextLog{};
};

std::mutex s_poseWatchLock;
std::unordered_map<uint32_t, PoseStepWatch> s_poseWatches;

PoseReading ReadingOf(const VRPose& acPose) noexcept
{
    PoseReading reading;
    reading.HasHips = acPose.HasHips;
    reading.Hips = glm::vec2{acPose.HipOffset[0], acPose.HipOffset[1]};
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
        reading.Bones[i] = static_cast<glm::quat>(acPose.Bones[i]);
    return reading;
}

// The horizontal distance between the two hips, and the largest turn of one bone other than the hands between them.
// The hands are left out because a wrist can flick that fast in a swing.
void StepBetween(const PoseReading& acFrom, const PoseReading& acTo, float& aHips, float& aBone, uint32_t& aBoneIndex) noexcept
{
    aHips = acFrom.HasHips && acTo.HasHips ? glm::distance(acFrom.Hips, acTo.Hips) : 0.f;
    aBone = 0.f;
    aBoneIndex = 0;
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        if (i == VRPose::kLeftHand || i == VRPose::kRightHand)
            continue;
        const float cDegrees = DegreesBetween(acFrom.Bones[i], acTo.Bones[i]);
        if (cDegrees > aBone)
        {
            aBone = cDegrees;
            aBoneIndex = i;
        }
    }
}

constexpr float cStepHips = 20.f;
constexpr float cStepBone = 45.f;

// One pose read or received; logs at most once per period, and only when the stream stepped far or the body read changed.
// aTick is the sender's tick of a received pose, 0 for a pose read here.
void WatchPoseSteps(const uint32_t aKey, const uint64_t aTick, const PoseReading& acReading, void* apRoot, const char* acpWho, const std::chrono::seconds aPeriod) noexcept
{
    constexpr float cBackHips = 10.f;
    constexpr float cBackBone = 20.f;
    constexpr uint32_t cBackWithin = 3;

    std::scoped_lock lock(s_poseWatchLock);
    if (s_poseWatches.size() > 256)
        s_poseWatches.clear();
    PoseStepWatch& watch = s_poseWatches[aKey];
    const auto cNow = std::chrono::steady_clock::now();
    if (watch.NextLog == std::chrono::steady_clock::time_point{})
        watch.NextLog = cNow + aPeriod;

    // Movement arrives unreliably, so a pose can come after a newer one, or twice.
    // Taken in arrival order it would look like a step away and back, so it is only counted.
    // A step back of more than a second is a clock resync and is taken, as the server does.
    if (aTick)
    {
        constexpr uint64_t cReorderWindowMs = 1000;
        if (aTick <= watch.LastTick && watch.LastTick - aTick < cReorderWindowMs)
        {
            ++watch.Late;
            return;
        }
        watch.LastTick = aTick;
    }

    const PoseReading& cReading = acReading;
    ++watch.Poses;
    if (apRoot && watch.pLastRoot && apRoot != watch.pLastRoot)
        ++watch.RootChanges;
    if (apRoot)
        watch.pLastRoot = apRoot;

    if (watch.HasLast)
    {
        float hips = 0.f;
        float bone = 0.f;
        uint32_t boneIndex = 0;
        StepBetween(watch.Last, cReading, hips, bone, boneIndex);
        if (hips > cStepHips || bone > cStepBone)
        {
            ++watch.Steps;
            if (hips > watch.WorstHips)
                watch.WorstHips = hips;
            if (bone > watch.WorstBone)
            {
                watch.WorstBone = bone;
                watch.WorstBoneIndex = boneIndex;
            }
            if (!watch.Watching)
            {
                watch.Watching = true;
                watch.Before = watch.Last;
                watch.SinceStep = 0;
            }
        }
        else if (watch.Watching)
        {
            ++watch.SinceStep;
            StepBetween(watch.Before, cReading, hips, bone, boneIndex);
            if (hips < cBackHips && bone < cBackBone)
            {
                ++watch.Returns;
                watch.Watching = false;
            }
            else if (watch.SinceStep >= cBackWithin)
                watch.Watching = false;
        }
    }
    watch.Last = cReading;
    watch.HasLast = true;

    if (cNow < watch.NextLog)
        return;
    watch.NextLog = cNow + aPeriod;
    if (watch.Steps || watch.RootChanges)
        spdlog::info("PoseFlipDiag: {} {:X}: {} poses in {} s, {} steps of over {:.0f} units at the hips or {:.0f} degrees at a bone, {} came back within {} poses to where they were "
                     "before; largest hips step {:.0f} units, largest bone step {:.0f} degrees ({}); body read changed {} times; {} came late or twice and were left out",
                     acpWho, aKey, watch.Poses, aPeriod.count(), watch.Steps, cStepHips, cStepBone, watch.Returns, cBackWithin, watch.WorstHips, watch.WorstBone,
                     kUpperBoneNames[watch.WorstBoneIndex], watch.RootChanges, watch.Late);
    watch.Poses = 0;
    watch.Steps = 0;
    watch.Returns = 0;
    watch.RootChanges = 0;
    watch.Late = 0;
    watch.WorstHips = 0.f;
    watch.WorstBone = 0.f;
    watch.WorstBoneIndex = 0;
}
} // namespace

bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose, float& aOutHeading) noexcept
{
    aOutPose = VRPose{};
    if (apPlayer)
        aOutHeading = apPlayer->rotation.z;

    if (!apPlayer)
        return false;

    void* pRoot = apPlayer->GetNiNode();
    if (!pRoot)
        return false;

    // The bones are looked up once per 3D and cached, because the search walks the whole skeleton and can take 50 ms.
    // The cache is dropped when the root changes, or when a cached node's vtable or name changes (freed node memory is reused).
    // It is also refreshed every ten minutes as a safety net.
    // Two roots are cached because a VR player has two bodies, and the game shows the other one while a menu is open.
    struct LocalCache
    {
        void* pRoot = nullptr;
        BoneNodes Nodes{};
        std::array<void*, VRPose::kBoneCount> VTables{};
        std::array<const char*, VRPose::kBoneCount> Names{};
        bool LegsFound = false;
        std::array<FingerEntries, 2> Fingers{};
        std::array<bool, 2> FingersFound{};
        void* pSkeletonRoot = nullptr;
        std::chrono::steady_clock::time_point RefreshAt{};
        std::chrono::steady_clock::time_point UsedAt{};
    };
    static std::array<LocalCache, 2> s_caches;
    const auto now = std::chrono::steady_clock::now();
    LocalCache* pCache = nullptr;
    for (LocalCache& cache : s_caches)
        if (cache.pRoot == pRoot)
            pCache = &cache;
    const bool cKnownRoot = pCache != nullptr;
    if (!pCache)
    {
        // An empty slot first, then the one used longest ago.
        // A root whose search fails keeps its slot empty and retries every frame, so picking by age alone would evict the good slot.
        if (!s_caches[0].pRoot)
            pCache = &s_caches[0];
        else if (!s_caches[1].pRoot)
            pCache = &s_caches[1];
        else
            pCache = s_caches[0].UsedAt <= s_caches[1].UsedAt ? &s_caches[0] : &s_caches[1];
    }
    LocalCache& s_local = *pCache;
    s_local.UsedAt = now;

    // Searched again for a new root, every ten minutes, or when a bone node changed kind or name.
    bool cached = cKnownRoot && now < s_local.RefreshAt;
    for (uint32_t i = 0; cached && i < VRPose::kBoneCount; ++i)
        if (s_local.Nodes[i] && (*static_cast<void**>(s_local.Nodes[i]) != s_local.VTables[i] || GetName(s_local.Nodes[i]) != s_local.Names[i]))
            cached = false;
    if (!cached)
    {
        BoneNodes nodes;
        bool legsFound = false;
        if (!FindBones(pRoot, nodes, &legsFound))
        {
            s_local.pRoot = nullptr;
            return false;
        }
        s_local.pRoot = pRoot;
        s_local.Nodes = nodes;
        s_local.LegsFound = legsFound;
        s_local.pSkeletonRoot = FindSkeletonRoot(pRoot);
        const BoneArrayInfo arrayInfo = ResolveBoneArray(pRoot, nodes, false);
        const uint8_t* pLeftHand = FindBoneEntry(arrayInfo.pArray, arrayInfo.Count, nodes[VRPose::kLeftHand]);
        const uint8_t* pRightHand = FindBoneEntry(arrayInfo.pArray, arrayInfo.Count, nodes[VRPose::kRightHand]);
        s_local.FingersFound[kLeftSide] = FindFingers(arrayInfo, pLeftHand, s_local.Fingers[kLeftSide]);
        s_local.FingersFound[kRightSide] = FindFingers(arrayInfo, pRightHand, s_local.Fingers[kRightSide]);
        static bool s_fingersNoted = false;
        if (!s_fingersNoted && !(s_local.FingersFound[kLeftSide] && s_local.FingersFound[kRightSide]))
        {
            s_fingersNoted = true;
            spdlog::debug(
                "VRBodySync: local finger bones not found (left {}, right {}); the fingers are not sent", s_local.FingersFound[kLeftSide], s_local.FingersFound[kRightSide]);
        }
        for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
        {
            s_local.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
            s_local.Names[i] = nodes[i] ? GetName(nodes[i]) : nullptr;
        }
        s_local.RefreshAt = now + std::chrono::minutes(10);
    }
    const BoneNodes& nodes = s_local.Nodes;
    const bool legsFound = s_local.LegsFound;
    bool cLegs = legsFound && FullBodyTrackingActive();
    const bool supplement = legsFound && !cLegs && GameExtension::WantsFallbackPose();
    cLegs = cLegs || supplement;

    // The pose is captured in the body's frame: the 3D root turned to where the VRIK body faces, and that facing is the heading sent.
    // The game turns the actor with the headset, so sending its heading made a head turn alone swing the copy's hips and legs.
    const glm::mat3 cRootRotation = ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate);
    if (!BodyPoseValidation::IsRotation(cRootRotation))
        return false;
    std::array<glm::mat3, VRPose::kBoneCount> boneRotations{};
    for (uint32_t i = 0; i < (legsFound ? VRPose::kBoneCount : VRPose::kUpperBoneCount); ++i)
    {
        boneRotations[i] = ToGlm(At<NiTransform>(nodes[i], kWorldOffset).rotate);
        if (!BodyPoseValidation::IsRotation(boneRotations[i]))
        {
            if (i < VRPose::kUpperBoneCount)
                return false;
            cLegs = false;
            break;
        }
    }
    glm::mat3 frame = cRootRotation;
    float bodyHeading = 0.f;
    if (legsFound && BodyFacing(nodes[VRPose::kLeftThigh], nodes[VRPose::kRightThigh], bodyHeading))
    {
        const glm::vec3 cRootForward = cRootRotation * glm::vec3{0.f, 1.f, 0.f};
        float turn = std::fmod(bodyHeading - std::atan2(cRootForward.x, cRootForward.y), glm::two_pi<float>());
        if (turn > glm::pi<float>())
            turn -= glm::two_pi<float>();
        else if (turn < -glm::pi<float>())
            turn += glm::two_pi<float>();
        if (std::isfinite(turn))
        {
            // A turn about +z lowers the heading, so the root is turned by minus the gap.
            frame = glm::mat3(glm::rotate(glm::mat4(1.f), -turn, glm::vec3{0.f, 0.f, 1.f})) * cRootRotation;
            aOutHeading = apPlayer->rotation.z + turn;
        }
    }
    s_sentHeading = aOutHeading;
    s_hasSentHeading = true;
    const glm::mat3 inverseRoot = glm::transpose(frame);

    const uint32_t boneCount = cLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        glm::mat3 boneWorld = boneRotations[i];

        // The head is turned to follow the headset, because VRIK barely turns or nods the body's head bone.
        // Yaw follows the headset, at most 80 degrees either way.
        // The head bone points about 13 degrees below the face, so its pitch is the headset's less 13, between 70 down and 60 up.
        if (i == VRPose::kHead)
        {
            if (const NiTransform* pHeadset = VRControllers::HeadsetTransform())
            {
                const auto yawOf = [](const glm::mat3& acRotate)
                {
                    const glm::vec3 forward = acRotate * glm::vec3{0.f, 1.f, 0.f};
                    return std::atan2(forward.x, forward.y);
                };
                float delta = yawOf(ToGlm(pHeadset->rotate)) - yawOf(boneWorld);
                while (delta > glm::pi<float>())
                    delta -= glm::two_pi<float>();
                while (delta < -glm::pi<float>())
                    delta += glm::two_pi<float>();
                delta = std::clamp(delta, glm::radians(-80.f), glm::radians(80.f));
                // atan2(x, y) grows clockwise seen from above; a turn about +z goes the other way.
                if (std::isfinite(delta))
                    boneWorld = glm::mat3(glm::rotate(glm::mat4(1.f), -delta, glm::vec3{0.f, 0.f, 1.f})) * boneWorld;

                const auto pitchOf = [](const glm::mat3& acRotate)
                {
                    const glm::vec3 forward = acRotate * glm::vec3{0.f, 1.f, 0.f};
                    return std::asin(std::clamp(forward.z, -1.f, 1.f));
                };
                const float wanted = std::clamp(pitchOf(ToGlm(pHeadset->rotate)) - glm::radians(13.f), glm::radians(-70.f), glm::radians(60.f));
                const float lift = wanted - pitchOf(boneWorld);
                // Up is a turn about the bone's level right axis (forward x up): it takes forward towards up.
                const glm::vec3 forward = boneWorld * glm::vec3{0.f, 1.f, 0.f};
                const glm::vec3 right = glm::cross(forward, glm::vec3{0.f, 0.f, 1.f});
                if (std::isfinite(lift) && glm::length(right) > 0.1f)
                    boneWorld = glm::mat3(glm::rotate(glm::mat4(1.f), lift, glm::normalize(right))) * boneWorld;
            }
        }

        const glm::quat rotation = glm::quat_cast(inverseRoot * boneWorld);
        if (i >= VRPose::kUpperBoneCount)
        {
            const float lengthSquared = glm::dot(rotation, rotation);
            if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(lengthSquared) ||
                lengthSquared < 1e-8f)
            {
                cLegs = false;
                break;
            }
        }
        aOutPose.Bones[i] = glm::normalize(rotation);
    }

    if (supplement && cLegs)
    {
        uint32_t bones[VRPose::kBoneCount - VRPose::kUpperBoneCount]{};
        for (size_t i = 0; i < std::size(bones); ++i)
            bones[i] = aOutPose.Bones[VRPose::kUpperBoneCount + i].Pack();
        GameExtension::CaptureFallbackPose(bones);
        cLegs = false;
    }
    aOutPose.HasLegs = cLegs;

    // The owner's hands relative to the root, sent with every pose.
    // The receiver puts its copy's hands there (ReachHands) and measures against them.
    aOutPose.HasHandCheck = false;
    if (nodes[VRPose::kLeftHand] && nodes[VRPose::kRightHand])
    {
        const glm::vec3 rootAt = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
        const glm::vec3 left = inverseRoot * (ToGlm(At<NiTransform>(nodes[VRPose::kLeftHand], kWorldOffset).translate) - rootAt);
        const glm::vec3 right = inverseRoot * (ToGlm(At<NiTransform>(nodes[VRPose::kRightHand], kWorldOffset).translate) - rootAt);
        if (std::isfinite(left.z) && std::isfinite(right.z) && glm::dot(left, left) < 512.f * 512.f && glm::dot(right, right) < 512.f * 512.f)
        {
            aOutPose.HasHandCheck = true;
            aOutPose.LeftHandOffset[0] = left.x;
            aOutPose.LeftHandOffset[1] = left.y;
            aOutPose.LeftHandOffset[2] = left.z;
            aOutPose.RightHandOffset[0] = right.x;
            aOutPose.RightHandOffset[1] = right.y;
            aOutPose.RightHandOffset[2] = right.z;
        }
    }

    FillSpellAim(aOutPose, now);

    // Hips: where the pelvis sits relative to the 3D root, in root space.
    // Crouching, leaning and hip sway move it without changing any rotation.
    // Sent always: VRIK stands the body some way back from the headset, and the receiver uses the horizontal part to match.
    // See ApplyRemotePoses; the height is applied only while the legs are tracked.
    aOutPose.HasHips = false;
    if (nodes[VRPose::kPelvis])
    {
        const NiTransform& rootWorld = At<NiTransform>(pRoot, kWorldOffset);
        const glm::vec3 pelvis = ToGlm(At<NiTransform>(nodes[VRPose::kPelvis], kWorldOffset).translate);
        const glm::vec3 offset = inverseRoot * (pelvis - ToGlm(rootWorld.translate));
        // A body is a couple of hundred units tall, so anything farther is a bone read mid-update.
        // Sending it would throw the copy across the room.
        if (IsFiniteVector(offset) && glm::dot(offset, offset) < 512.f * 512.f)
        {
            aOutPose.HasHips = true;
            aOutPose.HipOffset[0] = offset.x;
            aOutPose.HipOffset[1] = offset.y;
            aOutPose.HipOffset[2] = offset.z;
        }
    }

    CaptureWeapons(apPlayer, pRoot, nodes, aOutPose, now);
    CaptureArrowHold(apPlayer, nodes, now);

    // Fingers: sent only when both hands' bones are known, and only when they changed or kKeptFieldsResend has passed.
    aOutPose.HasFingers = false;
    if (s_local.FingersFound[kLeftSide] && s_local.FingersFound[kRightSide])
    {
        static std::array<Quaternion_NetQuantize, VRPose::kFingerBoneCount> s_lastSent{};
        static std::chrono::steady_clock::time_point s_lastSentAt{};
        constexpr size_t cPerHand = VRPose::kFingersPerHand * VRPose::kBonesPerFinger;
        std::array<glm::quat, VRPose::kFingerBoneCount> rotations{};
        ReadFingerRotations(s_local.Fingers[kLeftSide], ToGlm(At<NiTransform>(nodes[VRPose::kLeftHand], kWorldOffset).rotate), rotations.data());
        ReadFingerRotations(s_local.Fingers[kRightSide], ToGlm(At<NiTransform>(nodes[VRPose::kRightHand], kWorldOffset).rotate), rotations.data() + cPerHand);
        std::array<Quaternion_NetQuantize, VRPose::kFingerBoneCount> quantized{};
        for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
            quantized[i] = rotations[i];
        if (quantized != s_lastSent || now - s_lastSentAt >= kKeptFieldsResend)
        {
            s_lastSent = quantized;
            s_lastSentAt = now;
            aOutPose.HasFingers = true;
            aOutPose.Fingers = quantized;
        }
    }

    // Body size: the skeleton root's world scale, sent when it changes or every kKeptFieldsResend.
    // Logged whenever it changes, to show what scale VRIK gave this body.
    aOutPose.HasScale = false;
    if (s_local.pSkeletonRoot)
    {
        static uint16_t s_lastScaleSent = 0;
        static std::chrono::steady_clock::time_point s_lastScaleSentAt{};
        const float worldScale = At<NiTransform>(s_local.pSkeletonRoot, kWorldOffset).scale;
        const uint16_t quantized = static_cast<uint16_t>(glm::clamp(worldScale * 1000.f, 50.f, 20000.f) + 0.5f);
        if (quantized != s_lastScaleSent || now - s_lastScaleSentAt >= kKeptFieldsResend)
        {
            if (quantized != s_lastScaleSent)
                spdlog::info("VRBodySync: local body scale {:.3f} (skeleton root world scale, actor scale field {})", worldScale, apPlayer->scale);
            s_lastScaleSent = quantized;
            s_lastScaleSentAt = now;
            aOutPose.HasScale = true;
            aOutPose.RootScale = quantized;
        }
    }

    aOutPose.HasData = true;
    // TEMPORARY, see WatchPoseSteps.
    WatchPoseSteps(apPlayer->formID, 0, ReadingOf(aOutPose), pRoot, "local player", std::chrono::seconds(30));
    s_lastLocalRoot = pRoot;
    s_lastLocalPose = aOutPose;
    s_lastLocalPose.HasWeapons = false;
    s_lastLocalPose.HasFingers = false;
    s_lastLocalPose.HasScale = false;
    s_lastLocalPose.HasSpellAim = false;
    return true;
}

// Watches a dead body, or a living NPC lying in its ragdoll, this client does not own for sustained motion near the player.
// Only the owner sends a dragged corpse or a carried NPC (see AnimationSystem), so a body grabbed here has to be asked for first.
// Returns true once the body has moved for a third of a second with one of this player's hands on it.
// A settling ragdoll rests within a second or two while a dragged body keeps moving, so sustained motion is the test.
bool IsRemoteBodyGrabbedHere(Actor* apActor, const glm::vec3& acOwnerPosition) noexcept
{
    // Its live 3D is not read while a load is up: the client's update can run beside the main thread then, which may be freeing it.
    if (!apActor || LoadingScreenUp())
        return false;
    const ActorExtension* pExtension = apActor->GetExtension();
    if (!AsksForGrabbed(
            apActor->actorState.IsDead(), apActor->actorState.IsInKnockRagdoll(), apActor->actorState.IsBleedingOut(),
            !pExtension || pExtension->IsRemotePlayer() || apActor->IsPlayerSummon()))
        return false;

    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    const glm::vec3 at = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
    const glm::vec3 playerAt = ToGlm(static_cast<NiPoint3>(pPlayer->position));
    const float toPlayer = glm::distance(at, playerAt);
    // Farther than this from the player, nobody here can be touching it by hand.
    if (toPlayer > 400.f)
        return false;

    struct Watch
    {
        glm::vec3 Last{};
        bool Placed = false;
        std::chrono::steady_clock::time_point MovingSince{};
        std::chrono::steady_clock::time_point LastMoved{};
        std::chrono::steady_clock::time_point TouchedAt{};
        bool Reported = false; // this grab has been reported to the caller
        glm::vec3 OwnerLast{};
        std::chrono::steady_clock::time_point SampledAt{};
        std::chrono::steady_clock::time_point OwnerMovedAt{};
        // A living NPC: whether this player's hands carry it, and where they were at the sample before.
        HandHold Hold{};
        std::array<glm::vec3, 2> Hands{};
        std::array<bool, 2> HasHands{};
    };
    static std::unordered_map<uint32_t, Watch> s_watch;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = s_watch.begin(); it != s_watch.end();)
        it = now - it->second.TouchedAt > std::chrono::seconds(30) ? s_watch.erase(it) : std::next(it);

    Watch& watch = s_watch[apActor->formID];
    watch.TouchedAt = now;

    if (!watch.Placed)
    {
        watch.Last = at;
        watch.OwnerLast = acOwnerPosition;
        watch.Placed = true;
        return false;
    }

    // Sampled every 100 ms, as the owner samples its own bodies (CaptureBodyPose, at the snapshot rate).
    // Per frame, the 4-unit step below would need 240 units a second at 60 fps, which a hand dragging a body never reaches.
    if (now - watch.SampledAt < std::chrono::milliseconds(100))
        return false;
    watch.SampledAt = now;

    const glm::vec3 cBodyStep = at - watch.Last;
    const float step = glm::length(cBodyStep);
    watch.Last = at;
    std::array<glm::vec3, 2> handSteps{};
    for (uint8_t side = 0; side < 2; ++side)
    {
        glm::vec3 hand{};
        const bool cHas = LocalHandPosition(side, hand);
        if (cHas && watch.HasHands[side])
            handSteps[side] = hand - watch.Hands[side];
        watch.Hands[side] = hand;
        watch.HasHands[side] = cHas;
    }

    // Moved by its owner: the position or bones in its updates moved it, so the other side is carrying it, not a hand here.
    // Without this, a corpse the owner moves would be asked for here and taken back by the owner, again and again.
    if (glm::distance(acOwnerPosition, watch.OwnerLast) >= 4.f)
        watch.OwnerMovedAt = now;
    watch.OwnerLast = acOwnerPosition;
    // Our own placing of it (PlaceCorpse) counts too: it settles from there, and no hand here moved it.
    bool placedLately = false;
    {
        std::scoped_lock lock(s_placedCorpsesLock);
        const auto placed = s_placedCorpses.find(apActor->formID);
        placedLately = placed != s_placedCorpses.end() && now - placed->second.MovedAt < std::chrono::milliseconds(1500);
    }
    const auto cPosedAt = BodyPosedAt(apActor->formID);
    const bool cOwnerMovesIt = now - watch.OwnerMovedAt < std::chrono::milliseconds(1500) || (cPosedAt && now - *cPosedAt < std::chrono::milliseconds(1500)) || placedLately;
    if (cOwnerMovesIt)
    {
        watch.LastMoved = {};
        watch.Reported = false;
        watch.Hold = {};
        return false;
    }

    // The same 4 units CaptureBodyPose uses: a settled ragdoll still twitches.
    if (step >= 4.f)
    {
        if (now - watch.LastMoved > std::chrono::milliseconds(500))
            watch.MovingSince = now;
        watch.LastMoved = now;
    }
    else if (now - watch.LastMoved > std::chrono::milliseconds(500))
    {
        watch.Reported = false;
        watch.Hold = {};
        return false;
    }

    const auto movingFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - watch.MovingSince).count();

    // A living NPC moves when a hand hits or pushes it too: it counts as carried only while a hand stays on it and moves with it.
    const bool cAlive = !apActor->actorState.IsDead();
    const bool cHandNear = cAlive && step >= HandMotion::kMinStep && NearestHandToBody(pRoot) <= HandHold::kReach;
    const bool cHeldAlive = cAlive && watch.Hold.Sample(std::chrono::duration<double, std::milli>(now.time_since_epoch()).count(), cHandNear, handSteps, cBodyStep);

    // After a third of a second of motion with one of this player's hands on it, the caller asks for the body.
    // This side then becomes the one that sends it (see CharacterService::RunRemoteUpdates).
    // The hand is required because a body also moves by itself, falling or settling after its owner lets go.
    bool cGrabbed = false;
    if (cAlive)
    {
        cGrabbed = movingFor >= 300 && !watch.Reported && cHeldAlive;
        watch.Reported = watch.Reported || cGrabbed;
    }
    else if (movingFor >= 300 && !watch.Reported)
    {
        watch.Reported = true;
        const float cHand = NearestHandToBody(pRoot);
        cGrabbed = cHand <= 40.f;
        if (!cGrabbed)
            spdlog::info("Body {:X} is moving here, but no hand of this player's is on it (nearest {:.0f} units); left to its owner", apActor->formID,
                         std::min(cHand, 99999.f));
    }

    return cGrabbed;
}

void NoteDeathHere(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_diedHereLock);
    // Old ones, of bodies never read since (too far from the player), are dropped.
    if (s_diedHere.size() > 256)
        s_diedHere.clear();
    s_diedHere[aFormId] = std::chrono::steady_clock::now();
}

bool CaptureBodyPose(Actor* apActor, VRPose& aOutPose) noexcept
{
    aOutPose.HasData = false;

    // Its live 3D is not read while a load is up: the client's update can run beside the main thread then, which may be freeing it.
    if (!apActor || LoadingScreenUp())
        return false;

    void* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return false;

    if (GameExtension::CapturesRagdoll(apActor->formID, pRoot))
        return false;

    // Bones are cached per body, as for the local player, so the skeleton is not searched at every send.
    // Entries for bodies not seen for 30 seconds are dropped.
    struct BodyCapture
    {
        void* pRoot = nullptr;
        BoneNodes Nodes{};
        std::array<void*, VRPose::kBoneCount> VTables{};
        std::chrono::steady_clock::time_point RefreshAt{};
        std::array<Quaternion_NetQuantize, VRPose::kUpperBoneCount> LastSent{};
        std::chrono::steady_clock::time_point LastChangeAt{};
        std::chrono::steady_clock::time_point TouchedAt{};
        glm::vec3 LastPosition{};
        std::chrono::steady_clock::time_point MovedAt{};
        bool Placed = false;
        bool Sent = false;
        bool Boneless = false; // a skeleton this bone search cannot read; the root position is sent on its own
        bool LegsFound = false;
        // A dead body's bones as of the last time one of them moved, legs included, and when (CopyBodyRules::KeepsSendingBones).
        std::array<glm::quat, VRPose::kBoneCount> RestReference{};
        bool HasRestReference = false;
        std::chrono::steady_clock::time_point BonesMovedAt{};
        std::vector<RagdollBodiesPose::Body> RestBodies;
        std::chrono::steady_clock::time_point BodiesReadAt{};
        uint32_t BodyLayout{};
    };
    static std::unordered_map<uint32_t, BodyCapture> s_bodies;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = s_bodies.begin(); it != s_bodies.end();)
        it = now - it->second.TouchedAt > std::chrono::seconds(30) ? s_bodies.erase(it) : std::next(it);

    BodyCapture& capture = s_bodies[apActor->formID];
    if (capture.pRoot != pRoot)
        capture = {};
    capture.TouchedAt = now;

    bool cached = capture.pRoot == pRoot && now < capture.RefreshAt;
    for (uint32_t i = 0; cached && i < VRPose::kUpperBoneCount; ++i)
        if (capture.Nodes[i] && *static_cast<void**>(capture.Nodes[i]) != capture.VTables[i])
            cached = false;

    if (!cached)
    {
        BoneNodes nodes;
        bool legsFound = false;
        if (!FindBones(pRoot, nodes, &legsFound))
        {
            // Not a person: the bone search uses human bone names, so a Dwarven sphere, a spider or a centurion fails it.
            // Its position can still be sent, just not how it is bent (see VRPose::NoBones).
            capture.pRoot = pRoot;
            capture.Nodes = BoneNodes{};
            capture.VTables = {};
            capture.Boneless = true;
            capture.RefreshAt = now + std::chrono::seconds(5);
            capture.Sent = false;
        }
        else
        {
            capture.pRoot = pRoot;
            capture.Nodes = nodes;
            capture.Boneless = false;
            capture.LegsFound = legsFound;
            for (uint32_t i = 0; i < VRPose::kBoneCount; ++i)
                capture.VTables[i] = nodes[i] ? *static_cast<void**>(nodes[i]) : nullptr;
            capture.RefreshAt = now + std::chrono::seconds(5);
            capture.Sent = false;
        }
        // Bones found again may be other nodes: they are compared from here on, not against the old ones.
        capture.HasRestReference = false;
    }

    // Being moved is detected from the 3D root's displacement, not from the bones.
    // A settling corpse or an actor still playing an animation changes its bones without anyone touching it.
    const glm::vec3 rootPosition = ToGlm(At<NiTransform>(pRoot, kWorldOffset).translate);
    if (capture.Placed)
    {
        constexpr float cMovedSquared = 4.f * 4.f; // a settled ragdoll still twitches a little
        const glm::vec3 delta = rootPosition - capture.LastPosition;
        if (glm::dot(delta, delta) >= cMovedSquared)
            capture.MovedAt = now;
    }
    capture.LastPosition = rootPosition;
    capture.Placed = true;

    std::vector<RagdollBodiesPose::Body> bodies;
    uint32_t layout{};
    if (capture.Boneless && RagdollBodiesPose::SendDue(capture.BodiesReadAt, now) && !apActor->IsStillLoading() && !LoadingScreenUp())
    {
        capture.BodiesReadAt = now;
        if (ReadOwnRagdoll(pRoot, bodies, layout))
        {
            const bool cSameBodies = capture.BodyLayout == layout && capture.RestBodies.size() == bodies.size();
            const bool cMoved = cSameBodies && RagdollBodiesPose::ArticulationChanged(capture.RestBodies, bodies);
            if (cMoved)
                capture.MovedAt = now;
            if (!cSameBodies || cMoved)
            {
                capture.RestBodies = bodies;
                capture.BodyLayout = layout;
            }
        }
    }

    // A body that came to rest while it was still dying never moved once it read dead, so it would never be sent.
    // A death this game just reported counts as moving it, once: its resting pose goes out for the 2 s below.
    {
        std::scoped_lock lock(s_diedHereLock);
        if (const auto cDied = s_diedHere.find(apActor->formID); cDied != s_diedHere.end())
        {
            if (now - cDied->second < std::chrono::seconds(10))
                capture.MovedAt = now;
            s_diedHere.erase(cDied);
        }
    }

    // A body hanging still from one of this player's hands has not come to rest: the others would drop it under that hand.
    // The hand is looked for only when the body would stop being sent, so a held body costs one search every two seconds.
    // In Skyrim SE there is no hand: the game says what the player holds with the grab key.
    const bool cStill = now - capture.MovedAt >= std::chrono::seconds(2) || (!capture.Boneless && now - capture.LastChangeAt >= std::chrono::seconds(2));
#ifdef SKYRIMVR
    const auto cHeldHere = [&]
    {
        return NearestHandToBody(pRoot) <= 40.f;
    };
#else
    const auto cHeldHere = [&]
    {
        const PlayerCharacter* pPlayer = PlayerCharacter::Get();
        return pPlayer && pPlayer->grabbedObjectHandle && TESObjectREFR::GetByHandle(pPlayer->grabbedObjectHandle) == apActor;
    };
#endif
    if (capture.Sent && cStill && cHeldHere())
    {
        capture.MovedAt = now;
        capture.LastChangeAt = now;
    }

    // A dead body's limbs often settle after its root: each of its bones, legs too, against where they were when one last moved.
    const bool cDeadHere = apActor->actorState.IsDead();
    const auto cMsSince = [now](const std::chrono::steady_clock::time_point aAt)
    {
        return static_cast<uint32_t>(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - aAt).count(), 0, UINT32_MAX));
    };
    // Not looked at once its root has kept still past the most it is sent for: a body at rest costs nothing.
    const bool cRestPose = cDeadHere && !capture.Boneless && cMsSince(capture.MovedAt) < CopyBodyRules::kCorpseRestCapMs;
    bool bonesMovedNow = false;
    if (cRestPose)
    {
        const glm::mat3 cInverseRoot = glm::transpose(ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate));
        const uint32_t cCount = capture.LegsFound ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
        std::array<glm::quat, VRPose::kBoneCount> bones{};
        float mostDegrees = 0.f;
        for (uint32_t i = 0; i < cCount; ++i)
        {
            if (!capture.Nodes[i])
                continue;
            bones[i] = glm::normalize(glm::quat_cast(cInverseRoot * ToGlm(At<NiTransform>(capture.Nodes[i], kWorldOffset).rotate)));
            const float cDot = std::abs(glm::dot(bones[i], capture.RestReference[i]));
            // A reading that is not a number counts as a move (BonesMoved).
            mostDegrees = std::isfinite(cDot) ? std::max(mostDegrees, glm::degrees(2.f * std::acos(std::min(1.f, cDot)))) : std::numeric_limits<float>::quiet_NaN();
            if (std::isnan(mostDegrees))
                break;
        }
        // The first look is only where they are: a body found lying still long after its death is not sent again for it.
        // A fresh look after the bones were found again keeps when they last moved.
        if (!capture.HasRestReference)
            capture.BonesMovedAt = std::max(capture.BonesMovedAt, capture.MovedAt);
        else if (CopyBodyRules::BonesMoved(mostDegrees))
        {
            capture.BonesMovedAt = now;
            bonesMovedNow = true;
        }
        if (!capture.HasRestReference || bonesMovedNow)
            capture.RestReference = bones;
        capture.HasRestReference = true;
    }
    else
        capture.HasRestReference = false;

    // Two seconds after it comes to rest, this body stops costing anything at all; a dead one once its bones keep still too.
    if (!CopyBodyRules::KeepsSendingBones(cDeadHere && !capture.Boneless, cMsSince(capture.MovedAt), cMsSince(capture.BonesMovedAt)))
    {
        capture.Sent = false;
        return false;
    }

    // Creature bodies use a separate pose stream.
    if (capture.Boneless)
    {
        if (!capture.Sent)
            spdlog::debug("VRBodySync: creature {:X} started moving", apActor->formID);
        capture.Sent = true;

        aOutPose.NoBones = true;
        aOutPose.HasLegs = false;
        aOutPose.HasFingers = false;
        aOutPose.HasScale = false;
        aOutPose.HasRootPosition = true;
        aOutPose.RootPosition[0] = rootPosition.x;
        aOutPose.RootPosition[1] = rootPosition.y;
        aOutPose.RootPosition[2] = rootPosition.z;
        aOutPose.HasData = true;
        if (!bodies.empty())
            if (const auto cOwner = Utils::GetLocalOwnershipToken(apActor->formID))
                RagdollPoseService::NoteOwnBodies(apActor->formID, cOwner->ServerId, cOwner->OwnershipEpoch, rootPosition, std::move(bodies), layout);
        return true;
    }

    const glm::mat3 inverseRoot = glm::transpose(ToGlm(At<NiTransform>(pRoot, kWorldOffset).rotate));

    std::array<Quaternion_NetQuantize, VRPose::kUpperBoneCount> quantized{};
    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
    {
        const glm::mat3 boneWorld = ToGlm(At<NiTransform>(capture.Nodes[i], kWorldOffset).rotate);
        quantized[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
    }

    // Sending continues for two seconds after the body comes to rest, then stops.
    // The receiver buffers only two points, so the last pose is repeated a few times to make sure it lands.
    // After that the body is left to its own ragdoll on both sides.
    // A dead body's legs or hands settling count too, though its upper body's bones read the same.
    const bool cChanged = !capture.Sent || quantized != capture.LastSent || bonesMovedNow;
    if (cChanged)
    {
        // One line each time a body starts moving again, so the log shows whether a grab was ever sent.
        if (!capture.Sent || now - capture.LastChangeAt >= std::chrono::seconds(2))
            spdlog::info("VRBodySync: body {:X} is being moved here, sending its bones to the other players", apActor->formID);
        capture.LastChangeAt = now;
    }
    else if (now - capture.LastChangeAt >= std::chrono::seconds(2))
        return false;

    capture.LastSent = quantized;
    capture.Sent = true;

    for (uint32_t i = 0; i < VRPose::kUpperBoneCount; ++i)
        aOutPose.Bones[i] = quantized[i];

    // The legs too, encoded like the upper body.
    // Without them the receiver would pose the top half and leave the legs bent however its own ragdoll had them.
    if (capture.LegsFound)
    {
        for (uint32_t i = VRPose::kUpperBoneCount; i < VRPose::kBoneCount; ++i)
        {
            const glm::mat3 boneWorld = ToGlm(At<NiTransform>(capture.Nodes[i], kWorldOffset).rotate);
            aOutPose.Bones[i] = glm::normalize(glm::quat_cast(inverseRoot * boneWorld));
        }
    }
    aOutPose.HasLegs = capture.LegsFound;
    aOutPose.HasFingers = false;
    aOutPose.HasScale = false;

    // Where the body is, not just how it is bent.
    // The receiver cannot use the reference position: a corpse moves with its ragdoll, not with its reference.
    aOutPose.HasRootPosition = true;
    aOutPose.RootPosition[0] = rootPosition.x;
    aOutPose.RootPosition[1] = rootPosition.y;
    aOutPose.RootPosition[2] = rootPosition.z;

    aOutPose.HasData = true;
    return true;
}

void NoteReceivedPose(const uint32_t aFormId, const uint64_t aTick, const VRPose& acPose) noexcept
{
    if (!acPose.HasData || acPose.NoBones)
        return;
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    const ActorExtension* pExtension = pActor ? pActor->GetExtension() : nullptr;
    if (!pExtension || !pExtension->IsRemotePlayer())
        return;
    WatchPoseSteps(aFormId, aTick, ReadingOf(acPose), nullptr, "copy", std::chrono::seconds(10));
}

static void SetRemotePoseImpl(Actor* apActor, const VRPose& acPose, const ::RemotePose::Sample* apSample, double aNowNetworkMs, ::RemotePose::Policy aPolicy) noexcept
{
    if (!apActor)
        return;

    const ::RemotePose::Provenance* pSource = nullptr;
    if (apSample)
    {
        const auto* extension = apActor->GetExtension();
        if (!extension || (aPolicy == ::RemotePose::Policy::SparseBody && (!acPose.HasData || !SparseBodyEligible(apActor))))
        {
            ClearRemotePose(apActor->formID);
            return;
        }
        // Empty/stale samples never revive a cache. The existing live entry expires by its original deadline.
        if (!acPose.HasData || !std::isfinite(aNowNetworkMs))
            return;
        for (const auto* p : {&apSample->Before, &apSample->After})
        {
            if (!::RemotePose::IsFresh(*p, apSample->Generation, aNowNetworkMs, aPolicy))
                return;
            if (!pSource || p->SourceTick > pSource->SourceTick || (p->SourceTick == pSource->SourceTick && p->ReceivedAtMs > pSource->ReceivedAtMs))
                pSource = p;
        }
        // Accepted source ticks and their receive times must advance together.
        if ((apSample->Before.SourceTick < apSample->After.SourceTick && apSample->Before.ReceivedAtMs > apSample->After.ReceivedAtMs) ||
            (apSample->After.SourceTick < apSample->Before.SourceTick && apSample->After.ReceivedAtMs > apSample->Before.ReceivedAtMs))
            return;
    }

    // TEMPORARY, see WatchPoseSteps: the updates at which a remote player's copy had no pose, so its arms followed the animation.
    // Logged every 10 s for a copy that had a pose at some updates and not at others.
    if (apActor->GetExtension()->IsRemotePlayer() && !apActor->actorState.IsDeadOrDying())
    {
        struct PoseUpdateCount
        {
            uint32_t Updates = 0;
            uint32_t Missing = 0;
            std::chrono::steady_clock::time_point NextLog{};
        };
        static std::unordered_map<uint32_t, PoseUpdateCount> s_poseUpdateCounts;
        if (s_poseUpdateCounts.size() > 256)
            s_poseUpdateCounts.clear();
        PoseUpdateCount& count = s_poseUpdateCounts[apActor->formID];
        const auto cNow = std::chrono::steady_clock::now();
        if (count.NextLog == std::chrono::steady_clock::time_point{})
            count.NextLog = cNow + std::chrono::seconds(10);
        else if (cNow >= count.NextLog)
        {
            if (count.Missing && count.Missing < count.Updates)
                spdlog::info("PoseFlipDiag: copy {:X} had no pose at {} of {} updates in 10 s, its arms followed the animation there", apActor->formID, count.Missing,
                             count.Updates);
            count = PoseUpdateCount{};
            count.NextLog = cNow + std::chrono::seconds(10);
        }
        ++count.Updates;
        if (!acPose.HasData)
            ++count.Missing;
    }

    if (!acPose.HasData)
    {
        // Without pose data the arms follow the animation, for example a sword held up in the vanilla idle.
        // Only VR players send a pose, so this is logged for remote players that had one, at most every 10 s each.
        bool hadPose = false;
        {
            std::shared_lock lock(s_posesLock);
            hadPose = s_poses.find(apActor->formID) != s_poses.end();
        }
        if (hadPose && apActor->GetExtension()->IsRemotePlayer() && !apActor->actorState.IsDeadOrDying())
        {
            static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_nextNote;
            if (s_nextNote.size() > 256)
                s_nextNote.clear();
            const auto now = std::chrono::steady_clock::now();
            auto& next = s_nextNote[apActor->formID];
            if (now >= next)
            {
                next = now + std::chrono::seconds(10);
                spdlog::warn("VRBodySync: no VR pose data for remote actor {:X}, its arms follow the animation", apActor->formID);
            }
        }
        ClearRemotePose(apActor->formID);
        return;
    }

    std::unique_lock lock(s_posesLock);
    NativeRemotePose& pose = s_poses[apActor->formID];
    if (pSource)
    {
        const auto cNow = std::chrono::steady_clock::now();
        if (pose.HasProvenance && apSample->Generation < pose.Generation)
            return;
        const bool newGeneration = !pose.HasProvenance || apSample->Generation != pose.Generation;
        if (!newGeneration &&
            (pSource->SourceTick < pose.SourceTick || pSource->ReceivedAtMs < pose.ReceivedAtMs ||
             (pSource->SourceTick == pose.SourceTick && pSource->ReceivedAtMs != pose.ReceivedAtMs) || pose.Live != (aPolicy == ::RemotePose::Policy::LiveVRPlayer)))
            return;
        const bool newSource = newGeneration || pSource->SourceTick > pose.SourceTick;
        if (!newSource && !pose.Available(cNow))
            return;
        if (newGeneration || (newSource && !pose.Available(cNow)))
            pose = NativeRemotePose{}; // Never carry sparse fields across identities, epochs, or an expired live gap.
        if (newSource)
        {
            pose.Generation = apSample->Generation;
            pose.SourceTick = pSource->SourceTick;
            pose.ReceivedAtMs = pSource->ReceivedAtMs;
            pose.HasProvenance = true;
            pose.Live = aPolicy == ::RemotePose::Policy::LiveVRPlayer;
            if (pose.Live)
            {
                const double remaining = ::RemotePose::kLiveTimeoutMs - (aNowNetworkMs - pSource->ReceivedAtMs);
                pose.Deadline = cNow + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(remaining));
            }
        }
    }
    pose.NoBones = acPose.NoBones;
    pose.HasLegs = acPose.NoBones ? false : acPose.HasLegs;
    if (!acPose.NoBones)
        for (uint32_t i = 0; i < (acPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount); ++i)
            pose.Bones[i] = acPose.Bones[i];
    if (acPose.HasFingers)
    {
        pose.HasFingers = true;
        for (size_t i = 0; i < VRPose::kFingerBoneCount; ++i)
            pose.Fingers[i] = acPose.Fingers[i];
    }
    if (acPose.HasScale)
    {
        const float scale = acPose.RootScale / 1000.f;
        if (!pose.HasScale || std::fabs(pose.RootScale - scale) > 0.0005f)
            spdlog::info("VRBodySync: body scale of remote actor {:X} is {:.3f}", apActor->formID, scale);
        pose.HasScale = true;
        pose.RootScale = scale;
    }

    // Not sticky: the sender fills this only while a dead body is being moved.
    // When it stops, the body settles on its own ragdoll instead of staying where it was last dragged.
    pose.HasRootPosition = acPose.HasRootPosition;
    if (acPose.HasRootPosition)
        pose.RootPosition = glm::vec3{acPose.RootPosition[0], acPose.RootPosition[1], acPose.RootPosition[2]};

    pose.HasHips = acPose.HasHips;
    if (acPose.HasHips)
        pose.HipOffset = glm::vec3{acPose.HipOffset[0], acPose.HipOffset[1], acPose.HipOffset[2]};

    pose.HasHandCheck = acPose.HasHandCheck;
    if (acPose.HasHandCheck)
    {
        pose.HandOffsets[0] = glm::vec3{acPose.LeftHandOffset[0], acPose.LeftHandOffset[1], acPose.LeftHandOffset[2]};
        pose.HandOffsets[1] = glm::vec3{acPose.RightHandOffset[0], acPose.RightHandOffset[1], acPose.RightHandOffset[2]};
    }

    if (acPose.HasWeapons)
    {
        pose.HasWeapons = true;
        for (size_t side = 0; side < 2; ++side)
        {
            pose.WeaponHeld[side] = acPose.WeaponHeld[side];
            pose.WeaponRotation[side] = acPose.WeaponRotation[side];
            pose.WeaponOffset[side] = glm::vec3{acPose.WeaponOffset[side][0], acPose.WeaponOffset[side][1], acPose.WeaponOffset[side][2]};
        }
    }

    pose.HasSpellAim = acPose.HasSpellAim;
    if (acPose.HasSpellAim)
    {
        pose.SpellAim[0] = acPose.SpellAim[0];
        pose.SpellAim[1] = acPose.SpellAim[1];
    }
}

void SetRemotePoseSample(Actor* apActor, const ::RemotePose::Sample& acSample, double aNowNetworkMs, ::RemotePose::Policy aPolicy) noexcept
{
    SetRemotePoseImpl(apActor, acSample.Pose, &acSample, aNowNetworkMs, aPolicy);
}

void ClearRemotePose(uint32_t aFormId) noexcept
{
    {
        std::unique_lock lock(s_posesLock);
        s_poses.erase(aFormId);
#ifndef SKYRIMVR
        s_spellAims.erase(aFormId);
#endif
    }
    // A new copy under the same id has no arrow yet: the next one nocked must be attached.
    std::scoped_lock lock(s_nockedLock);
    s_nocked.erase(aFormId);
}
void ClearRemotePoseForGeneration(uint32_t aFormId, uint64_t aGeneration) noexcept
{
    std::unique_lock lock(s_posesLock);
    const auto it = s_poses.find(aFormId);
    if (it != s_poses.end() && it->second.HasProvenance && it->second.Generation == aGeneration)
        s_poses.erase(it);
}
uint64_t SkeletonMotionFingerprint(Actor* apActor) noexcept
{
    void* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
        return 0;

    // The same walk the pose code uses; every NiAVObject below the root carries a local transform at kLocalOffset.
    std::vector<void*> nodes;
    CollectNodeDescendants(pRoot, nodes);

    uint64_t hash = 1469598103934665603ull;
    size_t hashed = 0;
    for (void* pNode : nodes)
    {
        const auto* pBytes = reinterpret_cast<const uint8_t*>(&At<NiTransform>(pNode, kLocalOffset));
        for (size_t i = 0; i < sizeof(NiTransform); ++i)
        {
            hash ^= pBytes[i];
            hash *= 1099511628211ull;
        }
        if (++hashed >= 64)
            break;
    }
    return hash;
}

} // namespace VRBodySync
