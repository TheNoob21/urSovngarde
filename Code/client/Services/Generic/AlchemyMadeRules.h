#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

//! When another player's copy plays the sound of a potion or poison being made at an alchemy table.
//! Nothing is sent for the making itself: the owner's game sends what it does to the inventory, the ingredients used up
//! and the made item (a run-time form, sent as what it does, ItemData). An ingredient used up and a made potion or poison
//! gained close together is one making: one sound. Pure rules on plain values, so they can be tested
//! (tests/alchemy_made_rules.cpp).
namespace AlchemyMadeRules
{
using Clock = std::chrono::steady_clock;

//! An ingredient used up and a made item gained this close together are one making.
constexpr auto kTogether = std::chrono::milliseconds(1500);
//! The game's making sounds are UI sounds (output model SOMUIDefault): no fall-off with distance. A copy farther than
//! this from the player here makes none.
constexpr float kHearWithin = 1500.f;
//! More characters than this, and all are forgotten (a making in progress loses at most its sound).
constexpr size_t kMaxTracked = 64;

enum class Made : uint8_t
{
    None,
    Potion,
    Poison,
};

class Tracker
{
public:
    //! The owner ate or drank an ingredient (the equip message): its removal that comes with it is no making.
    void NoteEaten(const uint32_t aActor, const uint32_t aItem, const Clock::time_point aNow) noexcept
    {
        auto& state = Get(aActor);
        state.EatenItem = aItem;
        state.EatenAt = aNow;
        state.HasEaten = true;
        // The removal may have come first.
        if (state.HasIngredient && state.IngredientItem == aItem && Within(state.IngredientAt, aNow))
            state.HasIngredient = false;
    }

    //! The owner's inventory lost an ingredient. A drop, sale or chest put looks the same here (the server sends no drop flag
    //! to the others); only a made item gained close by makes it a making. Returns the sound to play now, if any.
    Made NoteIngredientUsed(const uint32_t aActor, const uint32_t aItem, const Clock::time_point aNow) noexcept
    {
        auto& state = Get(aActor);
        if (state.HasEaten && state.EatenItem == aItem && Within(state.EatenAt, aNow))
        {
            state.HasEaten = false;
            return Made::None;
        }
        if (state.MadeKind != Made::None && Within(state.MadeAt, aNow))
            return Complete(state, state.MadeKind);
        state.HasIngredient = true;
        state.IngredientItem = aItem;
        state.IngredientAt = aNow;
        return Made::None;
    }

    //! The owner's inventory gained a made potion or poison (not picked up from the world). Returns the sound to play now.
    Made NoteMade(const uint32_t aActor, const bool aPoison, const Clock::time_point aNow) noexcept
    {
        auto& state = Get(aActor);
        const Made cKind = aPoison ? Made::Poison : Made::Potion;
        if (state.HasIngredient && Within(state.IngredientAt, aNow))
            return Complete(state, cKind);
        state.MadeKind = cKind;
        state.MadeAt = aNow;
        return Made::None;
    }

    [[nodiscard]] size_t Size() const noexcept { return m_states.size(); }

private:
    struct State
    {
        Clock::time_point IngredientAt{};
        Clock::time_point MadeAt{};
        Clock::time_point EatenAt{};
        uint32_t IngredientItem{};
        uint32_t EatenItem{};
        Made MadeKind = Made::None;
        bool HasIngredient = false;
        bool HasEaten = false;
    };

    static bool Within(const Clock::time_point aThen, const Clock::time_point aNow) noexcept { return aNow >= aThen && aNow - aThen <= kTogether; }

    //! One sound per making: the other ingredients of the same making play nothing more.
    static Made Complete(State& aState, const Made aKind) noexcept
    {
        aState.HasIngredient = false;
        aState.MadeKind = Made::None;
        return aKind;
    }

    State& Get(const uint32_t aActor) noexcept
    {
        if (m_states.size() >= kMaxTracked && !m_states.contains(aActor))
            m_states.clear();
        return m_states[aActor];
    }

    std::unordered_map<uint32_t, State> m_states;
};

//! The copy is near enough to the player here to be heard.
inline bool Heard(const float aDistance) noexcept
{
    return aDistance <= kHearWithin;
}
} // namespace AlchemyMadeRules
