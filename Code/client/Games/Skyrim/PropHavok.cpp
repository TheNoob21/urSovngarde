#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/PropHavok.h>
#include <Games/Skyrim/HavokLayout.h>
#include <Games/Skyrim/RagdollBodiesPose.h>

#include <TESObjectREFR.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>
#include <Services/CopyWorkGate.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace
{
using namespace HavokLayout;

auto Retain(NiNode* apRoot) noexcept
{
    const auto cRelease = [](NiNode* apNode)
    {
        apNode->DecRef();
    };
    if (apRoot)
        apRoot->IncRef();
    return std::unique_ptr<NiNode, decltype(cRelease)>{apRoot, cRelease};
}

// Havok units per game unit. Skyrim's is 0.0142875; anything far off is a wrong address, and nothing is read with it.
float HavokScale() noexcept
{
    const float cScale = HavokWorldScale();
    return cScale > 0.01f && cScale < 0.02f ? cScale : 0.f;
}

// BhkWorldOf, remembered for the last world, since its lock is taken every frame a swing is driven.
void* CheckedBhkWorldOf(void* apHkpWorld) noexcept
{
    static void* s_checkedFrom = nullptr;
    static void* s_checked = nullptr;
    void* pWorld = apHkpWorld ? At<void*>(apHkpWorld, kBhkWorldOfHkpWorldOffset) : nullptr;
    if (!pWorld)
        return nullptr;
    if (apHkpWorld == s_checkedFrom && pWorld == s_checked)
        return pWorld;
    if (!BhkWorldOf(apHkpWorld))
        return nullptr;
    s_checkedFrom = apHkpWorld;
    s_checked = pWorld;
    return pWorld;
}

struct Body
{
    uint8_t* pHavok = nullptr;
};

struct Root
{
    glm::mat3 Turn{1.f};
    glm::vec3 At{};
    float Scale = 1.f;
};

Root RootOf(void* apNode) noexcept
{
    const NiTransform& world = At<NiTransform>(apNode, kWorldOffset);
    Root root{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            root.Turn[column][row] = world.rotate.data[row][column];
    root.At = {world.translate.x, world.translate.y, world.translate.z};
    root.Scale = world.scale;
    return root;
}

// The dynamic bodies below a root, all in one Havok world, which is returned; null when there are none or too many.
void* Collect(void* apRoot, std::vector<Body>& aBodies, uint32_t& aLayout) noexcept
{
    aBodies.clear();
    void* pHkpWorld = nullptr;
    RagdollBodiesPose::Layout layout;
    std::array<std::string_view, PropBodyState::kMaxBodies> names;
    bool fits = true;
    NiTree::VisitBelow(
        apRoot, 512,
        [&](void* apObject)
        {
            uint8_t* pBody = HavokBodyOfLive(apObject);
            if (!pBody)
                return true;
            void* pWorld = At<void*>(pBody, kHkWorldOffset);
            if (!pWorld || !WorldObjectBodies::IsDynamicMotion(pBody[kMotionTypeOffset]))
                return true;
            if ((pHkpWorld && pWorld != pHkpWorld) || aBodies.size() >= PropBodyState::kMaxBodies)
            {
                fits = false;
                return false;
            }
            constexpr uint32_t cNameOffset = 0x10;
            const char* pName = At<const char*>(apObject, cNameOffset);
            const auto cNext = names.begin() + aBodies.size();
            if (!pName || !*pName || std::find(names.begin(), cNext, pName) != cNext)
            {
                fits = false;
                return false;
            }
            *cNext = pName;
            pHkpWorld = pWorld;
            aBodies.push_back(Body{pBody});
            layout.Add(pName);
            return true;
        });
    aLayout = layout.Key();
    return fits && !aBodies.empty() ? pHkpWorld : nullptr;
}

glm::vec3 PositionOf(const uint8_t* apHavok, const float aScale) noexcept
{
    const auto* pAt = reinterpret_cast<const float*>(apHavok + kBodyTranslationOffset);
    return glm::vec3{pAt[0], pAt[1], pAt[2]} / aScale;
}

glm::quat TurnOf(const uint8_t* apHavok) noexcept
{
    const auto* pQuaternion = reinterpret_cast<const float*>(apHavok + kBodyQuaternionOffset);
    return glm::quat{pQuaternion[3], pQuaternion[0], pQuaternion[1], pQuaternion[2]};
}

bool IsFinite(const glm::vec3& acValue) noexcept
{
    return std::isfinite(acValue.x) && std::isfinite(acValue.y) && std::isfinite(acValue.z);
}

// Fixed bodies (hkpMotion 5) never swing; a keyframed one may be dynamic again later (carried, a trap not yet sprung).
int32_t CountMovable(void* apRoot, bool& aAnyCollision, bool& aAnyInWorld) noexcept
{
    int32_t count = 0;
    std::vector<void*> queue;
    queue.push_back(apRoot);
    for (size_t head = 0; head < queue.size() && head < 512; ++head)
    {
        void* pObject = queue[head];
        if (!IsReadable(static_cast<uint8_t*>(pObject) + kCollisionObjectOffset, sizeof(void*)))
            continue;
        if (At<void*>(pObject, kCollisionObjectOffset))
            aAnyCollision = true;
        if (uint8_t* pBody = HavokBodyOf(pObject, kAngularVelocityOffset + 16); pBody && At<void*>(pBody, kHkWorldOffset))
        {
            aAnyInWorld = true;
            const uint8_t cMotion = pBody[kMotionTypeOffset];
            if (head > 0 && cMotion != 0 && cMotion != 5)
                ++count;
        }
        void* pNode = AsNode(pObject);
        const NiTree::ChildSlots cChildren = pNode ? NiTree::ChildSlotsOf(pNode) : NiTree::ChildSlots{};
        if (cChildren.pSlots && cChildren.Count && !IsReadable(cChildren.pSlots, cChildren.Count * sizeof(void*)))
            continue;
        for (uint16_t i = 0; cChildren.pSlots && i < cChildren.Count; ++i)
            if (cChildren.pSlots[i])
                queue.push_back(cChildren.pSlots[i]);
    }
    return count;
}

// Flat: the drives the client's update decided on its job thread, made from the main loop (RunQueued).
std::mutex s_queueLock;
std::vector<std::pair<uint32_t, WorldObjectBodies::Driven>> s_queued;
std::chrono::steady_clock::time_point s_queuedAt{};

using Clock = std::chrono::steady_clock;
constexpr auto kSampleLifetime = WorldObjectBodies::kPoseSampleFor;
constexpr auto kProbeLifetime = WorldObjectBodies::kProbeSampleFor;
constexpr size_t kMaxSamples = 160;

struct Snapshot
{
    uintptr_t Root{};
    const TESObjectCELL* pCell{};
    Clock::time_point At{};
    std::optional<int32_t> Count;
    WorldObjectBodies::Poses Poses;
    uint32_t Layout{};

    Clock::duration Lifetime() const noexcept { return Count ? kProbeLifetime : kSampleLifetime; }
    bool Fresh(const Clock::time_point aNow) const noexcept { return aNow - At < Lifetime(); }
};

// Thrown objects to give a speed once their bodies are dynamic (Launch), at most kMaxLaunches, each tried for kLaunchFor.
struct LaunchRequest
{
    uint32_t FormId{};
    glm::vec3 Velocity{};
    glm::vec3 Spin{};
    Clock::time_point Since{};
};
constexpr size_t kMaxLaunches = 32;
constexpr auto kLaunchFor = std::chrono::milliseconds(250);
std::vector<LaunchRequest> s_launches;

// Under s_queueLock.
void DropLaunch(const uint32_t aFormId) noexcept
{
    std::erase_if(s_launches, [aFormId](const LaunchRequest& acLaunch) { return acLaunch.FormId == aFormId; });
}

// The dynamic bodies of a thrown object, its root's own among them, all in one Havok world, which is returned; null when there
// are none or too many (main loop).
void* CollectFlying(void* apRoot, std::vector<uint8_t*>& aBodies) noexcept
{
    aBodies.clear();
    void* pHkpWorld = nullptr;
    bool fits = true;
    NiTree::VisitTree(
        apRoot, 512,
        [&](void* apObject)
        {
            uint8_t* pBody = HavokBodyOfLive(apObject);
            if (!pBody)
                return true;
            void* pWorld = At<void*>(pBody, kHkWorldOffset);
            if (!pWorld || !WorldObjectBodies::IsDynamicMotion(pBody[kMotionTypeOffset]))
                return true;
            if ((pHkpWorld && pWorld != pHkpWorld) || aBodies.size() >= PropBodyState::kMaxBodies)
            {
                fits = false;
                return false;
            }
            pHkpWorld = pWorld;
            aBodies.push_back(pBody);
            return true;
        });
    return fits && !aBodies.empty() ? pHkpWorld : nullptr;
}

// Sets the thrown object's bodies moving, under the world's write lock. False while none is dynamic yet.
bool LaunchNow(TESObjectREFR* apRef, const glm::vec3& acVelocity, const glm::vec3& acSpin) noexcept
{
    auto root = Retain(apRef->GetNiNode());
    const float cScale = HavokScale();
    std::vector<uint8_t*> bodies;
    void* pHkpWorld = root && cScale > 0.f ? CollectFlying(root.get(), bodies) : nullptr;
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, kHkpEntityActivateAeId, 60096);
    if (!pHkpWorld || !s_activate.Get())
        return false;
    WorldWriteLock lock(CheckedBhkWorldOf(pHkpWorld));
    if (!lock.Held())
        return false;
    const glm::vec3 cVelocity = acVelocity * cScale;
    const glm::vec3 cSpin = acSpin;
    if (!IsFinite(cVelocity) || !IsFinite(cSpin))
        return true;
    for (uint8_t* pHavok : bodies)
    {
        if (At<void*>(pHavok, kHkWorldOffset) != pHkpWorld || !WorldObjectBodies::IsDynamicMotion(pHavok[kMotionTypeOffset]))
            continue;
        s_activate.Get()(pHavok);
        std::memset(pHavok + kLinearVelocityOffset, 0, 16);
        std::memcpy(pHavok + kLinearVelocityOffset, &cVelocity, sizeof(float) * 3);
        std::memset(pHavok + kAngularVelocityOffset, 0, 16);
        std::memcpy(pHavok + kAngularVelocityOffset, &cSpin, sizeof(float) * 3);
    }
    return true;
}

std::unordered_map<uint32_t, bool> s_sampleRequests;
std::unordered_map<uint32_t, Snapshot> s_samples;
uint64_t s_sampleGeneration{};

const Snapshot* RequestSample(TESObjectREFR* apRef, const bool aProbe) noexcept
{
    const auto cFormId = apRef->formID;
    auto request = s_sampleRequests.find(cFormId);
    if (request != s_sampleRequests.end())
        request->second = request->second && aProbe;
    else if (s_sampleRequests.size() < kMaxSamples)
        s_sampleRequests.emplace(cFormId, aProbe);

    const auto it = s_samples.find(cFormId);
    if (it == s_samples.end() || !it->second.Fresh(Clock::now()) || it->second.Root != reinterpret_cast<uintptr_t>(apRef->GetNiNode()) ||
        it->second.pCell != apRef->GetParentCellEx())
        return nullptr;
    return &it->second;
}

bool ReadNow(void* apRoot, WorldObjectBodies::Poses& aOut, uint32_t& aLayout) noexcept
{
    const float cScale = HavokScale();
    std::vector<Body> bodies;
    if (!(cScale > 0.f))
        return false;
    void* pWorld = Collect(apRoot, bodies, aLayout);
    WorldWriteLock lock(CheckedBhkWorldOf(pWorld));
    if (!lock.Held())
        return false;
    const Root cRoot = RootOf(apRoot);
    for (const Body& body : bodies)
    {
        if (At<void*>(body.pHavok, kHkWorldOffset) != pWorld)
            return false;
        const WorldObjectBodies::Pose cPose = WorldObjectBodies::ToRoot(cRoot.Turn, cRoot.At, cRoot.Scale, PositionOf(body.pHavok, cScale), TurnOf(body.pHavok));
        if (!IsFinite(cPose.Position) || !std::isfinite(cPose.Rotation.w) || glm::length(cPose.Position) > PropBodyState::kMaxDistance)
            return false;
        aOut.push_back(cPose);
    }
    return true;
}

int32_t CountNow(void* apRoot) noexcept
{
    bool anyCollision = false;
    bool anyInWorld = false;
    const int32_t cCount = CountMovable(apRoot, anyCollision, anyInWorld);
    // No collision at all (a fire, a light beam) never hangs anything.
    if (!anyCollision)
        return 0;
    if (!anyInWorld)
        return -1;
    return cCount > static_cast<int32_t>(PropBodyState::kMaxBodies) ? 0 : cCount;
}
} // namespace

