#pragma once

#include <TiltedCore/Stl.hpp>

namespace PluginRules
{
struct LeftOut
{
    TiltedPhoques::Vector<TiltedPhoques::String> Here;
    TiltedPhoques::Vector<TiltedPhoques::String> There;
};

// Apply the same personal-plugin rule used during admission to NPC plugin warnings.
LeftOut FilterForPair(
    TiltedPhoques::Vector<TiltedPhoques::String>& aOnlyHere, bool aHereFlat, TiltedPhoques::Vector<TiltedPhoques::String>& aOnlyThere, bool aThereFlat,
    const TiltedPhoques::Vector<TiltedPhoques::String>& acPersonal) noexcept;
} // namespace PluginRules
