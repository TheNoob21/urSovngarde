#pragma once

#include "Message.h"

// Another player's game does not load the same plugins: forms from a plugin only one game loads cannot be matched in the other (see ModIdTable).
struct NotifyModListsDiffer final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyModListsDiffer;

    NotifyModListsDiffer()
        : ServerMessage(Opcode)
    {
    }

    virtual ~NotifyModListsDiffer() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyModListsDiffer& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && Username == achRhs.Username && OnlyHere == achRhs.OnlyHere && OnlyThere == achRhs.OnlyThere;
    }

    String Username{};
    // In this game and not in theirs.
    TiltedPhoques::Vector<String> OnlyHere{};
    // In theirs and not in this game.
    TiltedPhoques::Vector<String> OnlyThere{};
};