namespace PropHavok
{
WorldObjectBodies::Sample Read(TESObjectREFR* apRef, WorldObjectBodies::Poses& aOut, uint32_t& aLayout) noexcept
{
    aOut.clear();
    if (!apRef)
        return WorldObjectBodies::Sample::Empty;
    std::scoped_lock lock(s_queueLock);
    // A probe's sample counts the bodies but holds no poses: the poses are read on the next frame.
    const auto* pSample = RequestSample(apRef, false);
    if (!pSample || pSample->Count)
        return WorldObjectBodies::Sample::Waiting;
    aLayout = pSample->Layout;
    aOut = pSample->Poses;
    return aOut.empty() ? WorldObjectBodies::Sample::Empty : WorldObjectBodies::Sample::Read;
}

std::optional<int32_t> CountBodies(TESObjectREFR* apRef) noexcept
{
    if (!apRef)
        return -1;
    std::scoped_lock lock(s_queueLock);
    const auto* pSample = RequestSample(apRef, true);
    return pSample ? pSample->Count : std::nullopt;
}

void ClearSamples() noexcept
{
    std::scoped_lock lock(s_queueLock);
    s_sampleRequests.clear();
    s_samples.clear();
    s_queued.clear();
    s_launches.clear();
    ++s_sampleGeneration;
}

bool ReadFlight(TESObjectREFR* apRef, glm::vec3& aVelocity, glm::vec3& aSpin) noexcept
{
    const float cScale = HavokScale();
    NiNode* pRoot = apRef ? apRef->GetNiNode() : nullptr;
    const uint8_t* pBody = pRoot && cScale > 0.f ? HavokBodyOf(pRoot, kAngularVelocityOffset + 16) : nullptr;
    if (!pBody || !At<void*>(const_cast<uint8_t*>(pBody), kHkWorldOffset) || !WorldObjectBodies::IsDynamicMotion(pBody[kMotionTypeOffset]))
        return false;
    // Each copied out as the four floats Havok keeps it in, so the three used come from one read.
    alignas(16) float velocity[4];
    alignas(16) float spin[4];
    std::memcpy(velocity, pBody + kLinearVelocityOffset, sizeof(velocity));
    std::memcpy(spin, pBody + kAngularVelocityOffset, sizeof(spin));
    aVelocity = glm::vec3{velocity[0], velocity[1], velocity[2]} / cScale;
    aSpin = {spin[0], spin[1], spin[2]};
    return IsFinite(aVelocity) && IsFinite(aSpin);
}

void Launch(const uint32_t aFormId, const glm::vec3& acVelocity, const glm::vec3& acSpin) noexcept
{
    std::scoped_lock lock(s_queueLock);
    DropLaunch(aFormId);
    if (s_launches.size() < kMaxLaunches)
        s_launches.push_back({aFormId, acVelocity, acSpin, Clock::now()});
}

void CancelLaunch(const uint32_t aFormId) noexcept
{
    std::scoped_lock lock(s_queueLock);
    DropLaunch(aFormId);
}

void Queue(std::vector<std::pair<uint32_t, WorldObjectBodies::Driven>> aDrives) noexcept
{
    std::scoped_lock lock(s_queueLock);
    s_queued = std::move(aDrives);
    s_queuedAt = std::chrono::steady_clock::now();
}

void RunQueued() noexcept
{
    static std::chrono::steady_clock::time_point s_lastRun{};
    const auto cNow = std::chrono::steady_clock::now();
    const Clock::duration cFrame = std::min<Clock::duration>(cNow - s_lastRun, std::chrono::milliseconds(250));
    const float cFrameSeconds = std::chrono::duration<float>(cFrame).count();
    s_lastRun = cNow;
    if (IsProcessExiting() || LoadingScreenUp() || UI::LoadingScreensQueued() != 0)
    {
        ClearSamples();
        return;
    }

    // Kept from frame to frame, so their room is not made again each time (main loop only).
    static std::unordered_map<uint32_t, bool> s_requests;
    static std::unordered_map<uint32_t, Snapshot> s_read;
    // The requests whose sample of that kind is kept this frame, with the 3D and cell it was read from.
    static std::unordered_map<uint32_t, std::pair<uintptr_t, const TESObjectCELL*>> s_serving;
    uint64_t generation;
    {
        std::scoped_lock lock(s_queueLock);
        s_requests.clear();
        s_requests.swap(s_sampleRequests);
        generation = s_sampleGeneration;
        s_serving.clear();
        for (const auto& [formId, probe] : s_requests)
            if (const auto it = s_samples.find(formId);
                it != s_samples.end() && it->second.Count.has_value() == probe && WorldObjectBodies::KeepsSample(it->second.At, probe, cNow, cFrame))
                s_serving.emplace(formId, std::make_pair(it->second.Root, it->second.pCell));
    }
    s_read.clear();
    for (const auto& [formId, probe] : s_requests)
    {
        auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (!pRef || pRef->IsDeleted() || pRef->IsDisabled() || pRef->IsStillLoading())
            continue;
        // Still good for the same 3D in the same cell: read again once it would not be. A new request has no sample, and is read now.
        if (const auto serving = s_serving.find(formId);
            serving != s_serving.end() && serving->second.first == reinterpret_cast<uintptr_t>(pRef->GetNiNode()) && serving->second.second == pRef->GetParentCellEx())
            continue;
        auto root = Retain(pRef->GetNiNode());
        if (!root)
            continue;
        auto& sample = s_read[formId];
        sample.Root = reinterpret_cast<uintptr_t>(root.get());
        sample.pCell = pRef->GetParentCellEx();
        sample.At = cNow;
        if (probe)
            sample.Count = CountNow(root.get());
        else if (!ReadNow(root.get(), sample.Poses, sample.Layout))
            sample.Poses.clear();
    }
    std::scoped_lock lock(s_queueLock);
    if (generation == s_sampleGeneration)
    {
        std::erase_if(s_samples, [cNow](const auto& acSample) { return !acSample.second.Fresh(cNow); });
        for (auto& [formId, sample] : s_read)
            s_samples.insert_or_assign(formId, std::move(sample));
    }
    // Thrown objects, once the game has made their bodies dynamic again.
    for (auto it = s_launches.begin(); it != s_launches.end();)
    {
        auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(it->FormId));
        const bool cGone = !pRef || pRef->IsDeleted() || pRef->IsDisabled() || pRef->IsStillLoading() || !pRef->GetNiNode();
        if (!cGone && LaunchNow(pRef, it->Velocity, it->Spin))
            it = s_launches.erase(it);
        else if (cGone || cNow - it->Since > kLaunchFor)
        {
            spdlog::debug("WorldObject: could not give thrown {:X} its speed", it->FormId);
            it = s_launches.erase(it);
        }
        else
            ++it;
    }
    // Stop driving if the update job has stopped submitting work.
    if (s_queued.empty() || cNow - s_queuedAt > std::chrono::milliseconds(500))
        return;
    for (const auto& [formId, driven] : s_queued)
    {
        if (driven.LetGo(cNow))
            continue;
        auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (!pRef || pRef->IsDeleted() || pRef->IsDisabled() || pRef->IsStillLoading() || !pRef->GetNiNode())
            continue;
        if (!Drive(pRef, driven.Now(cNow), driven.Layout, cFrameSeconds))
        {
            static Clock::time_point s_nextLog{};
            if (cNow >= s_nextLog)
            {
                s_nextLog = cNow + std::chrono::seconds(5);
                spdlog::debug("WorldObject: could not drive hanging bodies for {:X}", formId);
            }
        }
    }
}

