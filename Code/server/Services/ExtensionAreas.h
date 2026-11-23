#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <map>

// The states add-ons leave on references in the world. A state belongs to the world: its owner, the player who
// last wrote it, only bounds how many states one player may hold.
namespace ExtensionAreas
{
constexpr size_t kMaxPerAddon = 512;
constexpr size_t kMaxPerOwner = 128;
constexpr size_t kMaxTotal = 4096;
constexpr size_t kMaxAddons = 32;

// The states aOwner holds.
template <class T> size_t Owned(const T& acAreas, uint32_t aOwner)
{
    size_t count = 0;
    for (const auto& cEntry : acAreas)
        count += cEntry.second.Owner == aOwner;
    return count;
}

// A state that changes hands goes to aSender only while it has room; otherwise it stays with aOwner.
inline uint32_t NextOwner(uint32_t aOwner, uint32_t aSender, size_t aSenderOwned) noexcept
{
    return aOwner != aSender && aSenderOwned < kMaxPerOwner ? aSender : aOwner;
}

// A player leaves: what it left stays for whoever comes near, reconnects or joins later, with no owner.
template <class T> void Leave(T& aAreas, uint32_t aId)
{
    for (auto& entry : aAreas)
    {
        entry.second.Delivered.erase(aId);
        if (entry.second.Owner == aId)
            entry.second.Owner = 0;
    }
}

// Drops one state that has no owner and that no one has now: of aAddon only, or of any add-on when aAddon is 0.
template <class T> bool EvictUnowned(T& aAreas, uint32_t aAddon)
{
    for (auto it = aAreas.begin(); it != aAreas.end(); ++it)
        if ((!aAddon || it->first.first == aAddon) && !it->second.Owner && it->second.Delivered.empty())
        {
            aAreas.erase(it);
            return true;
        }
    return false;
}

// Whether aSender may add a new state for aAddon (never 0). Room comes only from states no one owns or has: one of the
// add-on's own, all of the smallest such add-on's when too many add-ons hold states, or any one when the whole store is full.
template <class T> bool MakeRoom(T& aAreas, uint32_t aAddon, uint32_t aSender)
{
    if (Owned(aAreas, aSender) >= kMaxPerOwner)
        return false;
    size_t addonCount = 0;
    struct Holding
    {
        size_t Count{};
        bool Free{true};
    };
    std::map<uint32_t, Holding> addons;
    for (const auto& cEntry : aAreas)
    {
        addonCount += cEntry.first.first == aAddon;
        auto& holding = addons[cEntry.first.first];
        ++holding.Count;
        holding.Free = holding.Free && !cEntry.second.Owner && cEntry.second.Delivered.empty();
    }
    if (addonCount >= kMaxPerAddon && !EvictUnowned(aAreas, aAddon))
        return false;
    if (!addonCount && addons.size() >= kMaxAddons)
    {
        auto smallest = addons.end();
        for (auto it = addons.begin(); it != addons.end(); ++it)
            if (it->second.Free && (smallest == addons.end() || it->second.Count < smallest->second.Count))
                smallest = it;
        if (smallest == addons.end())
            return false;
        std::erase_if(aAreas, [&](const auto& acEntry) { return acEntry.first.first == smallest->first; });
    }
    return aAreas.size() < kMaxTotal || EvictUnowned(aAreas, 0);
}
} // namespace ExtensionAreas
