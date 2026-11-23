#pragma once

#include "NiLayout.h"
#include <type_traits>
#include <utility>
#include <TiltedCore/Stl.hpp>

namespace NiTree
{
inline constexpr size_t kAsNodeSlot = 3; // NiObject::AsNode

inline void* AsNode(void* apObject) noexcept
{
    using TAsNode = void*(__fastcall*)(void*);
    auto** ppVTable = *static_cast<void***>(apObject);
    return static_cast<TAsNode>(ppVTable[kAsNodeSlot])(apObject);
}

/**
 * Where a node keeps its children: an array of slots, some of them empty.
 */
struct ChildSlots
{
    void** pSlots = nullptr; // the array; null when the node has none
    uint16_t Count = 0;      // how many slots the array has, empty ones included
};

/**
 * Read where a node keeps its children.
 * A plain read, cheap enough for every frame; the node watches read the same array through NodeWatch::ReadChildSlots, guarded, as their nodes may be freed.
 *
 * @param apNode A node, as AsNode gives it.
 * @return Its slot array, null when it has none, and how many slots it says it has.
 */
inline ChildSlots ChildSlotsOf(void* apNode) noexcept
{
    auto* pBytes = static_cast<uint8_t*>(apNode);
    return {*reinterpret_cast<void***>(pBytes + NiLayout::kChildSlotsOffset), *reinterpret_cast<uint16_t*>(pBytes + NiLayout::kChildCountOffset)};
}

/**
 * Call a visitor, and say whether the walk goes on.
 * A visitor that returns nothing never stops it; one that returns a bool stops it with false.
 *
 * @param aVisit The visitor.
 * @param aArguments What the visitor is called with: the object, and for some walks its node.
 * @return False when the visitor asked to stop.
 */
template <class TVisit, class... TArguments> bool VisitGoesOn(TVisit& aVisit, TArguments... aArguments) noexcept
{
    if constexpr (std::is_same_v<std::invoke_result_t<TVisit&, TArguments...>, bool>)
        return aVisit(aArguments...);
    else
    {
        aVisit(aArguments...);
        return true;
    }
}

/**
 * Call a visitor on each child of a node, in slot order, skipping empty slots.
 *
 * @param apNode A node, as AsNode gives it.
 * @param aVisit Called with each child; it may return false to stop before the next one.
 * @return False when the visitor stopped it, true when every child was visited.
 */
template <class TVisit> bool ForEachChild(void* apNode, TVisit&& aVisit) noexcept
{
    const ChildSlots cChildren = ChildSlotsOf(apNode);
    for (uint16_t i = 0; cChildren.pSlots && i < cChildren.Count; ++i)
        if (cChildren.pSlots[i] && !VisitGoesOn(aVisit, cChildren.pSlots[i]))
            return false;
    return true;
}

/**
 * Visit a tree of nodes breadth first: the root, then its children, then theirs, each in slot order.
 * A visitor that takes one object is called before the walk asks the object for its node.
 * A visitor that takes the object and its node (null for one that is not a node) gets the node the walk asked for, so it need not ask again.
 *
 * @param apRoot The tree's root; null visits nothing.
 * @param aMaxObjects The most objects visited, so that a broken tree cannot keep the walk going.
 * @param aVisit Called with each object; it may return false to end the walk at once.
 */
template <class TVisit> void VisitTree(void* apRoot, const size_t aMaxObjects, TVisit&& aVisit) noexcept
{
    if (!apRoot)
        return;

    TiltedPhoques::Vector<void*> queue;
    queue.push_back(apRoot);
    for (size_t head = 0; head < queue.size() && head < aMaxObjects; ++head)
    {
        void* pObject = queue[head];
        void* pNode = nullptr;
        if constexpr (std::is_invocable_v<TVisit&, void*, void*>)
        {
            pNode = AsNode(pObject);
            if (!VisitGoesOn(aVisit, pObject, pNode))
                return;
        }
        else
        {
            if (!VisitGoesOn(aVisit, pObject))
                return;
            pNode = AsNode(pObject);
        }
        if (pNode)
            ForEachChild(pNode, [&queue](void* pChild) { queue.push_back(pChild); });
    }
}

/**
 * Visit a tree of nodes as VisitTree does with a visitor that takes one object, but not its root.
 * The root still counts towards the limit, as the walk passes through it.
 *
 * @param apRoot The tree's root; null visits nothing.
 * @param aMaxObjects The most objects walked through, the root included.
 * @param aVisit Called with each object below the root; it may return false to end the walk at once.
 */
template <class TVisit> void VisitBelow(void* apRoot, const size_t aMaxObjects, TVisit&& aVisit) noexcept
{
    bool atRoot = true;
    VisitTree(apRoot, aMaxObjects, [&](void* pObject) { return std::exchange(atRoot, false) || VisitGoesOn(aVisit, pObject); });
}

} // namespace NiTree
