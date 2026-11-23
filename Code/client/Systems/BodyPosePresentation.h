#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Value-only candidate. It neither reads nor writes engine objects or skinning buffers.
namespace BodyPosePresentation
{
inline constexpr size_t kMaxNodes = 512;
inline constexpr size_t kWireBones = 19;
inline constexpr size_t kUpperBones = 12;
inline constexpr size_t kPelvis = 12;
inline constexpr int kMissing = -1;

struct Transform
{
    glm::quat Rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 Translation{};
    float Scale = 1.f;
};
struct Input
{
    size_t Count = 0;
    std::array<Transform, kMaxNodes> Baseline{};
    // -1 means no parent in this captured hierarchy. Other indices must name an existing node.
    std::array<int, kMaxNodes> Parent{};
    std::array<int, kWireBones> Bone{};
    std::array<glm::quat, kWireBones> Rotations{};
    glm::quat RootRotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 RootTranslation{};
    glm::vec3 HipOffset{};
    bool HasData = false;
    bool NoBones = false;
    bool HasLegs = false;
    bool HasHips = false;
    Input() noexcept
    {
        Parent.fill(kMissing);
        Bone.fill(kMissing);
        Rotations.fill(glm::quat{1.f, 0.f, 0.f, 0.f});
    }
};
struct Output
{
    size_t Count = 0;
    std::array<Transform, kMaxNodes> world{};
};
inline bool Finite(const glm::vec3& acValue) noexcept
{
    return std::isfinite(acValue.x) && std::isfinite(acValue.y) && std::isfinite(acValue.z);
}
inline bool Unit(const glm::quat& acQuaternion) noexcept
{
    if (!std::isfinite(acQuaternion.w) || !std::isfinite(acQuaternion.x) || !std::isfinite(acQuaternion.y) || !std::isfinite(acQuaternion.z))
        return false;
    return std::fabs(glm::dot(acQuaternion, acQuaternion) - 1.f) <= 0.002f;
}

// On failure output is untouched. Parents may be unsorted; a stable topological pass uses the actual hierarchy.
// The optional mask excludes entries such as the actor's own 3D root from the whole-body hip delta.
inline bool Solve(const Input& acInput, Output& aOutput, const std::array<bool, kMaxNodes>* apHipExcluded = nullptr) noexcept
{
    if (acInput.Count > kMaxNodes || !Unit(acInput.RootRotation) || !Finite(acInput.RootTranslation))
        return false;
    if (acInput.HasHips && !Finite(acInput.HipOffset))
        return false;
    for (size_t i = 0; i < acInput.Count; ++i)
    {
        const auto& t = acInput.Baseline[i];
        if (!Unit(t.Rotation) || !Finite(t.Translation) || !std::isfinite(t.Scale) || t.Scale <= 0.f || acInput.Parent[i] < kMissing ||
            acInput.Parent[i] >= static_cast<int>(acInput.Count))
            return false;
    }
    std::array<int, kMaxNodes> wireAt{};
    wireAt.fill(kMissing);
    for (size_t wire = 0; wire < kWireBones; ++wire)
    {
        const int node = acInput.Bone[wire];
        if (node < kMissing || node >= static_cast<int>(acInput.Count))
            return false;
        if (node != kMissing)
        {
            if (wireAt[node] != kMissing)
                return false;
            wireAt[node] = static_cast<int>(wire);
            if (acInput.HasData && !acInput.NoBones && (wire < kUpperBones || acInput.HasLegs) && !Unit(acInput.Rotations[wire]))
                return false;
        }
    }
    std::array<bool, kMaxNodes> done{};
    std::array<size_t, kMaxNodes> ordered{};
    for (size_t n = 0; n < acInput.Count; ++n)
    {
        size_t selected = acInput.Count;
        for (size_t i = 0; i < acInput.Count; ++i)
            if (!done[i] && (acInput.Parent[i] == kMissing || done[static_cast<size_t>(acInput.Parent[i])]))
            {
                selected = i;
                break;
            }
        if (selected == acInput.Count)
            return false;
        done[selected] = true;
        ordered[n] = selected;
    }

    Output result{};
    result.Count = acInput.Count;
    for (size_t i = 0; i < acInput.Count; ++i)
        result.world[i] = acInput.Baseline[i];
    if (acInput.HasData && !acInput.NoBones)
    {
        if (acInput.HasHips && acInput.Bone[kPelvis] != kMissing)
        {
            glm::vec3 delta = acInput.RootTranslation + acInput.RootRotation * acInput.HipOffset - result.world[acInput.Bone[kPelvis]].Translation;
            if (!Finite(delta))
                return false;
            if (!acInput.HasLegs)
            {
                glm::vec3 local = glm::inverse(acInput.RootRotation) * delta;
                local.z = 0.f;
                delta = acInput.RootRotation * local;
            }
            const float length = glm::length(delta);
            if (!std::isfinite(length))
                return false;
            if (length > 256.f)
                delta *= 256.f / length;
            for (size_t i = 0; i < acInput.Count; ++i)
                if (!apHipExcluded || !(*apHipExcluded)[i])
                    result.world[i].Translation += delta;
        }
        for (size_t n = 0; n < acInput.Count; ++n)
        {
            const size_t node = ordered[n];
            const int wire = wireAt[node];
            if (wire == kMissing || (wire >= static_cast<int>(kUpperBones) && !acInput.HasLegs))
                continue;
            const glm::quat target = acInput.RootRotation * acInput.Rotations[wire];
            const glm::quat delta = target * glm::inverse(result.world[node].Rotation);
            const glm::vec3 pivot = result.world[node].Translation;
            result.world[node].Rotation = target;
            for (size_t child = 0; child < acInput.Count; ++child)
            {
                int parent = acInput.Parent[child];
                while (parent != kMissing && parent != static_cast<int>(node))
                    parent = acInput.Parent[parent];
                if (parent == kMissing)
                    continue;
                auto& t = result.world[child];
                t.Rotation = delta * t.Rotation;
                t.Translation = pivot + delta * (t.Translation - pivot);
            }
        }
    }
    // Reject numerical overflow before committing any values.
    for (size_t i = 0; i < acInput.Count; ++i)
        if (!Unit(result.world[i].Rotation) || !Finite(result.world[i].Translation))
            return false;
    aOutput = result;
    return true;
}

// Shape passes on the solver's values (SE body commit): body size as world values (owner decision D4: nothing outlives
// one pass), hand fine-tuning (a value port of upstream VRBodySync ReachHands / TurnBone / RotationBetween) and finger
// worlds (VRBodySync WriteFingerRotations without its local write). Value-only, like Solve.

// Body size. ratio = sender skeleton-root world scale / this copy's skeleton-root world scale. Within the dead band
// nothing is scaled (upstream leaves a local scale within 0.003 alone); outside [kScaleMin, kScaleMax] it is clamped.
inline constexpr float kScaleDeadBand = 0.003f, kScaleMin = 0.5f, kScaleMax = 1.5f;
struct ScaleDecision
{
    float Ratio = 1.f;
    bool Applied{}, DeadBand{}, Clamped{}, Invalid{};
};
inline ScaleDecision RootScaleRatio(float aSender, float aNative) noexcept
{
    ScaleDecision d{};
    if (!std::isfinite(aSender) || !std::isfinite(aNative) || !(aSender > 0.f) || !(aNative > 0.f))
    {
        d.Invalid = true;
        return d;
    }
    float ratio = aSender / aNative;
    if (!std::isfinite(ratio))
    {
        d.Invalid = true;
        return d;
    }
    if (std::fabs(ratio - 1.f) <= kScaleDeadBand)
    {
        d.DeadBand = true;
        return d;
    }
    if (ratio < kScaleMin)
    {
        ratio = kScaleMin;
        d.Clamped = true;
    }
    else if (ratio > kScaleMax)
    {
        ratio = kScaleMax;
        d.Clamped = true;
    }
    d.Ratio = ratio;
    d.Applied = true;
    return d;
}
// Scales every marked baseline entry about pivot (the skeleton root's world position): translation pivot + ratio x
// (t - pivot), scale x ratio as one float multiply (a caller can reproduce the bits). Rotations and unmarked entries are
// unchanged. Run before Solve, so the hip delta is taken against the scaled pelvis and the legs swing at the sender's
// length. False (input untouched) on a non-finite pivot or a ratio outside the clamp.
template <size_t N> bool ApplyRootScale(Input& aInput, const std::array<bool, N>& acUnder, const glm::vec3& acPivot, float aRatio) noexcept
{
    if (!Finite(acPivot) || !std::isfinite(aRatio) || aRatio < kScaleMin || aRatio > kScaleMax || aInput.Count > N || aInput.Count > kMaxNodes)
        return false;
    for (size_t i = 0; i < aInput.Count; ++i)
    {
        if (!acUnder[i])
            continue;
        auto& t = aInput.Baseline[i];
        t.Translation = acPivot + aRatio * (t.Translation - acPivot);
        t.Scale = t.Scale * aRatio;
    }
    return true;
}
// A world position in the actor root's space (root rotation removed, no scale division), as the sender's hip and hand
// offsets are expressed (VRBodySync capture: inverse(root rotation) x (p - root position)).
inline glm::vec3 RootSpace(const glm::quat& acRootRotation, const glm::vec3& acRootTranslation, const glm::vec3& acWorld) noexcept
{
    return glm::inverse(acRootRotation) * (acWorld - acRootTranslation);
}
// True when node hangs (at any depth) below ancestor in the captured hierarchy.
inline bool Below(const std::array<int, kMaxNodes>& acParent, size_t aCount, size_t aNode, int aAncestor) noexcept
{
    if (aNode >= aCount || aAncestor < 0)
        return false;
    int p = acParent[aNode];
    for (size_t depth = 0; p != kMissing && depth <= aCount; ++depth)
    {
        if (p == aAncestor)
            return true;
        if (p < 0 || static_cast<size_t>(p) >= aCount)
            return false;
        p = acParent[static_cast<size_t>(p)];
    }
    return false;
}
// Turns one bone about its own joint by a world rotation and carries everything below it along (VRBodySync TurnBone).
inline void TurnBone(Output& aOutput, const std::array<int, kMaxNodes>& acParent, int aBone, const glm::quat& acDelta) noexcept
{
    if (aBone < 0 || static_cast<size_t>(aBone) >= aOutput.Count)
        return;
    const glm::vec3 pivot = aOutput.world[static_cast<size_t>(aBone)].Translation;
    aOutput.world[static_cast<size_t>(aBone)].Rotation = glm::normalize(acDelta * aOutput.world[static_cast<size_t>(aBone)].Rotation);
    for (size_t child = 0; child < aOutput.Count; ++child)
    {
        if (!Below(acParent, aOutput.Count, child, aBone))
            continue;
        auto& t = aOutput.world[child];
        t.Rotation = glm::normalize(acDelta * t.Rotation);
        t.Translation = pivot + acDelta * (t.Translation - pivot);
    }
}
// The rotation that turns unit direction from onto unit direction to (VRBodySync RotationBetween; aligned or exactly
// opposite: none, an arm never needs a half turn in one frame).
inline glm::quat RotationBetween(const glm::vec3& acFrom, const glm::vec3& acDestination) noexcept
{
    const glm::vec3 axis = glm::cross(acFrom, acDestination);
    const float sine = glm::length(axis);
    const float cosine = glm::clamp(glm::dot(acFrom, acDestination), -1.f, 1.f);
    if (!(sine >= 1e-5f))
        return glm::quat{1.f, 0.f, 0.f, 0.f};
    return glm::angleAxis(std::atan2(sine, cosine), axis / sine);
}
// Hand fine-tuning (VRBodySync ReachHands, one arm): shoulder and elbow are bent so the hand lands on target (two-bone IK,
// the elbow kept on the side it already bends to, a straight arm bending downwards), then the hand gets handWanted (the
// sender's own hand orientation) back. Fine-tuning only: a hand more than maxCorrection from target is left as the
// rotations put it (GapCap). GapBefore / ErrorBefore (copy hand minus target, root space) are the measure BEFORE the
// correction: the hand-error gate (the after values only show that the correction works).
inline constexpr float kReachMaxCorrection = 15.f;
enum class Reach : uint8_t
{
    NotRun,
    Applied,
    GapCap,
    BadRead,
    Degenerate,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(Reach::Count)> kReachNames{"not_run", "applied", "gap_cap", "bad_read", "degenerate"};
struct ReachResult
{
    Reach Outcome = Reach::NotRun;
    float GapBefore = -1.f, GapAfter = -1.f;
    glm::vec3 ErrorBefore{}, ErrorAfter{};
};
inline ReachResult ReachHand(
    Output& aOutput, const std::array<int, kMaxNodes>& acParent, const glm::quat& acRootRotation, int aUpper, int aFore, int aHand, const glm::vec3& acTarget,
    const glm::quat& acHandWanted, float aMaxCorrection = kReachMaxCorrection) noexcept
{
    ReachResult r{};
    const auto valid = [&aOutput](int aNode) noexcept
    {
        return aNode >= 0 && static_cast<size_t>(aNode) < aOutput.Count;
    };
    if (!valid(aUpper) || !valid(aFore) || !valid(aHand) || aUpper == aFore || aFore == aHand || aUpper == aHand || !Finite(acTarget) || !Unit(acHandWanted) ||
        !Unit(acRootRotation) || !Below(acParent, aOutput.Count, static_cast<size_t>(aFore), aUpper) || !Below(acParent, aOutput.Count, static_cast<size_t>(aHand), aFore))
    {
        r.Outcome = Reach::BadRead;
        return r;
    }
    const auto at = [&aOutput](int aNode) noexcept
    {
        return aOutput.world[static_cast<size_t>(aNode)].Translation;
    };
    const glm::quat inverseRoot = glm::inverse(acRootRotation);
    const glm::vec3 shoulder = at(aUpper), elbow = at(aFore), wrist = at(aHand);
    r.ErrorBefore = inverseRoot * (wrist - acTarget);
    const float gap = glm::distance(acTarget, wrist);
    r.GapBefore = gap;
    const auto finish = [&](Reach aOutcome) noexcept
    {
        r.Outcome = aOutcome;
        r.ErrorAfter = inverseRoot * (at(aHand) - acTarget);
        r.GapAfter = glm::distance(acTarget, at(aHand));
        return r;
    };
    if (!std::isfinite(gap))
    {
        r.Outcome = Reach::BadRead;
        return r;
    }
    if (gap > aMaxCorrection)
        return finish(Reach::GapCap);
    const float upperLength = glm::distance(shoulder, elbow);
    const float foreLength = glm::distance(elbow, wrist);
    const glm::vec3 toTarget = acTarget - shoulder;
    const float reach = glm::length(toTarget);
    // A bad read, not a hand: further from the shoulder than two arm lengths, or not a number at all.
    if (!std::isfinite(reach) || upperLength < 1.f || foreLength < 1.f || reach < 1.f || reach > 2.f * (upperLength + foreLength))
        return finish(Reach::BadRead);
    const float clamped = glm::clamp(reach, std::fabs(upperLength - foreLength) + 0.5f, upperLength + foreLength - 0.5f);
    const glm::vec3 along = toTarget / reach;
    // The plane the arm bends in, from how it bends now; a straight arm bends downwards, as an elbow does.
    glm::vec3 normal = glm::cross(elbow - shoulder, wrist - shoulder);
    if (glm::dot(normal, normal) < 1e-4f)
        normal = glm::cross(along, acRootRotation * glm::vec3(0.f, 0.f, -1.f));
    if (glm::dot(normal, normal) < 1e-6f)
        return finish(Reach::Degenerate);
    glm::vec3 side = glm::normalize(glm::cross(glm::normalize(normal), along));
    if (glm::dot(elbow - shoulder, side) < 0.f)
        side = -side;
    const float cosine = glm::clamp((upperLength * upperLength + clamped * clamped - foreLength * foreLength) / (2.f * upperLength * clamped), -1.f, 1.f);
    const float sine = std::sqrt(std::max(0.f, 1.f - cosine * cosine));
    const glm::vec3 elbowWanted = shoulder + upperLength * (cosine * along + sine * side);
    TurnBone(aOutput, acParent, aUpper, RotationBetween(glm::normalize(elbow - shoulder), glm::normalize(elbowWanted - shoulder)));
    const glm::vec3 elbowNow = at(aFore), wristNow = at(aHand);
    const glm::vec3 wristWanted = shoulder + clamped * along;
    // As upstream: the shoulder turn above stays when the second turn is degenerate.
    if (glm::distance(wristNow, elbowNow) < 1.f || glm::distance(wristWanted, elbowNow) < 1.f)
        return finish(Reach::Degenerate);
    TurnBone(aOutput, acParent, aFore, RotationBetween(glm::normalize(wristNow - elbowNow), glm::normalize(wristWanted - elbowNow)));
    // The sender's own hand orientation, which the two turns above carried away.
    TurnBone(aOutput, acParent, aHand, glm::normalize(acHandWanted * glm::inverse(aOutput.world[static_cast<size_t>(aHand)].Rotation)));
    return finish(Reach::Applied);
}
// Finger worlds below an already posed hand (VRBodySync WriteFingerRotations semantics, worlds only): each bone's world
// rotation is its parent's times the sent parent-relative rotation, its translation the parent's plus the parent's
// rotation applied to (its own native local translation x the parent's world scale); its own world scale is kept.
// entries: five fingers of three bones, thumb first, parent first. False (out untouched) on a bad index, a non-unit
// rotation or a non-finite result.
inline constexpr size_t kFingerChains = 5, kFingerChainBones = 3, kFingerBonesPerHand = kFingerChains * kFingerChainBones;
inline bool ApplyFingers(
    Output& aOutput, int aHand, const std::array<int, kFingerBonesPerHand>& acEntries, const std::array<glm::vec3, kFingerBonesPerHand>& acLocal,
    const std::array<glm::quat, kFingerBonesPerHand>& acRotations) noexcept
{
    if (aHand < 0 || static_cast<size_t>(aHand) >= aOutput.Count)
        return false;
    for (size_t k = 0; k < kFingerBonesPerHand; ++k)
        if (acEntries[k] < 0 || static_cast<size_t>(acEntries[k]) >= aOutput.Count || acEntries[k] == aHand || !Finite(acLocal[k]) || !Unit(acRotations[k]))
            return false;
    std::array<Transform, kFingerBonesPerHand> result{};
    for (size_t f = 0; f < kFingerChains; ++f)
    {
        Transform parent = aOutput.world[static_cast<size_t>(aHand)];
        for (size_t k = 0; k < kFingerChainBones; ++k)
        {
            const size_t at = f * kFingerChainBones + k;
            Transform world = aOutput.world[static_cast<size_t>(acEntries[at])];
            world.Rotation = glm::normalize(parent.Rotation * glm::normalize(acRotations[at]));
            world.Translation = parent.Translation + parent.Rotation * (acLocal[at] * parent.Scale);
            if (!Unit(world.Rotation) || !Finite(world.Translation))
                return false;
            result[at] = world;
            parent = world;
        }
    }
    for (size_t k = 0; k < kFingerBonesPerHand; ++k)
        aOutput.world[static_cast<size_t>(acEntries[k])] = result[k];
    return true;
}
// The same overflow check Solve makes before it commits values, for the output after the shape passes.
inline bool ValidOutput(const Output& acOutput) noexcept
{
    if (acOutput.Count > kMaxNodes)
        return false;
    for (size_t i = 0; i < acOutput.Count; ++i)
        if (!Unit(acOutput.world[i].Rotation) || !Finite(acOutput.world[i].Translation) || !std::isfinite(acOutput.world[i].Scale) || !(acOutput.world[i].Scale > 0.f))
            return false;
    return true;
}
} // namespace BodyPosePresentation
