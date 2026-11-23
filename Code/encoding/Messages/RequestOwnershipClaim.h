#pragma once

#include "Message.h"

struct RequestOwnershipClaim final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestOwnershipClaim;

    RequestOwnershipClaim()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestOwnershipClaim() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestOwnershipClaim& achRhs) const noexcept
    {
        return ServerId == achRhs.ServerId && ExpectedOwnershipEpoch == achRhs.ExpectedOwnershipEpoch && Follower == achRhs.Follower && KillingBlow == achRhs.KillingBlow &&
               ForRiding == achRhs.ForRiding && ForHolding == achRhs.ForHolding && ForCrime == achRhs.ForCrime && GetOpcode() == achRhs.GetOpcode();
    }

    uint32_t ServerId{};
    uint32_t ExpectedOwnershipEpoch{};
    //! The actor is the claimant's follower: it goes to the claimant, leader or not, and nobody else can claim it meanwhile.
    bool Follower{};
    //! The claimant's player landed a killing blow on this protected NPC: it goes to the claimant, leader or not, whose game kills it.
    //! The game lets only its own player kill a protected NPC, so only the killer's game can.
    bool KillingBlow{};
    //! The claimant's player is getting on this horse, which a party member's game runs: it goes to the claimant, whose game can only
    //! seat its player on a horse it runs (MountSync::MayClaimToRide).
    bool ForRiding{};
    //! The claimant's player grabbed this living NPC, lying in its ragdoll: it goes to the claimant for the hold, leader or not, and back when
    //! the hold ends (GrabHold). Only the game that runs an NPC sends how its body moves.
    bool ForHolding{};
    //! The claimant's player committed a crime this living NPC saw, and the claimant's report named it: it goes to the claimant for the
    //! arrest, leader or not (CrimeReports::ServerGivesWitness). Only such a claim can take a witness that way.
    bool ForCrime{};
};
