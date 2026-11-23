#pragma once

#include <cstdint>
#include <vector>

// A merchant's wares and gold are kept in its vendor faction's chest, often outside the shop: under a stall, or in a holding cell.
// Only the containers of the player's own cell are shared on arrival, so the chest of the merchant spoken to is shared while trading.
namespace VendorChest
{
struct Membership
{
    bool IsVendor{};
    int8_t Rank{};
    uint32_t ChestId{};
};

// The chest the merchant trades from: that of its one vendor faction with a chest. 0 when there is none, or two different ones.
// A rank below zero is not a member.
inline uint32_t PickChest(const std::vector<Membership>& acMemberships) noexcept
{
    uint32_t chestId = 0;
    for (const Membership& cMembership : acMemberships)
    {
        if (!cMembership.IsVendor || cMembership.Rank < 0 || !cMembership.ChestId)
            continue;
        if (chestId && chestId != cMembership.ChestId)
            return 0;
        chestId = cMembership.ChestId;
    }
    return chestId;
}

// One player's trade with one merchant, seen once a frame.
class Trade
{
public:
    enum class Step
    {
        kNothing,
        // Share the chest: it is not shared yet.
        kRegister,
        // The barter menu has just opened: if the game restocked the chest on opening it, send all of it.
        // RunFrame passes it on only when the chest changed.
        kCheckRestock,
        // The barter menu has just closed after the other game replaced the chest during it: send all of it, this game's trades included.
        kSendAll,
        // The barter menu has just closed: this game's trades went out one by one, so the chest as it is now is what the next opening compares with.
        kBarterClosed,
    };

    struct Action
    {
        Step What{Step::kNothing};
        uint32_t ChestId{};
    };

    // aChestId: the chest of the merchant spoken to, 0 when none. aShared: that chest has a server entity here.
    Action Update(uint32_t aChestId, bool aBarterOpen, bool aShared) noexcept
    {
        if (aChestId != m_chestId)
        {
            // The trade ended with the menus closing together: the chest this game kept still goes out.
            const Action cLeft{m_replacedDuringBarter ? Step::kSendAll : Step::kNothing, m_chestId};
            m_chestId = aChestId;
            m_registerSent = false;
            m_barterOpen = false;
            m_restockChecked = false;
            m_replacedDuringBarter = false;
            if (cLeft.What != Step::kNothing)
                return cLeft;
        }

        const bool cClosed = m_barterOpen && !aBarterOpen;
        m_barterOpen = aBarterOpen;

        if (!m_chestId)
            return {};

        if (!aShared)
        {
            if (m_registerSent)
                return {};
            m_registerSent = true;
            return {Step::kRegister, m_chestId};
        }

        if (cClosed)
        {
            m_restockChecked = false;
            if (m_replacedDuringBarter)
            {
                m_replacedDuringBarter = false;
                return {Step::kSendAll, m_chestId};
            }
            return {Step::kBarterClosed, m_chestId};
        }

        if (aBarterOpen && !m_restockChecked)
        {
            m_restockChecked = true;
            return {Step::kCheckRestock, m_chestId};
        }
        return {};
    }

    // Whether the other game's replacement of this chest must wait: the barter menu here shows the chest as it was.
    bool Defer(uint32_t aChestId) noexcept
    {
        if (!m_barterOpen || !aChestId || aChestId != m_chestId)
            return false;
        m_replacedDuringBarter = true;
        return true;
    }

    uint32_t ChestId() const noexcept { return m_chestId; }

private:
    uint32_t m_chestId{};
    bool m_registerSent{};
    bool m_barterOpen{};
    bool m_restockChecked{};
    bool m_replacedDuringBarter{};
};

// One frame of a trade, with its restock check. aBase: the chest as the games last agreed on it; the caller sets it when it shares,
// applies or sends the chest, and it is set here when the barter menu closes, never while the dialogue goes on: the game may restock
// the chest a frame before the barter menu reports itself open. acRead(aChestId): the chest's contents here now, read only when needed.
template <class TContents, class TRead> Trade::Action RunFrame(Trade& aTrade, TContents& aBase, const TRead& acRead, uint32_t aChestId, bool aBarterOpen, bool aShared)
{
    Trade::Action action = aTrade.Update(aChestId, aBarterOpen, aShared);
    if (action.What == Trade::Step::kCheckRestock && acRead(action.ChestId) == aBase)
        action.What = Trade::Step::kNothing;
    else if (action.What == Trade::Step::kBarterClosed)
    {
        aBase = acRead(action.ChestId);
        action.What = Trade::Step::kNothing;
    }
    return action;
}
} // namespace VendorChest
