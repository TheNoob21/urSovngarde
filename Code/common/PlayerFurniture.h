#pragma once

#include <NpcFurniture.h>
#include <Structs/PlayerFurnitureWord.h>

#include <optional>
#include <utility>

namespace PlayerFurniture
{
//! A marker index the furniture can have; an NPC's word from another game is read against it too.
inline bool MarkerFits(const uint32_t aMarker) noexcept
{
    return aMarker < kMarkerCount;
}

//! Two seats placed at run time this close are one chair: each game's script placed its own copy of it, a few units apart.
constexpr float kSameChairUnits = 16.f;

//! Whether two seats are one, for whether another player already occupies it: the same reference and marker, or for furniture
//! placed at run time, the same base form and marker within kSameChairUnits.
inline bool SameChair(const Seat& acA, const Seat& acB) noexcept
{
    if (!acA.Placement.Placed && !acB.Placement.Placed)
        return acA == acB;
    if (!acA.Placement.Placed || !acB.Placement.Placed || acA.Reference != acB.Reference || acA.Marker != acB.Marker)
        return false;
    const double cX = static_cast<double>(acA.Placement.X) - acB.Placement.X;
    const double cY = static_cast<double>(acA.Placement.Y) - acB.Placement.Y;
    const double cZ = static_cast<double>(acA.Placement.Z) - acB.Placement.Z;
    return cX * cX + cY * cY + cZ * cZ <= static_cast<double>(kSameChairUnits) * kSameChairUnits;
}

enum class Phase : uint8_t
{
    Entering,
    Sitting,
    Sleeping,
};

struct Authority
{
    uint32_t Actor{};
    uint32_t Epoch{};
    bool MayOccupy{}; // The server authorized the actor, cell, plugin and rider state.
    GameId Cell{};
};

struct Claim
{
    uint32_t Actor{};
    uint32_t Epoch{};
    uint64_t Lease{};
    Seat Where{};
    Phase State{};
    uint64_t RefreshedAt{};
    GameId Cell{};
    uint64_t EnterSequence{};
};

struct Result
{
    Verdict Decision{Verdict::Rejected};
    std::optional<Claim> Current;
    std::optional<Claim> Released;
};

class Claims
{
public:
    Result Apply(const Request& acRequest, const Authority& acAuthority, const uint64_t aNow) noexcept
    {
        if (!acAuthority.Epoch || acRequest.Actor != acAuthority.Actor || acRequest.Epoch != acAuthority.Epoch || !acRequest.Sequence)
            return {};

        auto& actor = m_actors[acAuthority.Actor];
        std::optional<Claim> retired;
        if (actor.Epoch != acAuthority.Epoch)
        {
            retired = actor.Occupying;
            actor = {acAuthority.Epoch, 0, std::nullopt};
        }
        const auto cCurrent = actor.Occupying && Active(*actor.Occupying, aNow) ? actor.Occupying : std::nullopt;
        if (acRequest.Sequence <= actor.LastSequence)
            return {Verdict::Rejected, cCurrent, retired};
        actor.LastSequence = acRequest.Sequence;

        if (acRequest.Change == Action::Enter)
        {
            if (!acRequest.Where.Reference.BaseId || acRequest.Where.Marker >= kMarkerCount || acRequest.Lease || acRequest.EnterSequence)
                return {Verdict::Rejected, cCurrent, retired};
            if (!acAuthority.MayOccupy)
            {
                if (cCurrent && cCurrent->State == Phase::Entering && cCurrent->Where == acRequest.Where && cCurrent->Cell == acAuthority.Cell)
                    return {Verdict::Rejected, {}, std::exchange(actor.Occupying, std::nullopt)};
                return {Verdict::Rejected, cCurrent, retired};
            }
            if (cCurrent && cCurrent->Where == acRequest.Where && cCurrent->Cell == acAuthority.Cell)
            {
                actor.Occupying->EnterSequence = acRequest.Sequence;
                return {Verdict::Granted, actor.Occupying, retired};
            }

            for (const auto& [cActorId, cActor] : m_actors)
                if (cActorId != acAuthority.Actor && cActor.Occupying && SameChair(cActor.Occupying->Where, acRequest.Where) && Active(*cActor.Occupying, aNow))
                    return {Verdict::Occupied, cCurrent, retired};

            if (++m_nextLease == 0)
                ++m_nextLease;
            if (actor.Occupying)
                retired = actor.Occupying;
            actor.Occupying = Claim{acAuthority.Actor, acAuthority.Epoch, m_nextLease, acRequest.Where, Phase::Entering, aNow, acAuthority.Cell, acRequest.Sequence};
            return {Verdict::Granted, actor.Occupying, retired};
        }

        if (acRequest.Change == Action::Cancel)
        {
            if (!cCurrent || cCurrent->State != Phase::Entering || cCurrent->Where != acRequest.Where || cCurrent->Cell != acAuthority.Cell || acRequest.Lease ||
                acRequest.EnterSequence != cCurrent->EnterSequence)
                return {Verdict::Rejected, cCurrent, retired};
            return {Verdict::Released, {}, std::exchange(actor.Occupying, std::nullopt)};
        }
        if (!cCurrent || cCurrent->Lease != acRequest.Lease || cCurrent->Where != acRequest.Where || acRequest.EnterSequence)
            return {Verdict::Rejected, cCurrent, retired};
        if (acRequest.Change == Action::Exit)
            return {Verdict::Released, {}, std::exchange(actor.Occupying, std::nullopt)};
        if (!acAuthority.MayOccupy || cCurrent->Cell != acAuthority.Cell || (acRequest.Change == Action::Refresh && cCurrent->State == Phase::Entering))
            return {Verdict::Rejected, cCurrent, retired};

        auto& claim = *actor.Occupying;
        switch (acRequest.Change)
        {
        case Action::Sit: claim.State = Phase::Sitting; break;
        case Action::Sleep: claim.State = Phase::Sleeping; break;
        case Action::Refresh: break;
        default: return {Verdict::Rejected, cCurrent, retired};
        }
        claim.RefreshedAt = aNow;
        return {acRequest.Change == Action::Refresh ? Verdict::Refreshed : Verdict::Settled, claim, retired};
    }

