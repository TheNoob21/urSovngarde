#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Stl.hpp>

#include <DecodeGuard.h>
#include <Structs/GameId.h>

//! One lasting effect a player put on themselves (a cloak, Oakflesh, Invisibility), as their game has it now.
struct MagicStateEffect
{
    GameId SpellId{};
    //! The magic effect (MGEF) of the spell's effect item.
    GameId EffectId{};
    float Magnitude{};
    //! Seconds the effect has run, and its full length, in the owner's game.
    float Elapsed{};
    float Duration{};

    bool operator==(const MagicStateEffect& acRhs) const noexcept
    {
        return SpellId == acRhs.SpellId && EffectId == acRhs.EffectId && Magnitude == acRhs.Magnitude && Elapsed == acRhs.Elapsed && Duration == acRhs.Duration;
    }
};

//! A held spell a player is casting from one hand (a concentration spell or a ward); no spell when the hand casts nothing.
struct MagicStateCast
{
    GameId SpellId{};
    bool IsDualCasting{};
    //! A spell the hand is charging and has not cast yet, so its glow is seen; none when the hand charges nothing.
    GameId ChargeId{};

    bool operator==(const MagicStateCast& acRhs) const noexcept { return SpellId == acRhs.SpellId && IsDualCasting == acRhs.IsDualCasting && ChargeId == acRhs.ChargeId; }
};

//! What another player is seen casting and wearing as magic: kept by the server for players who arrive later.
struct MagicState
{
    //! A body seldom carries more than a handful; the rest are refused.
    static constexpr uint64_t kMaxEffects = 32;

    TiltedPhoques::Vector<MagicStateEffect> Effects{};
    MagicStateCast LeftHand{};
    MagicStateCast RightHand{};

    bool operator==(const MagicState& acRhs) const noexcept { return Effects == acRhs.Effects && LeftHand == acRhs.LeftHand && RightHand == acRhs.RightHand; }

    [[nodiscard]] bool Empty() const noexcept { return Effects.empty() && !LeftHand.SpellId && !RightHand.SpellId && !LeftHand.ChargeId && !RightHand.ChargeId; }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        using TiltedPhoques::Serialization;

        Serialization::WriteVarInt(aWriter, Effects.size());
        for (const auto& effect : Effects)
        {
            effect.SpellId.Serialize(aWriter);
            effect.EffectId.Serialize(aWriter);
            Serialization::WriteFloat(aWriter, effect.Magnitude);
            Serialization::WriteFloat(aWriter, effect.Elapsed);
            Serialization::WriteFloat(aWriter, effect.Duration);
        }
        for (const MagicStateCast* pCast : {&LeftHand, &RightHand})
        {
            pCast->SpellId.Serialize(aWriter);
            Serialization::WriteBool(aWriter, pCast->IsDualCasting);
            pCast->ChargeId.Serialize(aWriter);
        }
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
    {
        using TiltedPhoques::Serialization;

        const auto cCount = Serialization::ReadVarInt(aReader);
        if (!DecodeGuard::Within(cCount, kMaxEffects))
            return;
        Effects.resize(static_cast<size_t>(cCount));
        for (auto& effect : Effects)
        {
            effect.SpellId.Deserialize(aReader);
            effect.EffectId.Deserialize(aReader);
            effect.Magnitude = Serialization::ReadFloat(aReader);
            effect.Elapsed = Serialization::ReadFloat(aReader);
            effect.Duration = Serialization::ReadFloat(aReader);
        }
        for (MagicStateCast* pCast : {&LeftHand, &RightHand})
        {
            pCast->SpellId.Deserialize(aReader);
            pCast->IsDualCasting = Serialization::ReadBool(aReader);
            pCast->ChargeId.Deserialize(aReader);
        }
    }
};
