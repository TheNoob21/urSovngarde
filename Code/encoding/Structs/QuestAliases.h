#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <Structs/GameId.h>

#include <cstdint>
#include <vector>

//! What a quest's aliases were filled with in the game that picked them: the reference or location a radiant quest chose.
//! Only aliases with a target every game can name are listed (no empty ones, no references made at run time).
struct QuestAliases
{
    enum Kind : uint8_t
    {
        kRef = 0,
        kLocation = 1
    };

    struct Fill
    {
        uint16_t AliasId{};
        uint8_t AliasKind{kRef};
        GameId Target{};

        bool operator==(const Fill& acRhs) const noexcept { return AliasId == acRhs.AliasId && AliasKind == acRhs.AliasKind && Target == acRhs.Target; }
    };

    //! More than any vanilla quest has; a longer list is cut when read.
    static constexpr size_t kMaxFills = 128;

    bool operator==(const QuestAliases& acRhs) const noexcept { return Quest == acRhs.Quest && Fills == acRhs.Fills; }

    void Write(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        using TiltedPhoques::Serialization;
        Quest.Serialize(aWriter);
        const size_t cCount = Fills.size() < kMaxFills ? Fills.size() : kMaxFills;
        Serialization::WriteVarInt(aWriter, cCount);
        for (size_t i = 0; i < cCount; ++i)
        {
            const Fill& cFill = Fills[i];
            Serialization::WriteVarInt(aWriter, cFill.AliasId);
            aWriter.WriteBits(cFill.AliasKind, 8);
            cFill.Target.Serialize(aWriter);
        }
    }

    void Read(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        using TiltedPhoques::Serialization;
        Quest.Deserialize(aReader);
        const uint64_t cCount = Serialization::ReadVarInt(aReader);
        Fills.clear();
        // Nothing follows the list, so a longer one is cut without reading the rest.
        for (uint64_t i = 0; i < cCount && i < kMaxFills; ++i)
        {
            Fill fill{};
            fill.AliasId = static_cast<uint16_t>(Serialization::ReadVarInt(aReader));
            uint64_t kind = 0;
            aReader.ReadBits(kind, 8);
            fill.AliasKind = static_cast<uint8_t>(kind);
            fill.Target.Deserialize(aReader);
            Fills.push_back(fill);
        }
    }

    GameId Quest{};
    std::vector<Fill> Fills{};
};