    TiltedPhoques::Vector<Claim> Expire(const uint64_t aNow) noexcept
    {
        TiltedPhoques::Vector<Claim> expired;
        for (auto it = m_actors.begin(); it != m_actors.end(); ++it)
        {
            auto& occupying = it.value().Occupying;
            if (occupying && !Active(*occupying, aNow))
            {
                expired.push_back(*occupying);
                occupying.reset();
            }
        }
        return expired;
    }

    std::optional<Claim> RemoveActor(const uint32_t aActor) noexcept
    {
        const auto it = m_actors.find(aActor);
        if (it == m_actors.end())
            return {};
        const auto cRetired = it->second.Occupying;
        m_actors.erase(it);
        return cRetired;
    }

    std::optional<Claim> ReleaseActor(const uint32_t aActor) noexcept
    {
        const auto it = m_actors.find(aActor);
        return it == m_actors.end() ? std::nullopt : std::exchange(it.value().Occupying, std::nullopt);
    }

    const Claim* Find(const uint32_t aActor, const uint64_t aNow) const noexcept
    {
        const auto it = m_actors.find(aActor);
        return it != m_actors.end() && it->second.Occupying && Active(*it->second.Occupying, aNow) ? &*it->second.Occupying : nullptr;
    }

private:
    static bool Active(const Claim& acClaim, const uint64_t aNow) noexcept { return !NpcFurniture::CopyExpired(acClaim.RefreshedAt, aNow); }

    struct ActorState
    {
        uint32_t Epoch{};
        uint64_t LastSequence{};
        std::optional<Claim> Occupying;
    };

    TiltedPhoques::Map<uint32_t, ActorState> m_actors;
    uint64_t m_nextLease{};
};
} // namespace PlayerFurniture
