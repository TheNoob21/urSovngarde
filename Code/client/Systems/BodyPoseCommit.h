#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <TiltedCore/Buffer.hpp>
#include "BodyPosePresentation.h"
#include "BodyPoseValidation.h"
#include "RemotePoseState.h"

// SE post-root presentation uses captured values and addresses without accessing engine memory.
namespace BodyPoseCommit
{
// SE 1.7.104 flattened bone tree, from the installed image: EF4F80 loads tree+130 and entry+70,
// EF4F95 reads the int16 parent at entry+68 (negative: the tree's own world at tree+7C),
// EF4FBB composes flat-only entries into entry+34, EF4FF1 copies a linked node's +7C into entry+34,
// EF5030 advances by 80h. Node parent is node+30, node world node+7C (NiAVObject).
inline constexpr size_t kEntryStride = 0x80, kEntryWorld = 0x34, kEntryParent = 0x68, kEntryNode = 0x70;
inline constexpr size_t kNodeParent = 0x30, kNodeWorld = 0x7C, kTreeCount = 0x128, kTreeEntries = 0x130;
inline constexpr size_t kTransformBytes = 52;
static_assert(kEntryWorld + kTransformBytes == kEntryParent, "flat world ends where the parent index starts");
static_assert(kEntryParent + sizeof(int16_t) <= kEntryNode && kEntryNode + sizeof(uintptr_t) <= kEntryStride);

using Transform = std::array<float, 13>; // native row-major 3x3, translation, uniform scale
static_assert(sizeof(Transform) == kTransformBytes);

inline bool Finite(const Transform& acTransform) noexcept
{
    for (const float v : acTransform)
        if (!std::isfinite(v))
            return false;
    return true;
}
inline bool SameBits(const Transform& acLeft, const Transform& acRight) noexcept
{
    return std::memcmp(acLeft.data(), acRight.data(), sizeof(Transform)) == 0;
}

// A writable presentation target: every float finite, a proper rotation, a positive scale that is
// bit-identical to the expected scale, and a bounded translation change. The expected scale is the native baseline's scale
// (scale not posed: ValidTarget, the rule) or, under the root-scale pass, native scale x the pass's ratio
// computed by the caller as one float multiply (so the bits are reproducible).
inline bool ValidTargetScale(const Transform& acTarget, const Transform& acBaseline, float aExpectedScale, float aMaxTravel) noexcept
{
    if (!Finite(acTarget) || !Finite(acBaseline) || !(acTarget[12] > 0.f) || std::memcmp(&acTarget[12], &aExpectedScale, sizeof(float)) != 0 || !std::isfinite(aMaxTravel) ||
        aMaxTravel < 0.f)
        return false;
    float m[3][3]{}; // m[column][row], as BodyPoseValidation expects
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            m[column][row] = acTarget[row * 3 + column];
    if (!BodyPoseValidation::IsRotation(m))
        return false;
    const double dx = static_cast<double>(acTarget[9]) - acBaseline[9], dy = static_cast<double>(acTarget[10]) - acBaseline[10],
                 dz = static_cast<double>(acTarget[11]) - acBaseline[11];
    const double travel = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(travel) && travel <= aMaxTravel;
}
inline bool ValidTarget(const Transform& acTarget, const Transform& acBaseline, float aMaxTravel) noexcept
{
    return ValidTargetScale(acTarget, acBaseline, acBaseline[12], aMaxTravel);
}

// The actor's own 3D root keeps its native world: an entry linked to it gets no hip delta and is never written.
template <size_t N> uint32_t MarkRootExcluded(const std::array<uintptr_t, N>& acNodes, size_t aCount, uintptr_t aRoot, std::array<bool, N>& aExcluded) noexcept
{
    uint32_t marked = 0;
    for (size_t i = 0; i < N; ++i)
    {
        aExcluded[i] = i < aCount && aRoot && acNodes[i] == aRoot;
        marked += aExcluded[i] ? 1u : 0u;
    }
    return marked;
}

struct Write
{
    uintptr_t Address{};
    uint16_t Entry{};
    bool NodeWorld{}; // true: linked node+7C; false: flat entry+34
};
// One write per flat-only entry (entry+34); a linked entry writes node+7C AND its flat copy entry+34,
// exactly the pair EF4FF1 keeps equal. Unselected entries produce nothing. Returns false on overflow
// (then nothing in out is meaningful) or on an address that would wrap.
template <size_t N, size_t M>
bool SelectWrites(
    uintptr_t aEntries, const std::array<uintptr_t, N>& acNodes, size_t aCount, const std::array<bool, N>& acSelected, std::array<Write, M>& aOutput, size_t& aWritten) noexcept
{
    aWritten = 0;
    if (!aEntries || aCount > N || aEntries > UINTPTR_MAX - aCount * kEntryStride)
        return false;
    for (size_t i = 0; i < aCount; ++i)
    {
        if (!acSelected[i])
            continue;
        if (acNodes[i])
        {
            if (acNodes[i] > UINTPTR_MAX - kNodeWorld || aWritten == M)
                return false;
            aOutput[aWritten++] = {acNodes[i] + kNodeWorld, static_cast<uint16_t>(i), true};
        }
        if (aWritten == M)
            return false;
        aOutput[aWritten++] = {aEntries + i * kEntryStride + kEntryWorld, static_cast<uint16_t>(i), false};
    }
    return true;
}

// Fixed-size open-addressing table of nonzero pointers, no allocation. Readers only Find.
template <size_t Slots> struct AddressTable
{
    static_assert(Slots && (Slots & (Slots - 1)) == 0, "power of two");
    std::array<uintptr_t, Slots> Key{};
    std::array<int16_t, Slots> Value{};
    size_t Used{};
    static size_t Hash(uintptr_t aKey) noexcept
    {
        uint64_t h = static_cast<uint64_t>(aKey) * 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(h >> 40) & (Slots - 1);
    }
    void Clear() noexcept
    {
        Key.fill(0);
        Value.fill(-1);
        Used = 0;
    }
    // False when full (at most half full is allowed), the key is zero, or the key is already present.
    bool Insert(uintptr_t aKey, int16_t aValue) noexcept
    {
        if (!aKey || aValue < 0 || Used >= Slots / 2)
            return false;
        for (size_t i = Hash(aKey), n = 0; n < Slots; ++n, i = (i + 1) & (Slots - 1))
        {
            if (Key[i] == aKey)
                return false;
            if (!Key[i])
            {
                Key[i] = aKey;
                Value[i] = aValue;
                ++Used;
                return true;
            }
        }
        return false;
    }
    int Find(uintptr_t aKey) const noexcept
    {
        if (!aKey)
            return -1;
        for (size_t i = Hash(aKey), n = 0; n < Slots; ++n, i = (i + 1) & (Slots - 1))
        {
            if (Key[i] == aKey)
                return Value[i];
            if (!Key[i])
                return -1;
        }
        return -1;
    }
    // replace the value of a present key (false if absent or the value is negative).
    bool Assign(uintptr_t aKey, int16_t aValue) noexcept
    {
        if (!aKey || aValue < 0)
            return false;
        for (size_t i = Hash(aKey), n = 0; n < Slots; ++n, i = (i + 1) & (Slots - 1))
        {
            if (Key[i] == aKey)
            {
                Value[i] = aValue;
                return true;
            }
            if (!Key[i])
                return false;
        }
        return false;
    }
};

// Bounded iterative DFS that collects every object reachable from root exactly once into an AddressTable set.
// visit(node, depth, push) returns the object's kind (>= 0, stored as the table value) or < 0 when the object
// could not be read (it stays a member with kind 0, but the set is then incomplete). push(child) queues a child
// and returns false when it cannot (depth cap or a full stack; both make the set incomplete).
struct SubtreePending
{
    uintptr_t Node{};
    uint32_t Depth{};
};
struct SubtreeStats
{
    size_t Objects{};
    uint32_t MaxDepth{}, Duplicates{}, Unreadable{};
    bool Truncated{}, DepthCapped{};
    bool Complete() const noexcept { return Objects && !Truncated && !DepthCapped && !Unreadable; }
};
template <size_t Slots, size_t Max, class Visit>
SubtreeStats
CollectSubtree(uintptr_t aRoot, uint32_t aMaxDepth, AddressTable<Slots>& aSet, std::array<uintptr_t, Max>& aOrder, std::array<SubtreePending, Max>& aStack, Visit&& aVisit) noexcept
{
    static_assert(Max <= Slots / 2, "the table admits at most Slots/2 keys");
    SubtreeStats stats{};
    aSet.Clear();
    if (!aRoot)
        return stats;
    size_t pending = 0;
    aStack[pending++] = {aRoot, 0};
    while (pending)
    {
        const auto item = aStack[--pending];
        if (aSet.Find(item.Node) >= 0)
        {
            ++stats.Duplicates;
            continue;
        }
        if (stats.Objects == Max || !aSet.Insert(item.Node, 0))
        {
            stats.Truncated = true;
            break;
        }
        aOrder[stats.Objects++] = item.Node;
        stats.MaxDepth = std::max(stats.MaxDepth, item.Depth);
        const auto push = [&](uintptr_t aChild) noexcept
        {
            if (!aChild)
                return true;
            if (item.Depth >= aMaxDepth)
            {
                stats.DepthCapped = true;
                return false;
            }
            if (pending == Max)
            {
                stats.Truncated = true;
                return false;
            }
            aStack[pending++] = {aChild, item.Depth + 1};
            return true;
        };
        const int kind = aVisit(item.Node, item.Depth, push);
        if (kind < 0)
            ++stats.Unreadable;
        else if (kind > 0)
            aSet.Assign(item.Node, static_cast<int16_t>(kind));
    }
    return stats;
}
// Order-sensitive hash of a node's child pointer array (a replaced, added or removed child changes it).
inline uint64_t ChildHash(const uintptr_t* apChildren, size_t aCount) noexcept
{
    uint64_t h = 0xCBF29CE484222325ull ^ static_cast<uint64_t>(aCount);
    for (size_t i = 0; i < aCount; ++i)
    {
        h ^= static_cast<uint64_t>(apChildren[i]);
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}
// Native NiMatrix3 is row-major: element (row, column) at raw[row * 3 + column]; glm is column-major.
inline void PackRotation(const glm::quat& acQuaternion, Transform& aTransform) noexcept
{
    const glm::mat3 m = glm::mat3_cast(acQuaternion);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            aTransform[row * 3 + column] = m[column][row];
}
inline bool UnpackRotation(const Transform& acTransform, glm::quat& aQuaternion) noexcept
{
    glm::mat3 m{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            m[column][row] = acTransform[row * 3 + column];
    if (!BodyPoseValidation::IsRotation(m))
        return false;
    const glm::quat raw = glm::quat_cast(m);
    const float length = std::sqrt(raw.w * raw.w + raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);
    if (!std::isfinite(length) || length < .5f)
        return false;
    aQuaternion = glm::quat{raw.w / length, raw.x / length, raw.y / length, raw.z / length};
    return true;
}
inline float AngleDeg(const glm::quat& acLeft, const glm::quat& acRight) noexcept
{
    const auto q = glm::normalize(glm::conjugate(acLeft) * acRight);
    return glm::degrees(2.f * std::atan2(glm::length(glm::vec3{q.x, q.y, q.z}), std::fabs(q.w)));
}
// Max / median / count over 1 degree of per-entry angles (non-finite values ignored).
template <size_t N> struct AngleStats
{
    std::array<float, N> Values{};
    size_t Count{};
    float Max{};
    uint32_t Over1{};
    void Clear() noexcept
    {
        Count = 0;
        Max = 0.f;
        Over1 = 0;
    }
    void Add(float aValue) noexcept
    {
        if (!std::isfinite(aValue) || Count == N)
            return;
        Values[Count++] = aValue;
        Max = std::max(Max, aValue);
        Over1 += aValue > 1.f ? 1u : 0u;
    }
    float Median() noexcept
    {
        if (!Count)
            return 0.f;
        const auto begin = Values.begin(), end = Values.begin() + static_cast<std::ptrdiff_t>(Count);
        const auto mid = begin + static_cast<std::ptrdiff_t>(Count / 2);
        std::nth_element(begin, mid, end);
        const float upper = *mid;
        if (Count % 2)
            return upper;
        return (upper + *std::max_element(begin, mid)) * .5f;
    }
};
// Rigid attachments. SE 1.7.104 NiAVObject (dumpbin of the installed image): parent +30, collision object +40,
// local NiTransform +48, world +7C, previous world +B0, flags +F4. UpdateWorldData EEBF40: with a collision object
// the collision object updates the world (EEBFBF-EEBFD1); flags bit 9 (200h) copies the parent world unchanged
// (EEBFE8-EEC034); otherwise world = compose(parent world, local) (EEC039-EEC042, scale stored at +AC by EEC0CA).
// the collision object's update composes the node itself through EEC1A0 (the same two arms, compose call at
// EEC214) or copies its Havok body's transform into it; see DecideRigid.
inline constexpr size_t kNodeCollision = 0x40, kNodeLocal = 0x48, kNodeFlags = 0xF4, kGeometrySkin = 0x130;
inline constexpr uint32_t kIgnoreLocalFlag = 0x200;
static_assert(kNodeLocal + kTransformBytes == kNodeWorld, "local ends where the world starts");
// Native NiTransform compose 3059A0 (RCX parent, RDX out, R8 local; out = parent x local) with the same float operation
// order, so a world the engine composed from these two inputs is reproduced bit for bit:
// rotation out[i][j] = (P[i][0]*L[0][j] + P[i][1]*L[1][j]) + P[i][2]*L[2][j]           (3059B8-305BFC)
// translation out[i] = ((P[i][0]*x + P[i][1]*y) + P[i][2]*z) * P.scale + P.t[i]       (305C02-305C99)
// scale out = L.scale * P.scale                                                       (3059B8-3059CD)
inline Transform ComposeWorld(const Transform& acParent, const Transform& acLocal) noexcept
{
    Transform o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            o[i * 3 + j] = (acParent[i * 3 + 0] * acLocal[0 * 3 + j] + acParent[i * 3 + 1] * acLocal[1 * 3 + j]) + acParent[i * 3 + 2] * acLocal[2 * 3 + j];
    for (int i = 0; i < 3; ++i)
        o[9 + i] = ((acParent[i * 3 + 0] * acLocal[9] + acParent[i * 3 + 1] * acLocal[10]) + acParent[i * 3 + 2] * acLocal[11]) * acParent[12] + acParent[9 + i];
    o[12] = acLocal[12] * acParent[12];
    return o;
}
// Element-wise closeness: rotation entries, translation (absolute + relative to the coordinate size), scale (relative).
inline bool NearTransform(const Transform& acLeft, const Transform& acRight, float aRotation, float aTravel, float aScale) noexcept
{
    if (!Finite(acLeft) || !Finite(acRight))
        return false;
    for (int k = 0; k < 9; ++k)
        if (!(std::fabs(acLeft[k] - acRight[k]) <= aRotation))
            return false;
    for (int k = 9; k < 12; ++k)
        if (!(std::fabs(acLeft[k] - acRight[k]) <= aTravel + 1e-6f * std::max(std::fabs(acLeft[k]), std::fabs(acRight[k]))))
            return false;
    return std::fabs(acLeft[12] - acRight[12]) <= aScale * std::max(std::fabs(acLeft[12]), std::fabs(acRight[12]));
}
// Why a rigid descendant was not written this pass. ParentUnwritten is the natural case (its written ancestor was
// left native by the solver because its solved value equals native); every other reason is a refusal. appended
// ParentRefused (the written-capable ancestor entry was refused by the flat validation, non-finite or ValidTarget, and
// so left native) and OwnUpdate (static: the class's world update, vtable slot 180h, is not the plain NiAVObject one,
// EEBF40, e.g. NiBillboardNode F02F60, BSValueNode FD36E0, NiParticleSystem F99E20). Appended only (logged by name).
// appended CollisionDriven: the item owns a collision object and its native world this pass is not bit for bit
// the engine's compose from its parent's native world and its local (the collision object set it from the Havok body).
// Collision is now only static: a collision object of a class whose world update was not analysed, or its guards failed.
enum class RigidSkip : uint8_t
{
    None,
    ParentUnwritten,
    ParentSkipped,
    Skinned,
    Collision,
    OtherClass,
    Particles,
    Unverified,
    Unreadable,
    Changed,
    NotComposed,
    Invalid,
    ParentRefused,
    OwnUpdate,
    CollisionDriven,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(RigidSkip::Count)> kRigidSkipNames{
    "none",       "parent_unwritten", "parent_skipped", "skinned", "collision",      "other_class", "particles",       "unverified",
    "unreadable", "changed",          "not_composed",   "invalid", "parent_refused", "own_update",  "collision_driven"};
inline constexpr size_t kNodeUpdateWorldSlot = 0x180; // NiAVObject::UpdateWorldData, EEBF40 for every plain node and geometry class
// Static part, from the topology walk: an item hangs under a flat entry's linked node (ParentEntry) or under an earlier
// item (ParentRigid, always a smaller index: the walk is preorder). Static != None: never written (skinned geometry,
// a collision object of an unanalysed class (; /any collision object), a class other than
// NiNode/BSGeometry, a particle system, a class with its own world update, a parent whose child array is unknown).
struct RigidItem
{
    int32_t ParentEntry = -1, ParentRigid = -1;
    RigidSkip Static = RigidSkip::None;
};
// Per pass, read after the flat stores: Matches = vtable, parent pointer, collision object pointer () and the
// parent's child array as recorded. Collision: the item owns a collision object (+40), the one recorded at the build.
struct RigidRead
{
    bool Readable{}, Matches{}, Collision{};
    uint32_t Flags{};
    Transform Local{}, World{};
};
struct RigidResult
{
    RigidSkip Reason = RigidSkip::None;
    bool Valid{}, Store{}, Exact{}; // Valid: Target is this pass's world (Store: it differs from native and is written)
    // Collision = a collision-owning item that reached the compose test (CollisionComposed: it passed, exactly);
    // the largest element differences between its native world and that compose (0 when exact; telemetry only).
    bool Collision{}, CollisionComposed{};
    float CollisionDevRotation{}, CollisionDevTravel{};
    Transform Target{}, Native{};
    float Travel{}, AngleDeg{};
};
inline constexpr float kRigidRotationTolerance = 1e-4f, kRigidTravelTolerance = 0.05f, kRigidScaleTolerance = 1e-5f;
// One item. The native world must be what the engine composed from the parent's pre-commit world and the item's local
// (else the node is not a plain composed child: refuse); the target is the same compose from the committed parent world.
// Scale is never posed: the target keeps the native scale bits. Validation is the flat entries' (ValidTarget).
// a collision-owning item is accepted only when that compose reproduces its native world bit for bit (tolerance
// zero). Native reason (SE 1.7.104, dumpbin): EEBFBF hands such a node to its collision object's world update, which
// ends either in EEC1A0, the node's own compose through the same 3059A0 (alone, or followed in the same call by a push
// of that just-composed world into the Havok body or phantom), or in 106B160, which copies the body's transform
// (rotation from a quaternion, position x 69.99) into the node world (with kSetLocal also local = inverse(parent) x
// body, so parent x local is then within rounding of the world: no non-zero tolerance separates that case). The
// non-collision rule reproduced 9,061 of 9,061 native worlds exactly in the K4 rehearsal. The collision object, its
// body and Havok are never written; only world +7C, after the traversal (no native path reads it into Havok).
inline RigidResult DecideRigid(
    const RigidItem& acItem, bool aParentValid, RigidSkip aParentReason, const Transform& acParentNew, const Transform& acParentNative, const RigidRead& acRead,
    float aMaxTravel) noexcept
{
    RigidResult r{};
    const auto refuse = [&r](RigidSkip aReason) noexcept
    {
        r.Reason = aReason;
        r.Valid = r.Store = false;
        return r;
    };
    if (acItem.Static != RigidSkip::None)
        return refuse(acItem.Static);
    // Natural (unwritten) and flat-refused parents keep their reason all the way down; any other refusal is ParentSkipped.
    if (!aParentValid)
        return refuse(aParentReason == RigidSkip::ParentUnwritten || aParentReason == RigidSkip::ParentRefused ? aParentReason : RigidSkip::ParentSkipped);
    if (!acRead.Readable)
        return refuse(RigidSkip::Unreadable);
    if (!acRead.Matches)
        return refuse(RigidSkip::Changed);
    if (!Finite(acRead.Local) || !Finite(acRead.World) || !Finite(acParentNew) || !Finite(acParentNative) || !(acRead.Local[12] > 0.f) || !(acRead.World[12] > 0.f))
        return refuse(RigidSkip::Invalid);
    const bool ignoreLocal = (acRead.Flags & kIgnoreLocalFlag) != 0;
    const Transform composed = ignoreLocal ? acParentNative : ComposeWorld(acParentNative, acRead.Local);
    r.Exact = SameBits(composed, acRead.World);
    if (acRead.Collision)
    {
        r.Collision = true;
        for (int k = 0; k < 9; ++k)
            r.CollisionDevRotation = std::max(r.CollisionDevRotation, std::fabs(composed[k] - acRead.World[k]));
        for (int k = 9; k < 12; ++k)
            r.CollisionDevTravel = std::max(r.CollisionDevTravel, std::fabs(composed[k] - acRead.World[k]));
        if (!r.Exact)
            return refuse(RigidSkip::CollisionDriven);
        r.CollisionComposed = true;
    }
    else if (!r.Exact && !NearTransform(composed, acRead.World, kRigidRotationTolerance, kRigidTravelTolerance, kRigidScaleTolerance))
        return refuse(RigidSkip::NotComposed);
    Transform target = ignoreLocal ? acParentNew : ComposeWorld(acParentNew, acRead.Local);
    // a parent whose committed scale differs from its native scale (the root-scale pass) passes it on as the
    // engine's compose would (scale = local scale x parent scale, or the parent's own with ignoreLocal): the composed
    // scale is kept. Any other parent: the rule, the item's native scale bits (scale not posed).
    const bool parentScaled = std::memcmp(&acParentNew[12], &acParentNative[12], sizeof(float)) != 0;
    if (!parentScaled)
        target[12] = acRead.World[12];
    if (!ValidTargetScale(target, acRead.World, target[12], aMaxTravel))
        return refuse(RigidSkip::Invalid);
    r.Native = acRead.World;
    r.Target = target;
    r.Valid = true;
    r.Store = !SameBits(target, acRead.World);
    const float dx = target[9] - acRead.World[9], dy = target[10] - acRead.World[10], dz = target[11] - acRead.World[11];
    r.Travel = std::sqrt(dx * dx + dy * dy + dz * dz);
    glm::quat a{}, b{};
    r.AngleDeg = UnpackRotation(acRead.World, a) && UnpackRotation(target, b) ? AngleDeg(a, b) : 0.f;
    return r;
}
// All items in order (parents first). Entry state: written this pass (the flat commit stored it), refused by the flat
// validation (; left native, so ParentRefused instead of ParentUnwritten), its committed world and its pre-commit
// native world. Returns the number of results to store.
template <size_t N, size_t E>
size_t SolveRigid(
    const std::array<RigidItem, N>& acItems, size_t aCount, const std::array<RigidRead, N>& acReads, const std::array<bool, E>& acEntryWritten,
    const std::array<bool, E>& acEntryRefused, const std::array<Transform, E>& acEntryTarget, const std::array<Transform, E>& acEntryNative, size_t aEntryCount,
    std::array<RigidResult, N>& aOutput, float aMaxTravel) noexcept
{
    size_t stores = 0;
    for (size_t n = 0; n < aCount && n < N; ++n)
    {
        const auto& item = acItems[n];
        bool valid{};
        RigidSkip reason = RigidSkip::ParentSkipped;
        const Transform* parentNew = nullptr;
        const Transform* parentNative = nullptr;
        if (item.ParentEntry >= 0 && static_cast<size_t>(item.ParentEntry) < aEntryCount && static_cast<size_t>(item.ParentEntry) < E)
        {
            const auto e = static_cast<size_t>(item.ParentEntry);
            valid = acEntryWritten[e];
            reason = valid ? RigidSkip::None : acEntryRefused[e] ? RigidSkip::ParentRefused : RigidSkip::ParentUnwritten;
            parentNew = &acEntryTarget[e];
            parentNative = &acEntryNative[e];
        }
        else if (item.ParentRigid >= 0 && static_cast<size_t>(item.ParentRigid) < n)
        {
            const auto& parent = aOutput[static_cast<size_t>(item.ParentRigid)];
            valid = parent.Valid;
            reason = parent.Reason;
            parentNew = &parent.Target;
            parentNative = &parent.Native;
        }
        if (!parentNew)
        {
            aOutput[n] = {};
            aOutput[n].Reason = item.Static != RigidSkip::None ? item.Static : RigidSkip::Unverified;
            continue;
        }
        aOutput[n] = DecideRigid(item, valid, reason, *parentNew, *parentNative, acReads[n], aMaxTravel);
        stores += aOutput[n].Store ? 1u : 0u;
    }
    return stores;
}
// one item's outcome in one pass (commit record rigidItems): a refusal by its RigidSkip name, or one of the
// accepted outcomes appended after them. collision_composed = a collision-owning item written (exact compose).
inline constexpr uint8_t kRigidOutcomeWritten = static_cast<uint8_t>(RigidSkip::Count), kRigidOutcomeUnchanged = kRigidOutcomeWritten + 1,
                         kRigidOutcomeCollisionComposed = kRigidOutcomeWritten + 2, kRigidOutcomeCount = kRigidOutcomeWritten + 3;
inline constexpr std::array<const char*, 3> kRigidOutcomeAcceptedNames{"written", "unchanged", "collision_composed"};
inline uint8_t RigidOutcome(const RigidResult& acResult) noexcept
{
    if (!acResult.Valid)
        return acResult.Reason == RigidSkip::None ? static_cast<uint8_t>(RigidSkip::Unverified) : static_cast<uint8_t>(acResult.Reason);
    if (!acResult.Store)
        return kRigidOutcomeUnchanged;
    return acResult.CollisionComposed ? kRigidOutcomeCollisionComposed : kRigidOutcomeWritten;
}
inline const char* RigidOutcomeName(uint8_t aCode) noexcept
{
    if (aCode < static_cast<uint8_t>(RigidSkip::Count))
        return kRigidSkipNames[aCode];
    if (aCode < kRigidOutcomeCount)
        return kRigidOutcomeAcceptedNames[aCode - kRigidOutcomeWritten];
    return "unknown";
}

// Source plausibility guard replacing 's 50 ms
// step rule (which trips on fast real motion and does not suppress the archive's glitch). A pass's source sample is
// IMPLAUSIBLE when the root-relative Neck-to-Head rotation, or (with legs) the Spine1-to-Pelvis rotation, exceeds
// kFlipImplausibleDeg: no body bends that far. Arms, hands and legs are never tested, so a fast real limb motion is
// never held. Recorded evidence (458 frames): normal frames Neck-Head
// 10.6-57.1 deg and Spine1-Pelvis 1.3-44.8 deg; the flipped frames (1, 441-457) 169.8-170.3 and 156.5-159.4 deg; either
// test alone separates 458 of 458 at 110 deg.
// A sample is interpolated between two source ticks (Before, After). An implausible pass marks its After tick bad and
// discards every admitted candidate whose After tick is at or past its Before tick (either endpoint may be the flipped
// one, so poses interpolated toward it are suspect). A plausible pass whose Before tick is at or before the newest bad
// tick is CONTAMINATED (interpolated out of a bad tick) and handled like an implausible one. While implausible or
// contaminated, the newest remaining admitted candidate is used (HELD). RELEASE (the caller commits nothing: native
// pose, expiry semantics): no admitted candidate remains (NoCandidate), or the held candidate is more than
// kFlipHoldTimeoutMs older than the pass (Timeout; sticky for the episode because nothing is admitted during it).
// The first clean pass ends the episode and is admitted. The ring keeps the newest candidate per distinct After tick.
// A boneless or empty sample is passed through untested and not admitted. A new stream generation starts empty.
inline constexpr float kFlipImplausibleDeg = 110.f;
inline constexpr double kFlipHoldTimeoutMs = 2000.0;
inline constexpr size_t kFlipSpine1 = 0, kFlipNeck = 2, kFlipHead = 3, kFlipPelvis = BodyPosePresentation::kPelvis;
inline constexpr uint32_t kFlipUpperMask = (1u << BodyPosePresentation::kUpperBones) - 1u;
inline constexpr uint32_t kFlipAllMask = (1u << BodyPosePresentation::kWireBones) - 1u;
static_assert(kFlipHead < BodyPosePresentation::kUpperBones && kFlipPelvis >= BodyPosePresentation::kUpperBones, "legs bit covers the pelvis");
enum class FlipReason : uint8_t
{
    None,
    NeckHead,
    Spine1Pelvis,
    Contaminated,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(FlipReason::Count)> kFlipReasonNames{"none", "neck_head", "spine1_pelvis", "contaminated"};
struct FlipPose
{
    std::array<glm::quat, BodyPosePresentation::kWireBones> Rotation{};
    uint32_t Valid{}; // one bit per wire
    glm::vec3 Hip{};
    bool HasLegs{}, HasHips{};
    double TimeMs{};
    uint64_t Generation{}, BeforeTick{}, AfterTick{};
};
struct FlipDecision
{
    bool Checked{}, Reset{}, Implausible{}, Contaminated{}, Hold{}, Release{}, Timeout{}, NoCandidate{}, EpisodeStart{}, EpisodeEnd{}, TimeoutStart{};
    FlipReason Reason = FlipReason::None;
    float NeckHeadDeg = -1.f, Spine1PelvisDeg = -1.f; // -1: not measured (no bones, or no legs for Spine1-Pelvis)
    double HeldAgeMs{}, EpisodeMs{};
    uint64_t HeldTick{}; // After tick of the held candidate
    uint32_t Purged{};   // admitted candidates discarded by this pass
};
inline FlipPose FlipPoseFrom(const BodyPosePresentation::Input& acInput, double aTimeMs, uint64_t aGeneration, uint64_t aBeforeTick, uint64_t aAfterTick) noexcept
{
    FlipPose p{};
    for (size_t wire = 0; wire < BodyPosePresentation::kWireBones; ++wire)
        p.Rotation[wire] = acInput.Rotations[wire];
    p.Valid = acInput.HasData && !acInput.NoBones ? (acInput.HasLegs ? kFlipAllMask : kFlipUpperMask) : 0u;
    p.Hip = acInput.HipOffset;
    p.HasLegs = acInput.HasLegs;
    p.HasHips = acInput.HasHips;
    p.TimeMs = aTimeMs;
    p.Generation = aGeneration;
    p.BeforeTick = aBeforeTick;
    p.AfterTick = aAfterTick;
    return p;
}
inline void ApplyFlipPose(const FlipPose& acPose, BodyPosePresentation::Input& aInput) noexcept
{
    for (size_t wire = 0; wire < BodyPosePresentation::kWireBones; ++wire)
        aInput.Rotations[wire] = acPose.Rotation[wire];
    aInput.HipOffset = acPose.Hip;
    aInput.HasLegs = acPose.HasLegs;
    aInput.HasHips = acPose.HasHips;
}
struct FlipPlausibility
{
    float NeckHeadDeg = -1.f, Spine1PelvisDeg = -1.f;
    FlipReason Reason = FlipReason::None; // NeckHead wins when both exceed the limit
};
// A non-finite angle (a non-unit wire) does not trigger: the solver refuses such a sample itself, as before.
inline FlipPlausibility Plausibility(const FlipPose& acPose, float aLimitDeg) noexcept
{
    FlipPlausibility r{};
    const auto has = [&acPose](size_t aWire) noexcept
    {
        return (acPose.Valid & (1u << aWire)) != 0;
    };
    if (has(kFlipNeck) && has(kFlipHead))
        r.NeckHeadDeg = AngleDeg(acPose.Rotation[kFlipNeck], acPose.Rotation[kFlipHead]);
    if (has(kFlipSpine1) && has(kFlipPelvis))
        r.Spine1PelvisDeg = AngleDeg(acPose.Rotation[kFlipSpine1], acPose.Rotation[kFlipPelvis]);
    if (r.NeckHeadDeg > aLimitDeg)
        r.Reason = FlipReason::NeckHead;
    else if (r.Spine1PelvisDeg > aLimitDeg)
        r.Reason = FlipReason::Spine1Pelvis;
    return r;
}
template <size_t N> struct FlipGuard
{
    static_assert(N >= 2, "the hold needs the candidate before the flipped pair");
    std::array<FlipPose, N> Ring{}; // Ring[0, Count): admitted candidates, one per After tick, ascending (oldest first)
    size_t Count{};
    bool HaveGeneration{}, HaveBad{}, Episode{}, TimedOut{};
    uint64_t Generation{}, BadTick{};
    double EpisodeStartMs{};
    void Reset() noexcept
    {
        Count = 0;
        HaveBad = Episode = TimedOut = false;
        BadTick = 0;
        EpisodeStartMs = 0.;
    }
    const FlipPose* Newest() const noexcept { return Count ? &Ring[Count - 1] : nullptr; }
    void Admit(const FlipPose& acPose) noexcept
    {
        while (Count && Ring[Count - 1].AfterTick >= acPose.AfterTick)
            --Count; // newest per After tick; never out of tick order
        if (Count == N)
        {
            for (size_t k = 1; k < N; ++k)
                Ring[k - 1] = Ring[k];
            --Count;
        }
        Ring[Count++] = acPose;
    }
    // use = the pose to solve from: the candidate (admitted, untested or released) or the held candidate (Hold).
    // Release: the caller must not commit this pass.
    FlipDecision Step(const FlipPose& acCandidate, FlipPose& aUse, float aLimitDeg, double aTimeoutMs) noexcept
    {
        FlipDecision d{};
        aUse = acCandidate;
        if (HaveGeneration && acCandidate.Generation != Generation)
        {
            Reset();
            d.Reset = true;
        }
        HaveGeneration = true;
        Generation = acCandidate.Generation;
        if (!acCandidate.Valid)
            return d; // no bones to test: passed through, not admitted, the episode state is kept
        d.Checked = true;
        const auto p = Plausibility(acCandidate, aLimitDeg);
        d.NeckHeadDeg = p.NeckHeadDeg;
        d.Spine1PelvisDeg = p.Spine1PelvisDeg;
        d.Reason = p.Reason;
        d.Implausible = p.Reason != FlipReason::None;
        if (d.Implausible)
        {
            if (!HaveBad || acCandidate.AfterTick > BadTick)
                BadTick = acCandidate.AfterTick;
            HaveBad = true;
            while (Count && Ring[Count - 1].AfterTick >= acCandidate.BeforeTick)
            {
                --Count;
                ++d.Purged;
            }
        }
        else if (HaveBad && acCandidate.BeforeTick <= BadTick)
        {
            d.Contaminated = true;
            d.Reason = FlipReason::Contaminated;
        }
        if (!d.Implausible && !d.Contaminated)
        {
            d.EpisodeEnd = Episode;
            if (Episode)
                d.EpisodeMs = acCandidate.TimeMs - EpisodeStartMs;
            Episode = TimedOut = false;
            Admit(acCandidate);
            return d;
        }
        if (!Episode)
        {
            Episode = true;
            TimedOut = false;
            EpisodeStartMs = acCandidate.TimeMs;
            d.EpisodeStart = true;
        }
        d.EpisodeMs = acCandidate.TimeMs - EpisodeStartMs;
        const FlipPose* held = Newest();
        if (!held)
        {
            d.Release = d.NoCandidate = true;
            return d;
        }
        d.HeldAgeMs = acCandidate.TimeMs - held->TimeMs;
        d.HeldTick = held->AfterTick;
        if (!(d.HeldAgeMs <= aTimeoutMs))
        {
            d.Release = d.Timeout = true;
            d.TimeoutStart = !TimedOut;
            TimedOut = true;
            return d;
        }
        d.Hold = true;
        aUse = *held;
        return d;
    }
};

// Children of a flat entry by the int16 flat parent index (entry+68). Returns how many; first = the lowest index.
template <size_t N> size_t FlatChildren(const std::array<int16_t, N>& acFlatParent, size_t aCount, int aParent, int& aFirst) noexcept
{
    aFirst = -1;
    size_t n = 0;
    for (size_t i = 0; i < aCount && i < N; ++i)
        if (aParent >= 0 && acFlatParent[i] == aParent)
        {
            if (!n)
                aFirst = static_cast<int>(i);
            ++n;
        }
    return n;
}
// The one child of an entry (a foot's toe), or -1 when it has none or several.
template <size_t N> int UniqueChild(const std::array<int16_t, N>& acFlatParent, size_t aCount, int aParent) noexcept
{
    int first{};
    return FlatChildren(acFlatParent, aCount, aParent, first) == 1 ? first : -1;
}
// The finger bones of one hand, found by structure as upstream VRBodySync FindFingers does (flattened entries have no
// names), but stricter: every entry whose flat parent is the hand and that has children must be a simple chain of exactly
// three (one child, which has one child, which has none); there must be exactly five such chains. Leaves under the hand
// (magic node, WEAPON / SHIELD, AnimObject) are not fingers. Order: entry order, thumb first on the vanilla skeleton (the
// flat tree is skeleton.nif's NPC Root subtree in depth-first order: Finger00..02, 10..12, ..., 40..42), the order the
// sender's FindFingers uses for the 30 wire rotations. Anything else: false and out all -1 (no fingers for that hand).
template <size_t N>
bool FindFingerChains(const std::array<int16_t, N>& acFlatParent, size_t aCount, int aHand, std::array<int, BodyPosePresentation::kFingerBonesPerHand>& aOutput) noexcept
{
    aOutput.fill(-1);
    if (aHand < 0 || static_cast<size_t>(aHand) >= aCount || aCount > N)
        return false;
    size_t found = 0;
    for (size_t i = 0; i < aCount; ++i)
    {
        if (acFlatParent[i] != aHand)
            continue;
        int second{}, third{}, fourth{};
        const size_t direct = FlatChildren(acFlatParent, aCount, static_cast<int>(i), second);
        if (!direct)
            continue;
        if (direct != 1 || FlatChildren(acFlatParent, aCount, second, third) != 1 || FlatChildren(acFlatParent, aCount, third, fourth) != 0 ||
            found == BodyPosePresentation::kFingerChains)
        {
            aOutput.fill(-1);
            return false;
        }
        aOutput[found * BodyPosePresentation::kFingerChainBones + 0] = static_cast<int>(i);
        aOutput[found * BodyPosePresentation::kFingerChainBones + 1] = second;
        aOutput[found * BodyPosePresentation::kFingerChainBones + 2] = third;
        ++found;
    }
    if (found != BodyPosePresentation::kFingerChains)
    {
        aOutput.fill(-1);
        return false;
    }
    return true;
}
// A world the engine composed from parent and local: bit for bit (exact) or within the rigid tolerances. Used per pass on
// the finger entries, whose native world must be compose(parent world, own local) (SE 1.7.104 EF4FBB: 3059A0(parent
// entry+34 or tree+7C, out, entry+00) for a flat-only entry), before their local translations are trusted.
inline bool ComposesFrom(const Transform& acWorld, const Transform& acParent, const Transform& acLocal, bool& aExact) noexcept
{
    const Transform composed = ComposeWorld(acParent, acLocal);
    aExact = SameBits(composed, acWorld);
    return aExact || NearTransform(composed, acWorld, kRigidRotationTolerance, kRigidTravelTolerance, kRigidScaleTolerance);
}

// : one biped body's Havok state from the reads collision object (node+40) -> body (+20) -> its hkp
// world object (+10) -> that one's Havok world (+10) and motion type (+160), the offsets ReadCollisionBodyChain reads.
// In a world: a ragdoll the engine or a script added (knockdown, death, Havok hit, behaviour event, Papyrus
// ForceAddRagdollToWorld / PushActorAway; NATIVE-RISKS section 1). Dynamic outside a world: left so after a ragdoll
// was removed (k5c: motion 3/2 with world 0); not simulated. Unreadable: a link could not be read.
inline constexpr size_t kHavokBodyOffset = 0x20, kHavokObjectOffset = 0x10, kHavokWorldOffset = 0x10, kHavokMotionOffset = 0x160;
enum class HavokState : uint8_t
{
    NoBody,
    Detached,
    DynamicOutside,
    InWorld,
    Unreadable,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(HavokState::Count)> kHavokStateNames{"no_body", "detached", "dynamic_outside", "in_world", "unreadable"};
struct HavokRead
{
    bool BodyRead{}, ObjectRead{}, StateRead{};
    uintptr_t Collision{}, Body{}, Object{}, World{};
    uint8_t Motion{};
};
inline HavokState ClassifyHavok(const HavokRead& acRead) noexcept
{
    if (!acRead.Collision)
        return HavokState::NoBody;
    if (!acRead.BodyRead)
        return HavokState::Unreadable;
    if (!acRead.Body)
        return HavokState::NoBody;
    if (!acRead.ObjectRead)
        return HavokState::Unreadable;
    if (!acRead.Object)
        return HavokState::NoBody;
    if (!acRead.StateRead)
        return HavokState::Unreadable;
    if (acRead.World)
        return HavokState::InWorld;
    return acRead.Motion == 4 || acRead.Motion == 5 ? HavokState::Detached : HavokState::DynamicOutside; // 4 keyframed, 5 fixed
}
// The gate: a body in a Havok world, or one whose state cannot be read, refuses the pass.
inline bool HavokRefuses(HavokState aState) noexcept
{
    return aState == HavokState::InWorld || aState == HavokState::Unreadable;
}

// : selected re-selection. The remote player behind a copy is the receiver's pose identity
// (RemotePose::Identity: Session, and the server id in the upper 32 bits of Entity; the lower 32 bits are the copy's
// FormID, which changes when the copy is respawned). Session 0 is never issued (the pose session starts at 1).
struct RemotePlayerKey
{
    uint64_t Session{};
    uint32_t Server{};
    bool Known() const noexcept { return Session != 0; }
};
inline RemotePlayerKey RemotePlayerOf(uint64_t aSession, uint64_t aEntity) noexcept
{
    return {aSession, static_cast<uint32_t>(aEntity >> 32)};
}
inline bool SameRemotePlayer(const RemotePlayerKey& acLeft, const RemotePlayerKey& acRight) noexcept
{
    return acLeft.Known() && acRight.Known() && acLeft.Session == acRight.Session && acLeft.Server == acRight.Server;
}
// A candidate copy takes over the selected only if it is another copy of the selected's own remote player (same session and
// server id) and the selected's current copy has no live binding left (gone, expired or forgotten); never two at once.
enum class SelectedRebind : uint8_t
{
    Keep,
    Rebind,
    Unknown,
    OtherPlayer,
    CurrentLive,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(SelectedRebind::Count)> kSelectedRebindNames{"keep", "rebind", "unknown", "other_player", "current_live"};
inline SelectedRebind DecideSelectedRebind(uint32_t aCurrent, uint32_t aCandidate, const RemotePlayerKey& acSelected, const RemotePlayerKey& acKey, bool aCurrentLive) noexcept
{
    if (!aCurrent || !aCandidate || aCurrent == aCandidate)
        return SelectedRebind::Keep;
    if (!acSelected.Known() || !acKey.Known())
        return SelectedRebind::Unknown;
    if (!SameRemotePlayer(acSelected, acKey))
        return SelectedRebind::OtherPlayer;
    if (aCurrentLive)
        return SelectedRebind::CurrentLive;
    return SelectedRebind::Rebind;
}
// When the selected's player leaves or reconnects, its new copy comes back under a new server id, so the rule above sees
// another player and the body was never shown again. Any other remote player may take the selected over once the current
// copy has had no binding for kSelectedReselectHoldMs: one presented copy at a time, and a short network gap (the binding
// survives it until its pose expires) never moves the selected to a third player.
inline constexpr double kSelectedReselectHoldMs = 2000.0;
enum class SelectedReselect : uint8_t
{
    Keep,
    Reselect,
    CurrentLive,
    Waiting
};
// unboundSinceMs: when the current selected was first seen without a binding (0 = not seen unbound yet).
inline SelectedReselect DecideSelectedReselect(uint32_t aCurrent, uint32_t aCandidate, bool aCurrentLive, double aUnboundSinceMs, double aNowMs) noexcept
{
    if (!aCurrent || !aCandidate || aCurrent == aCandidate)
        return SelectedReselect::Keep;
    if (aCurrentLive)
        return SelectedReselect::CurrentLive;
    if (!std::isfinite(aUnboundSinceMs) || !std::isfinite(aNowMs) || !(aUnboundSinceMs > 0.0) || !(aNowMs - aUnboundSinceMs >= kSelectedReselectHoldMs))
        return SelectedReselect::Waiting;
    return SelectedReselect::Reselect;
}
// Branch B presentation grace: values only, independent of receive freshness. A successful FRESH commit
// arms one immutable source deadline; successful held commits and repeated polls never extend it.
inline constexpr double kPresentationGraceMs = 200.0;
struct GraceKey
{
    uint64_t Session{}, Owner{}, Entity{}, Generation{};
    uintptr_t Root{}, Tree{};
    uint32_t Actor{};
    bool Known() const noexcept { return Session && Entity && Generation && Root && Tree && Actor; }
    bool operator==(const GraceKey& acRight) const noexcept
    {
        return Session == acRight.Session && Owner == acRight.Owner && Entity == acRight.Entity && Generation == acRight.Generation && Root == acRight.Root &&
               Tree == acRight.Tree && Actor == acRight.Actor;
    }
};
// The adapter can clear the pose entirely, or collapse an expired pair to its still-fresh After endpoint.
// Neither shape is a newer receive. Recognize only the original endpoints; never consume the replacement inputs.
inline bool UnrefreshedGraceSample(const RemotePose::Sample& acCommitted, const RemotePose::Sample& acSample) noexcept
{
    if (!acCommitted.Generation || acSample.Generation != acCommitted.Generation || acSample.Pose.NoBones)
        return false;
    if (!acSample.Pose.HasData)
        return !acSample.Before.Valid && !acSample.After.Valid;
    const auto same = [](const RemotePose::Provenance& acLeft, const RemotePose::Provenance& acRight) noexcept
    {
        return acLeft.Valid && acRight.Valid && acLeft.Generation == acRight.Generation && acLeft.SourceTick == acRight.SourceTick && std::isfinite(acLeft.ReceivedAtMs) &&
               acLeft.ReceivedAtMs == acRight.ReceivedAtMs;
    };
    return same(acSample.After, acCommitted.After) && (same(acSample.Before, acCommitted.Before) || same(acSample.Before, acCommitted.After));
}
enum class GraceAdmission : uint8_t
{
    None,
    Fresh,
    Hold
};
struct GraceWindow
{
    GraceKey Key{};
    uint64_t Before{}, After{};
    double SourceDeadlineMs{}, LastNowMs{};
    bool Armed{}, Blocked{};
    double HoldDeadlineMs() const noexcept { return SourceDeadlineMs + kPresentationGraceMs; }
    void Invalidate() noexcept
    {
        Armed = false;
        Blocked = true;
    }
    // Only a successfully committed fresh source may arm. Once invalidated, the same/older source cannot
    // re-arm; a genuinely newer source (or a trusted new identity/generation) can resume normally.
    bool CommittedFresh(const GraceKey& acKey, uint64_t aBefore, uint64_t aAfter, double aDeadline, double aNow, bool aSafe) noexcept
    {
        return CommittedFreshCompletion(acKey, aBefore, aAfter, aDeadline, aNow, aNow, aSafe);
    }
    // A store qualified fresh at its FINAL source gate can complete just after the source deadline.
    // Completion may arm only from that proven gate time, within the original bounded grace endpoint.
    bool CommittedFreshCompletion(const GraceKey& acKey, uint64_t aBefore, uint64_t aAfter, double aDeadline, double aCheckedAt, double aCompletedAt, bool aSafe) noexcept
    {
        if (!aSafe || !acKey.Known() || aBefore > aAfter || !std::isfinite(aCheckedAt) || !std::isfinite(aCompletedAt) || !std::isfinite(aDeadline) ||
            !std::isfinite(aDeadline + kPresentationGraceMs) || aCheckedAt < 0. || aDeadline < 0. || aCheckedAt > aDeadline || aCompletedAt < aCheckedAt ||
            aCompletedAt > aDeadline + kPresentationGraceMs || (Key.Known() && aCompletedAt < LastNowMs) ||
            (Key == acKey && (aAfter < After || (Blocked && aAfter <= After) || (aAfter == After && aDeadline > SourceDeadlineMs))))
        {
            Invalidate();
            return false;
        }
        Key = acKey;
        Before = aBefore;
        After = aAfter;
        SourceDeadlineMs = aDeadline;
        LastNowMs = aCompletedAt;
        Armed = true;
        Blocked = false;
        return true;
    }
    GraceAdmission Observe(const GraceKey& acKey, double aNow, bool aSafe) noexcept
    {
        if (!Armed)
            return GraceAdmission::None;
        if (!aSafe || !(acKey == Key) || !std::isfinite(aNow) || aNow < LastNowMs || aNow > HoldDeadlineMs())
        {
            Invalidate();
            return GraceAdmission::None;
        }
        LastNowMs = aNow;
        return aNow <= SourceDeadlineMs ? GraceAdmission::Fresh : GraceAdmission::Hold;
    }
};
// Only a freshness-only refusal may trigger one cached-input rebuild; every safety refusal stays final.
inline bool CanRetryWithGrace(bool aFreshnessOnlyReason, bool aAlreadyHeld, double aNow, double aDeadline) noexcept
{
    return aFreshnessOnlyReason && !aAlreadyHeld && std::isfinite(aNow) && std::isfinite(aDeadline) && aNow > aDeadline && aNow <= aDeadline + kPresentationGraceMs;
}
// Reuse a completed guard assessment without replaying ring mutations or one-shot events. Original
// candidate angles/reason remain evidence of WHY effective inputs were selected; a held candidate's age
// and episode time continue from their original origins. Clean episode-end durations are not replayed.
inline bool ReuseFlipDecision(const FlipDecision& acCompleted, double aAssessedAt, double aNow, double aHeldOrigin, FlipDecision& aReused) noexcept
{
    if (!acCompleted.Checked || acCompleted.Release || !std::isfinite(aAssessedAt) || !std::isfinite(aNow) || aNow < aAssessedAt)
        return false;
    aReused = acCompleted;
    aReused.Reset = aReused.EpisodeStart = aReused.EpisodeEnd = aReused.TimeoutStart = false;
    aReused.Purged = 0;
    if (acCompleted.Hold)
    {
        const double age = aNow - aHeldOrigin;
        if (!std::isfinite(age) || age < 0. || age > kFlipHoldTimeoutMs)
            return false;
        aReused.HeldAgeMs = age;
        aReused.EpisodeMs = acCompleted.EpisodeMs + (aNow - aAssessedAt);
    }
    else
    {
        aReused.HeldAgeMs = aReused.EpisodeMs = 0.;
        aReused.HeldTick = 0;
    }
    return true;
}
// Freeze the actual body inputs after the plausibility guard, before current-baseline scale/solve passes.
// A partially rejected/invalid bone sample can commit under the older solver policy but cannot arm grace.
inline bool ValidGracePose(const FlipPose& acPose) noexcept
{
    if (acPose.Valid != (acPose.HasLegs ? kFlipAllMask : kFlipUpperMask) || !BodyPosePresentation::Finite(acPose.Hip))
        return false;
    for (size_t wire = 0; wire < acPose.Rotation.size(); ++wire)
    {
        if (!(acPose.Valid & (1u << wire)))
            continue;
        if (!BodyPosePresentation::Unit(acPose.Rotation[wire]))
            return false;
    }
    return true;
}

// V2 withdrawal admission, process-wide for the selected presentation rather than a pose-generation cache.
// Only actual presentation callbacks increment frame progress. No lifecycle reset operation is provided.
inline constexpr double kWithdrawalDwellMs = 750.0;
inline constexpr uint64_t kWithdrawalDwellPresentFrames = 45;
struct DwellObservation
{
    bool Waiting{}, Ready{}, Live{}, PresentedLive{}, ClockValid{}, StartValid{};
    double StartedMs{}, ElapsedMs{};
    uint64_t StartedPresentFrame{}, ElapsedPresentFrames{}, PresentFrame{}, Transition{};
};
struct PresentationDwell
{
    bool Live{}, PresentedLive{}, Waiting{}, StartValid{}, HaveObservation{}, ObservationValid{};
    double StartedMs{}, LastNowMs{};
    uint64_t StartedPresentFrame{}, LastPresentFrame{}, Transition{};
    bool Observe(double aNow, uint64_t aPresentedFrame) noexcept
    {
        const bool finiteNow = std::isfinite(aNow) && aNow >= 0.;
        ObservationValid = finiteNow && (!HaveObservation || (aNow >= LastNowMs && aPresentedFrame >= LastPresentFrame));
        if (!ObservationValid)
        {
            // Keep independent high-water marks even when the other observation is invalid. A bad
            // clock paired with a later counter cannot later re-anchor the dwell at an earlier frame.
            if (!HaveObservation || aPresentedFrame > LastPresentFrame)
                LastPresentFrame = aPresentedFrame;
            if (finiteNow && (!HaveObservation || aNow > LastNowMs))
                LastNowMs = aNow;
            HaveObservation = true;
            return false;
        }
        HaveObservation = true;
        LastNowMs = aNow;
        LastPresentFrame = aPresentedFrame;
        if (Waiting && !StartValid)
        {
            // An invalid transition clock cannot grant an early start. Anchor at the first later valid observation.
            StartedMs = aNow;
            StartedPresentFrame = aPresentedFrame;
            StartValid = true;
        }
        return true;
    }
    bool Ready() const noexcept
    {
        return Waiting && StartValid && ObservationValid && LastNowMs - StartedMs >= kWithdrawalDwellMs && LastPresentFrame - StartedPresentFrame >= kWithdrawalDwellPresentFrames;
    }
    void Presented(double aNow, uint64_t aPresentedFrame) noexcept
    {
        if (Observe(aNow, aPresentedFrame) && Live)
            PresentedLive = true;
    }
    bool Withdraw(double aNow, uint64_t aPresentedFrame) noexcept
    {
        const bool valid = Observe(aNow, aPresentedFrame);
        if (!Live)
            return false; // repeated refusals/lifecycle operations never restart a waiting interval
        Live = false;
        if (!PresentedLive)
            return false; // cold, never actually presented: no withdrawal to hide
        PresentedLive = false;
        Waiting = true;
        StartValid = valid;
        ++Transition;
        if (valid)
        {
            StartedMs = aNow;
            StartedPresentFrame = aPresentedFrame;
        }
        return true;
    }
    bool Allows(double aNow, uint64_t aPresentedFrame, bool aFreshEligible, bool aCommittedHoldEligible) noexcept
    {
        if (!Observe(aNow, aPresentedFrame))
            return false;
        if (Waiting)
            return Ready() && aFreshEligible; // elapsed dwell grants no stale or ownership entitlement
        return aFreshEligible || (Live && aCommittedHoldEligible);
    }
    bool Commit(double aNow, uint64_t aPresentedFrame, bool aFreshEligible, bool aCommittedHoldEligible, uint64_t aExpectedTransition) noexcept
    {
        if (aExpectedTransition != Transition || !Allows(aNow, aPresentedFrame, aFreshEligible, aCommittedHoldEligible))
            return false;
        if (!Live)
            PresentedLive = false;
        Live = true;
        Waiting = false;
        return true;
    }
    DwellObservation Snapshot() const noexcept
    {
        return {
            Waiting,
            Ready(),
            Live,
            PresentedLive,
            ObservationValid,
            StartValid,
            StartedMs,
            StartValid && ObservationValid ? LastNowMs - StartedMs : 0.,
            StartedPresentFrame,
            StartValid && ObservationValid ? LastPresentFrame - StartedPresentFrame : 0,
            LastPresentFrame,
            Transition};
    }
};

} // namespace BodyPoseCommit
