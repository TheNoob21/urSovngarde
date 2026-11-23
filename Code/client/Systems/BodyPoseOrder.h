#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// A rig supplies actual descendant relationships; IDs and output retain wire indices.
namespace BodyPoseOrder
{
struct Identity
{
    const void* Node = nullptr;
    const void* Entry = nullptr;
    bool Present() const noexcept { return Node || Entry; }
};
inline bool Matches(const Identity& acLeft, const Identity& acRight) noexcept
{
    return (acLeft.Node && acLeft.Node == acRight.Node) || (acLeft.Entry && acLeft.Entry == acRight.Entry);
}

template <std::size_t N> struct Graph
{
    std::array<Identity, N> Bones{};
    std::array<std::array<bool, N>, N> Descendants{};

    // Unposed descendants are ignored. A node/entry pair matching different posed bones is ambiguous.
    bool AddDescendant(std::size_t aParent, Identity aChild) noexcept
    {
        std::size_t match = N;
        for (std::size_t i = 0; i < N; ++i)
            if (Matches(Bones[i], aChild))
            {
                if (match != N && match != i)
                    return false;
                match = i;
            }
        if (match != N)
            Descendants[aParent][match] = true;
        return true;
    }

    // Stable topological order, lowest wire index among the available bones. Missing bones are omitted.
    // Reject aliases and cycles before returning any usable order.
    bool Sort(std::array<std::uint8_t, N>& aOrder, std::size_t& aCount) const noexcept
    {
        aCount = 0;
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = i + 1; j < N; ++j)
                if (Matches(Bones[i], Bones[j]))
                    return false;
        std::array<bool, N> done{};
        std::array<std::uint8_t, N> result{};
        std::size_t total = 0;
        for (const auto& bone : Bones)
            if (bone.Present())
                ++total;
        while (aCount < total)
        {
            std::size_t selected = N;
            for (std::size_t i = 0; i < N; ++i)
            {
                if (!Bones[i].Present() || done[i])
                    continue;
                bool blocked = false;
                for (std::size_t parent = 0; parent < N; ++parent)
                    if (Bones[parent].Present() && !done[parent] && Descendants[parent][i])
                    {
                        blocked = true;
                        break;
                    }
                if (!blocked)
                {
                    selected = i;
                    break;
                }
            }
            if (selected == N)
            {
                aCount = 0;
                return false;
            }
            result[aCount++] = static_cast<std::uint8_t>(selected);
            done[selected] = true;
        }
        aOrder = result;
        return true;
    }
};
} // namespace BodyPoseOrder
