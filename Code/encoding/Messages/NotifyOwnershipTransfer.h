#pragma once

#include "Message.h"
#include <Structs/ActorData.h>
#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

struct NotifyOwnershipTransfer final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyOwnershipTransfer;
    NotifyOwnershipTransfer()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyOwnershipTransfer() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyOwnershipTransfer& achRhs) const noexcept
    {
        return ServerId == achRhs.ServerId && OwnerPlayerId == achRhs.OwnerPlayerId && OwnershipEpoch == achRhs.OwnershipEpoch && CurrentActorData == achRhs.CurrentActorData &&
               LeveledNpcPickId == achRhs.LeveledNpcPickId && CombatTarget == achRhs.CombatTarget && CellId == achRhs.CellId && Position == achRhs.Position &&
               Heading == achRhs.Heading && GetOpcode() == achRhs.GetOpcode();
    }

    uint32_t ServerId{};
    uint32_t OwnerPlayerId{};
    uint32_t OwnershipEpoch{};
    ActorData CurrentActorData{};
    GameId LeveledNpcPickId{};
    //! Whom its last owner's game had it fighting, by server id; 0 for nobody.
    uint32_t CombatTarget{};
    //! Where its last owner's game last had it, for the new owner's game to carry on from; no cell when that game sent nothing since it was given the actor.
    GameId CellId{};
    Vector3_NetQuantize Position{};
    float Heading{};
};
