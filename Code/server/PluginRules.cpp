#include "PluginRules.h"

#include <ModContent.h>

namespace PluginRules
{
namespace
{
TiltedPhoques::Vector<TiltedPhoques::String> RemoveExempt(
    TiltedPhoques::Vector<TiltedPhoques::String>& aPlugins, const TiltedPhoques::Vector<TiltedPhoques::String>& acPersonal, ModContent::Edition aHere, ModContent::Edition aThere)
{
    TiltedPhoques::Vector<TiltedPhoques::String> removed, kept;
    for (auto& plugin : aPlugins)
    {
        if (ModContent::IsPersonal(plugin, acPersonal) || ModContent::IsBundledSEOnly(plugin, aHere, aThere))
            removed.push_back(std::move(plugin));
        else
            kept.push_back(std::move(plugin));
    }
    aPlugins = std::move(kept);
    return removed;
}
} // namespace

LeftOut FilterForPair(
    TiltedPhoques::Vector<TiltedPhoques::String>& aOnlyHere, bool aHereFlat, TiltedPhoques::Vector<TiltedPhoques::String>& aOnlyThere, bool aThereFlat,
    const TiltedPhoques::Vector<TiltedPhoques::String>& acPersonal) noexcept
{
    using ModContent::Edition;
    const auto cHere = aHereFlat ? Edition::kSE : Edition::kVR;
    const auto cThere = aThereFlat ? Edition::kSE : Edition::kVR;
    return {RemoveExempt(aOnlyHere, acPersonal, cHere, cThere), RemoveExempt(aOnlyThere, acPersonal, cThere, cHere)};
}
}
