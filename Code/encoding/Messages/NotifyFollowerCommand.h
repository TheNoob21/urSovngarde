#pragma once

#include "Message.h"

// A word to a follower this game runs, from another player, or this game's answer back from her player's game (FollowerCommand.h).
struct NotifyFollowerCommand final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyFollowerCommand;

    NotifyFollowerCommand()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyFollowerCommand() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyFollowerCommand& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Command == acRhs.Command && Reply == acRhs.Reply && FromPlayerId == acRhs.FromPlayerId && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t ServerId{};
    uint8_t Command{};
    uint8_t Reply{};
    uint32_t FromPlayerId{};
};
