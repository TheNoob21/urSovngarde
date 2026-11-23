#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/PropBodyState.h>
#include <Structs/Vector3_NetQuantize.h>

//! A placed object's hinged or hanging bodies (a shop sign, a hanging pot) swing here while its root stays put; this is how they lie now.
struct RequestPropBodies final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPropBodies;

    RequestPropBodies()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestPropBodies& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ObjectId == acRhs.ObjectId && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position &&
               Bodies == acRhs.Bodies && AtRest == acRhs.AtRest && Tick == acRhs.Tick;
    }

    //! The object's own reference id, the same in every game that has the plugin it comes from.
    GameId ObjectId{};
    GameId CellId{};
    GameId WorldSpaceId{};
    //! Where its root is, for who is in range.
    Vector3_NetQuantize Position{};
    PropBodyState Bodies{};
    //! The last of a swing: the bodies have stopped, and the receiver hands them back to its own physics.
    bool AtRest{};
    //! When the sender read the bodies, on the clock every movement report uses (0: not given).
    uint64_t Tick{};
};
