#pragma once

// Which activations another game made before this one came near are replayed here, and how fast: plain values, so the rules
// are unit-tested.
//
// Only mechanisms whose state is the sum of their uses: levers, pull chains and bars, buttons, valves, puzzle pillars, and the
// claw doors' rings and keyholes. Each use is replayed in the order it happened, one at a time, as their scripts ignore a use
// while they turn (HallofStoriesDiskScript's busy state), and the keyhole checks that every ring has stopped.
// Nothing with an effect on the player who used it is replayed: shrines and standing stones apply only to that player, and a
// trap trigger is left alone (a fired trap is not fired again for a late player). A lever that sets off a trap does fire it.
//
// The bases were found in the game's masters by the scripts on them (TrapLever, NorLever01SCRIPT, NorPullBar01SCRIPT,
// defaultButtonSCRIPT, DwePipeValve01Script, defaultPuzzlePillar*, dunHalldirPuzzlePillar, HallofStoriesDiskScript,
// HallofStoriesKeyholeScript), leaving out quest ones, TempleBlessingScript, and the Dwemer button's TrapTriggerBase.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ActivationCatchUp
{
enum class Plugin : uint8_t
{
    Skyrim,
    Dawnguard,
    Dragonborn,
    Other
};

inline constexpr std::array<uint32_t, 47> kSkyrimBases{
    0x0001717B, // RuinsPuzzlePillar01
    0x00021513, // NorLever01
    0x00026826, // GenPullBar01
    0x00026858, // GenPullChain01
    0x0004D77E, // MetalLever01
    0x0004DAC6, // PuzzleDoorKeyHole01Glass
    0x0004DAC7, // PuzzleDoorKeyHole01Ruby
    0x0004DAC8, // PuzzleDoorKeyHole01Ivory
    0x0004DAC9, // PuzzleDoorKeyHole01Diamond
    0x0004DACA, // PuzzleDoorKeyHole01Sapphire
    0x0004DACB, // PuzzleDoorKeyHole01Emerald
    0x0004DACC, // PuzzleDoorKeyHole01Coral
    0x0004E2AE, // PuzzleDoorSmallWheel01
    0x0004E2B0, // PuzzleDoorMediumWheel01
    0x0004E2B1, // PuzzleDoorLargeWheel01
    0x0004E2B2, // PuzzleDoorKeyHole01
    0x00050553, // PuzzleDoorLargeWheel02
    0x00050554, // PuzzleDoorMediumWheel02
    0x00050555, // PuzzleDoorSmallWheel02
    0x00052E9B, // ImpButton01
    0x0005B35B, // PuzzleDoorSmallWheel03
    0x0005B35C, // PuzzleDoorMediumWheel03
    0x0005B35D, // PuzzleDoorLargeWheel03
    0x00066B21, // PuzzleDoorLargeWheel04
    0x00066B22, // PuzzleDoorMediumWheel04
    0x00066B23, // PuzzleDoorSmallWheel04
    0x00069E4B, // TrapStoneButton
    0x0007122F, // PuzzleDoorLargeWheel05
    0x00071231, // PuzzleDoorMediumWheel05
    0x00071233, // PuzzleDoorSmallWheel05
    0x00071269, // SimpleLever
    0x0008CDCC, // PuzzleDoorKeyHole01Ebony
    0x0008F3F9, // DweLever01
    0x0008F3FA, // DweLever02
    0x0009405D, // DwePipeValve01
    0x00095576, // PuzzleDoorKeyHole01Iron
    0x000977E4, // PuzzleDoorLargeWheel06
    0x000977E5, // PuzzleDoorMediumWheel06
    0x000977E6, // PuzzleDoorSmallWheel06
    0x00098665, // NorPullChain01
    0x00098668, // NorPullBar01
    0x000AA8CE, // NorDefaultPuzzlePillar01
    0x000C6386, // GenPullChain01NoName
    0x000F13AC, // defaultPuzzleLever01NoFurn
    0x000F13AE, // NorDefaultPuzzlePillar01NoFurn
    0x000F13B4, // defaultPuzzlePullChain01NoFurn
    0x000F13B5, // NorDefaultPuzzlePillarPullBar01NoFurn
};

inline constexpr std::array<uint32_t, 3> kDawnguardBases{
    0x00007F4D, // DLC1FalmerPullChain01
    0x00015FBC, // DLC1LD_BthalftValves
    0x0001A3BF, // DLC1VCCandleStickLever
};

inline constexpr std::array<uint32_t, 1> kDragonbornBases{
    0x00038D44, // DLC2SV01FullClawKeyHole
};

// aObjectId: the base form's id within its plugin (the low 24 bits).
[[nodiscard]] constexpr bool IsMechanism(Plugin aPlugin, uint32_t aObjectId) noexcept
{
    const auto cContains = [aObjectId](const auto& acBases)
    {
        return std::find(acBases.begin(), acBases.end(), aObjectId) != acBases.end();
    };
    switch (aPlugin)
    {
    case Plugin::Skyrim: return cContains(kSkyrimBases);
    case Plugin::Dawnguard: return cContains(kDawnguardBases);
    case Plugin::Dragonborn: return cContains(kDragonbornBases);
    default: return false;
    }
}

// Long enough for a lever, a pillar or a ring to finish turning, and for a keyhole's own three seconds.
inline constexpr auto kGap = std::chrono::milliseconds(3500);
// A use waits this long for its reference's cell to be loaded here; a player who went elsewhere meanwhile does not get it.
inline constexpr auto kWait = std::chrono::seconds(30);
// A replayed lever's script may use the gate it works as this player: within this long of a replay, this player's use of
// another activator is that chain, which the other games did themselves, and is not sent to them.
inline constexpr auto kChainWindow = std::chrono::seconds(3);
static_assert(kChainWindow < kGap);

[[nodiscard]] inline bool FollowsReplay(std::chrono::steady_clock::time_point aLastReplay, std::chrono::steady_clock::time_point aNow) noexcept
{
    return aLastReplay != std::chrono::steady_clock::time_point{} && aNow - aLastReplay < kChainWindow;
}

// What a use waiting to be replayed is now.
enum class Readiness : uint8_t
{
    //! Its reference's cell is not loaded here yet.
    Wait,
    //! Loaded, and a mechanism.
    Go,
    //! Loaded, and not one to replay (ObjectService passes on every activation): passed over at once.
    Skip
};

// The uses to replay, in order, one per kGap.
class Pacer
{
public:
    using Clock = std::chrono::steady_clock;

    struct Step
    {
        uint32_t FormId{};
        //! Given up: its cell was not loaded here within kWait.
        bool Dropped{};
    };

    void Add(uint32_t aFormId, Clock::time_point aNow) { m_waiting.push_back({aFormId, aNow}); }

    // The use to replay now, if any. aReadiness: what a reference is here now. A use that waits holds back the ones after it,
    // as their order matters.
    template <class TReadiness> [[nodiscard]] Step Next(Clock::time_point aNow, TReadiness&& aReadiness)
    {
        while (!m_waiting.empty() && aNow >= m_nextAt)
        {
            const Waiting cHead = m_waiting.front();
            const Readiness cReadiness = aReadiness(cHead.FormId);
            if (cReadiness == Readiness::Skip)
            {
                m_waiting.pop_front();
                continue;
            }
            if (cReadiness == Readiness::Go)
            {
                m_waiting.pop_front();
                m_nextAt = aNow + kGap;
                return {cHead.FormId, false};
            }
            if (aNow - cHead.Since >= kWait)
            {
                m_waiting.pop_front();
                // The next one waits its own time from now.
                if (!m_waiting.empty())
                    m_waiting.front().Since = std::max(m_waiting.front().Since, aNow);
                return {cHead.FormId, true};
            }
            break;
        }
        return {};
    }

    void Clear() noexcept
    {
        m_waiting.clear();
        m_nextAt = {};
    }

    [[nodiscard]] size_t Size() const noexcept { return m_waiting.size(); }

private:
    struct Waiting
    {
        uint32_t FormId;
        Clock::time_point Since;
    };

    std::deque<Waiting> m_waiting;
    Clock::time_point m_nextAt{};
};

// A lever that stays down or a pillar that stays turned is set to the end state the game that used it last saw, not
// replayed use by use: a late player's own save may already hold some of those uses. That state is the mechanism's resting
// pose, the local transforms of the first objects below its 3D root (its script's position follows its animation, and both
// games run the same mesh and animation). A control that springs back (a pull chain, a button) has no pose of its own and
// keeps its uses replayed.
using Pose = std::vector<int16_t>;

inline constexpr size_t kPoseObjects = 16;
//! A rotation (nine values, in thousandths) and a translation (three values, in tenths of a unit) per object.
inline constexpr size_t kPoseValuesPerObject = 12;
inline constexpr size_t kMaxPoseValues = kPoseObjects * kPoseValuesPerObject;
inline constexpr int kRotationTolerance = 60;
inline constexpr int kTranslationTolerance = 20;
//! Uses a game makes at most to reach a state: a pillar or a ring has three positions.
inline constexpr uint32_t kMaxStateUses = 3;

[[nodiscard]] constexpr int16_t Quantize(float aValue, float aScale) noexcept
{
    const float cScaled = aValue * aScale;
    if (!(cScaled > -32767.f))
        return -32767;
    if (cScaled >= 32767.f)
        return 32767;
    return static_cast<int16_t>(cScaled < 0.f ? cScaled - 0.5f : cScaled + 0.5f);
}

[[nodiscard]] constexpr int16_t QuantizeRotation(float aValue) noexcept
{
    return Quantize(aValue, 1000.f);
}

[[nodiscard]] constexpr int16_t QuantizeTranslation(float aValue) noexcept
{
    return Quantize(aValue, 10.f);
}

[[nodiscard]] constexpr bool IsValidPoseSize(size_t aSize) noexcept
{
    return aSize > 0 && aSize <= kMaxPoseValues && aSize % kPoseValuesPerObject == 0;
}

// Two poses are the same position when each value is within its tolerance; poses of different meshes never are.
template <class TLeft, class TRight> [[nodiscard]] bool PoseMatches(const TLeft& acLeft, const TRight& acRight) noexcept
{
    if (acLeft.size() != acRight.size() || !IsValidPoseSize(acLeft.size()))
        return false;
    for (size_t i = 0; i < acLeft.size(); ++i)
    {
        const int cTolerance = i % kPoseValuesPerObject < 9 ? kRotationTolerance : kTranslationTolerance;
        const int cDelta = static_cast<int>(acLeft[i]) - static_cast<int>(acRight[i]);
        if (cDelta > cTolerance || cDelta < -cTolerance)
            return false;
    }
    return true;
}

enum class Settle : uint8_t
{
    //! At the end state.
    Done,
    //! Use it once more.
    Use,
    //! Left as it is: another mesh, it did not move, it came round to where it started, or it had its uses.
    GiveUp
};

// What a game does next to bring a mechanism to acTarget. acStart: its pose before the first use here; acPrevious: before
// the last use; acCurrent: now; aUses: uses made so far. It stops rather than guess, and a two-way lever that comes round
// is back where it started.
template <class TPose> [[nodiscard]] Settle NextSettle(const TPose& acTarget, const Pose& acStart, const Pose& acPrevious, const Pose& acCurrent, uint32_t aUses) noexcept
{
    if (PoseMatches(acTarget, acCurrent))
        return Settle::Done;
    if (acTarget.size() != acCurrent.size() || !IsValidPoseSize(acCurrent.size()))
        return Settle::GiveUp;
    if (aUses > 0 && (PoseMatches(acCurrent, acPrevious) || PoseMatches(acCurrent, acStart)))
        return Settle::GiveUp;
    return aUses < kMaxStateUses ? Settle::Use : Settle::GiveUp;
}

// The end states to set, one use at a time, each kGap apart, and each once its mechanism has been loaded here for kGap,
// so its own script has set its starting position. A state waits at most kWait to be loaded.
class StatePacer
{
public:
    using Clock = std::chrono::steady_clock;

    enum class Action : uint8_t
    {
        None,
        Use,
        Done,
        GaveUp,
        Dropped
    };

    struct Step
    {
        uint32_t FormId{};
        Action What{Action::None};
    };

    //! What a reference is here now and, when it is Go, its pose.
    struct Look
    {
        Readiness State{Readiness::Wait};
        Pose Current;
    };

    // A newer state for the same reference replaces the one waiting, and starts over.
    void Add(uint32_t aFormId, Pose aTarget, Clock::time_point aNow)
    {
        for (size_t i = 0; i < m_waiting.size(); ++i)
        {
            if (m_waiting[i].FormId == aFormId)
            {
                m_waiting.erase(m_waiting.begin() + static_cast<std::ptrdiff_t>(i));
                if (m_active && *m_active == i)
                    m_active.reset();
                else if (m_active && *m_active > i)
                    --*m_active;
                break;
            }
        }
        m_waiting.push_back({aFormId, std::move(aTarget), aNow});
    }

    // aLook(formId) -> Look.
    template <class TLook> [[nodiscard]] Step Next(Clock::time_point aNow, TLook&& aLook)
    {
        if (aNow < m_nextAt)
            return {};

        if (m_active)
        {
            const size_t cIndex = *m_active;
            Entry& entry = m_waiting[cIndex];
            const Look cLook = aLook(entry.FormId);
            if (cLook.State != Readiness::Go)
            {
                // Unloaded or gone mid-way: left as it is.
                return Finish(cIndex, Action::Dropped);
            }
            const Settle cSettle = NextSettle(entry.Target, entry.Start, entry.Previous, cLook.Current, entry.Uses);
            if (cSettle == Settle::Use)
            {
                entry.Previous = cLook.Current;
                ++entry.Uses;
                m_nextAt = aNow + kGap;
                return {entry.FormId, Action::Use};
            }
            return Finish(cIndex, cSettle == Settle::Done ? Action::Done : Action::GaveUp);
        }

        for (size_t i = 0; i < m_waiting.size();)
        {
            Entry& entry = m_waiting[i];
            Look look = aLook(entry.FormId);
            if (look.State == Readiness::Skip)
                return Finish(i, Action::Dropped);
            if (look.State == Readiness::Wait)
            {
                entry.ReadySince = {};
                if (aNow - entry.Since >= kWait)
                    return Finish(i, Action::Dropped);
                ++i;
                continue;
            }
            if (entry.ReadySince == Clock::time_point{})
                entry.ReadySince = aNow;
            if (aNow - entry.ReadySince < kGap)
            {
                ++i;
                continue;
            }

            const Settle cSettle = NextSettle(entry.Target, look.Current, look.Current, look.Current, 0);
            if (cSettle != Settle::Use)
                return Finish(i, cSettle == Settle::Done ? Action::Done : Action::GaveUp);
            entry.Start = look.Current;
            entry.Previous = std::move(look.Current);
            entry.Uses = 1;
            m_active = i;
            m_nextAt = aNow + kGap;
            return {entry.FormId, Action::Use};
        }
        return {};
    }

    void Clear() noexcept
    {
        m_waiting.clear();
        m_active.reset();
        m_nextAt = {};
    }

    [[nodiscard]] size_t Size() const noexcept { return m_waiting.size(); }
    //! Whether a state is being set now; a use replay waits for it, as a keyhole needs its rings to have stopped.
    [[nodiscard]] bool Busy() const noexcept { return m_active.has_value(); }

private:
    struct Entry
    {
        uint32_t FormId;
        Pose Target;
        Clock::time_point Since;
        Clock::time_point ReadySince{};
        Pose Start{};
        Pose Previous{};
        uint32_t Uses{};
    };

    Step Finish(size_t aIndex, Action aAction)
    {
        const uint32_t cFormId = m_waiting[aIndex].FormId;
        m_waiting.erase(m_waiting.begin() + static_cast<std::ptrdiff_t>(aIndex));
        m_active.reset();
        return {cFormId, aAction};
    }

    std::vector<Entry> m_waiting;
    std::optional<size_t> m_active;
    Clock::time_point m_nextAt{};
};

// The game that used a mechanism reports its end state once it has come to rest: kGap after its last use here, when its
// pose differs from the one before that use (a control that springs back reports nothing).
class StateReporter
{
public:
    using Clock = std::chrono::steady_clock;

    // A use of aFormId, whose pose was acBefore just then. Another use before the report keeps the first pose.
    void Used(uint32_t aFormId, Pose aBefore, Clock::time_point aNow)
    {
        for (Pending& pending : m_pending)
        {
            if (pending.FormId == aFormId)
            {
                pending.Due = aNow + kGap;
                return;
            }
        }
        if (m_pending.size() == kMaxPending)
            m_pending.erase(m_pending.begin());
        m_pending.push_back({aFormId, std::move(aBefore), aNow + kGap});
    }

    // The next mechanism due, if any; aPose(formId) -> its pose now (empty when it is not loaded). aSend(formId, pose) is
    // called when its pose changed.
    template <class TPose, class TSend> void Update(Clock::time_point aNow, TPose&& aPose, TSend&& aSend)
    {
        for (size_t i = 0; i < m_pending.size();)
        {
            if (aNow < m_pending[i].Due)
            {
                ++i;
                continue;
            }
            Pending pending = std::move(m_pending[i]);
            m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
            Pose after = aPose(pending.FormId);
            if (IsValidPoseSize(after.size()) && !PoseMatches(after, pending.Before))
                aSend(pending.FormId, std::move(after));
        }
    }

    void Clear() noexcept { m_pending.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_pending.size(); }

    static constexpr size_t kMaxPending = 32;

private:
    struct Pending
    {
        uint32_t FormId;
        Pose Before;
        Clock::time_point Due;
    };

    std::vector<Pending> m_pending;
};
} // namespace ActivationCatchUp