bool Drive(TESObjectREFR* apRef, const WorldObjectBodies::Targets& acTargets, const uint32_t aLayout, const float aFrameSeconds) noexcept
{
    auto root = Retain(apRef ? apRef->GetNiNode() : nullptr);
    void* pRoot = root.get();
    const float cScale = HavokScale();
    std::vector<Body> bodies;
    uint32_t layout{};
    void* pHkpWorld = pRoot && cScale > 0.f ? Collect(pRoot, bodies, layout) : nullptr;
    if (!pHkpWorld || !RagdollBodiesPose::Fits(acTargets.size(), aLayout, acTargets.size(), bodies.size(), layout))
        return false;
    POINTER_SKYRIMSE(THkpEntityActivate, s_activate, kHkpEntityActivateAeId, 60096);
    if (!s_activate.Get())
        return false;

    const Root cRoot = RootOf(pRoot);
    WorldWriteLock lock(CheckedBhkWorldOf(pHkpWorld));
    if (!lock.Held())
        return false;
    bool written = false;
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        uint8_t* pHavok = bodies[i].pHavok;
        // Still in that world now that it is locked.
        if (At<void*>(pHavok, kHkWorldOffset) != pHkpWorld)
            continue;
        const WorldObjectBodies::Target& cWanted = acTargets[i];
        const WorldObjectBodies::Pose cTarget = WorldObjectBodies::FromRoot(cRoot.Turn, cRoot.At, cRoot.Scale, cWanted.At);
        // Toward the target, plus the target's own speed, turned out of the root's space.
        const glm::vec3 cSpin = WorldObjectBodies::AngularToward(TurnOf(pHavok), cTarget.Rotation, aFrameSeconds) + cRoot.Turn * cWanted.Spin;
        glm::vec3 velocity =
            (WorldObjectBodies::LinearToward(PositionOf(pHavok, cScale), cTarget.Position, aFrameSeconds) + cRoot.Turn * (cWanted.Velocity * cRoot.Scale)) * cScale;
        // Havok's linear velocity is the centre of mass's; the target is the body's origin, which the spin carries around it.
        const auto* pOrigin = reinterpret_cast<const float*>(pHavok + kBodyTranslationOffset);
        const auto* pCenter = reinterpret_cast<const float*>(pHavok + kCenterOfMassOffset);
        const glm::vec3 cArm = glm::vec3{pCenter[0], pCenter[1], pCenter[2]} - glm::vec3{pOrigin[0], pOrigin[1], pOrigin[2]};
        if (IsFinite(cArm) && glm::length(cArm) < 10.f)
            velocity += glm::cross(cSpin, cArm);
        if (!IsFinite(velocity) || !IsFinite(cSpin))
            continue;
        const glm::vec3 cVelocity = velocity;
        s_activate.Get()(pHavok);
        std::memset(pHavok + kLinearVelocityOffset, 0, 16);
        std::memcpy(pHavok + kLinearVelocityOffset, &cVelocity, sizeof(float) * 3);
        std::memset(pHavok + kAngularVelocityOffset, 0, 16);
        std::memcpy(pHavok + kAngularVelocityOffset, &cSpin, sizeof(float) * 3);
        written = true;
    }
    return written;
}
} // namespace PropHavok
