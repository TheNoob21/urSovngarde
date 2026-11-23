#pragma once

#include "Message.h"

// Another player's word to a follower (FollowerCommand.h), or her player's game's answer to it.
// A word goes to her player's game; an answer (Reply not 0) goes back to the player who asked.
struct RequestFollowerCommand final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestFollowerCommand;

    RequestFollowerCommand()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestFollowerCommand() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestFollowerCommand& acRhs) const noexcept
    {
        return ServerId == acRhs.ServerId && Command == acRhs.Command && Reply == acRhs.Reply && ReplyToPlayerId == acRhs.ReplyToPlayerId && GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t ServerId{};
    uint8_t Command{};
    uint8_t Reply{};
    uint32_t ReplyToPlayerId{};
};
