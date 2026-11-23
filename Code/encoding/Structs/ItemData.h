#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <Structs/GameId.h>

//! One magic effect of an item, its effect told by its server id.
struct ItemEffect
{
    bool operator==(const ItemEffect& acRhs) const noexcept
    {
        return Magnitude == acRhs.Magnitude && Area == acRhs.Area && Duration == acRhs.Duration && RawCost == acRhs.RawCost && EffectId == acRhs.EffectId;
    }
    bool operator!=(const ItemEffect& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    float Magnitude{};
    int32_t Area{};
    int32_t Duration{};
    float RawCost{};
    GameId EffectId{};
};

namespace ItemEffects
{
//! A message with more effects than this keeps only the first ones.
static constexpr size_t kMaxEffects = 32;

//! The most an effect's magnitude or cost is read as, either way, and the most its area or duration is: far above what any made
//! item has, so that no value a game could not hold reaches a form kept in the receiver's save.
static constexpr float kMaxValue = 1000000.f;
static constexpr int32_t kMaxSpan = 1000000;

//! Brings a decoded effect within those limits. A number that is not a number at all becomes 0: the message is kept, because a
//! player who carries such a made item must still appear for the others, and the receiver makes a harmless item.
inline void KeepSane(ItemEffect& aEffect) noexcept
{
    if (!std::isfinite(aEffect.Magnitude))
        aEffect.Magnitude = 0.f;
    if (!std::isfinite(aEffect.RawCost))
        aEffect.RawCost = 0.f;
    aEffect.Magnitude = std::clamp(aEffect.Magnitude, -kMaxValue, kMaxValue);
    aEffect.RawCost = std::clamp(aEffect.RawCost, -kMaxValue, kMaxValue);
    aEffect.Area = std::clamp(aEffect.Area, int32_t{0}, kMaxSpan);
    aEffect.Duration = std::clamp(aEffect.Duration, int32_t{0}, kMaxSpan);
}

//! The id a run-time form is sent by: each game numbers its own run-time forms, but the same effects make the same form.
//! What the game itself compares when it looks for a made potion or enchantment: effect, magnitude, area and duration.
inline uint32_t Key(const TiltedPhoques::Vector<ItemEffect>& acEffects, const uint8_t aKind) noexcept
{
    uint32_t hash = 2166136261u;
    const auto cMix = [&hash](const uint32_t aValue)
    {
        for (int i = 0; i < 4; ++i)
        {
            hash ^= (aValue >> (i * 8)) & 0xFFu;
            hash *= 16777619u;
        }
    };
    cMix(aKind);
    cMix(static_cast<uint32_t>(acEffects.size()));
    for (const ItemEffect& effect : acEffects)
    {
        uint32_t magnitude = 0;
        std::memcpy(&magnitude, &effect.Magnitude, sizeof(magnitude));
        cMix(effect.EffectId.ModId);
        cMix(effect.EffectId.BaseId);
        cMix(magnitude);
        cMix(static_cast<uint32_t>(effect.Area));
        cMix(static_cast<uint32_t>(effect.Duration));
    }
    // 0 is no form.
    return hash ? hash : 1u;
}
} // namespace ItemEffects

//! A potion or poison a player made. Its form exists only in that game, so it travels as what it does.
struct CreatedAlchemy
{
    //! A message with a longer name keeps its start.
    static constexpr size_t kMaxNameLength = 256;

    bool operator==(const CreatedAlchemy& acRhs) const noexcept
    {
        return Name == acRhs.Name && CostOverride == acRhs.CostOverride && IsPoison == acRhs.IsPoison && Effects == acRhs.Effects;
    }
    bool operator!=(const CreatedAlchemy& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    [[nodiscard]] bool IsSet() const noexcept { return !Effects.empty(); }
    //! Its id on the wire (ItemEffects::Key), the same in every game.
    [[nodiscard]] uint32_t Key() const noexcept { return ItemEffects::Key(Effects, IsPoison ? 2 : 1); }

    TiltedPhoques::String Name{};
    //! The value the game gave it over the one its effects make, or -1.
    int32_t CostOverride = -1;
    bool IsPoison{};
    TiltedPhoques::Vector<ItemEffect> Effects{};
};

//! What an inventory entry carries beyond Inventory::Entry's own extra data.
struct ItemData
{
    bool operator==(const ItemData& acRhs) const noexcept { return DisplayName == acRhs.DisplayName && Base == acRhs.Base && Poison == acRhs.Poison; }
    bool operator!=(const ItemData& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    //! True when the item itself carries something: a name or a made poison. A made base is its form, not extra data.
    [[nodiscard]] bool HasExtra() const noexcept { return !DisplayName.empty() || Poison.IsSet(); }
    //! A made poison is told by what it does: each game names a made item in its own words.
    [[nodiscard]] bool IsExtraEqual(const ItemData& acRhs) const noexcept
    {
        return DisplayName == acRhs.DisplayName && Poison.IsSet() == acRhs.Poison.IsSet() && (!Poison.IsSet() || Poison.Key() == acRhs.Poison.Key());
    }

    //! The name a player gave it (enchanting, a mod's rename); empty for its base form's name.
    TiltedPhoques::String DisplayName{};
    //! Set when the item is itself a made potion or poison.
    CreatedAlchemy Base{};
    //! Set when the poison on a weapon is a made one.
    CreatedAlchemy Poison{};
};
