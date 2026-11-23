#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <TiltedCore/Stl.hpp>

using TiltedPhoques::String;

//! The server's ids for plugins, and how two plugin lists are compared.
//! A plugin file name is the same plugin whatever its case: Skyrim and its mod managers treat names that way, and a
//! game can be installed with "SkyUI_SE.esp" where another has "skyui_se.esp".
namespace ModIdTable
{
inline bool SameName(const String& acLeft, const String& acRight) noexcept
{
    return acLeft.size() == acRight.size() && std::equal(
                                                  acLeft.begin(), acLeft.end(), acRight.begin(), [](const char aLeft, const char aRight)
                                                  { return std::tolower(static_cast<unsigned char>(aLeft)) == std::tolower(static_cast<unsigned char>(aRight)); });
}

//! The plugins of aFrom that aNotIn does not have.
inline TiltedPhoques::Vector<String> OnlyIn(const TiltedPhoques::Vector<String>& acFrom, const TiltedPhoques::Vector<String>& acNotIn) noexcept
{
    TiltedPhoques::Vector<String> only;
    for (const String& plugin : acFrom)
        if (std::none_of(acNotIn.begin(), acNotIn.end(), [&plugin](const String& acOther) { return SameName(plugin, acOther); }))
            only.push_back(plugin);
    return only;
}

struct Entry
{
    uint32_t id;
    uint32_t refCount;
};

using TModList = TiltedPhoques::Map<String, Entry>;

//! The entry of a plugin by name, or the end of the list.
inline TModList::iterator Find(TModList& aList, const String& acFilename) noexcept
{
    return std::find_if(aList.begin(), aList.end(), [&](const TModList::value_type& aEntry) { return SameName(aEntry.first, acFilename); });
}

//! One id for a plugin, whether a game has it light (ESL) or normal: SE and VR do not always agree on that, and a plugin with
//! two ids cannot be matched between them. The id is in the list of the kind the game has it.
inline uint32_t Add(TModList& aList, TModList& aOther, const String& acFilename, uint32_t& aSeed) noexcept
{
    const auto itor = Find(aList, acFilename);
    if (itor != aList.end())
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto other = Find(aOther, acFilename);
    const uint32_t id = other != aOther.end() ? other->second.id : aSeed++;
    aList.emplace(acFilename, Entry{id, 1});
    return id;
}
} // namespace ModIdTable
