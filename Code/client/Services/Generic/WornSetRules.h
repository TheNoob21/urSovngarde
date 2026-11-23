#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

//! What a remote copy takes off and puts on to wear what its owner wears.
//!
//! The owner's equipment snapshot lists the worn pieces its game could name; a piece from a plugin the server does not know
//! never reaches it, so a missing piece proves nothing. A change made while its player was out of range travels nowhere else.
//! These say which pieces differ. They are pure functions of plain values, so they can be tested (see tests/worn_set_rules.cpp).
namespace WornSetRules
{
//! A worn armor piece: its form id and the biped slots it covers.
struct Piece
{
    uint32_t Id{};
    uint32_t Slots{};
};

struct Changes
{
    std::vector<uint32_t> Remove;
    std::vector<uint32_t> Add;
};

//! aWanted: the pieces the owner wears. aWorn: the pieces the copy wears.
//! aLastWanted: the ids its owner was last known to wear (its spawn data, then each snapshot this game applied).
//! A piece is put on when the copy lacks it. A piece of the copy comes off when a piece put on takes its slot,
//! or when its owner wore it before and wears it no more: taken off, or changed for one this game cannot name.
//! A piece its owner was never known to wear stays when nothing replaces it: an NPC's own outfit here, where its owner
//! may wear a piece this game cannot name.
inline Changes Compare(const std::vector<Piece>& acWanted, const std::vector<Piece>& acWorn, const std::vector<uint32_t>& acLastWanted = {})
{
    const auto cHas = [](const std::vector<Piece>& acList, const uint32_t aId)
    {
        return std::any_of(acList.begin(), acList.end(), [aId](const Piece& acPiece) { return acPiece.Id == aId; });
    };

    Changes changes;
    uint32_t addedSlots = 0;
    for (const Piece& cPiece : acWanted)
    {
        if (cHas(acWorn, cPiece.Id) || std::find(changes.Add.begin(), changes.Add.end(), cPiece.Id) != changes.Add.end())
            continue;
        changes.Add.push_back(cPiece.Id);
        addedSlots |= cPiece.Slots;
    }
    for (const Piece& cPiece : acWorn)
    {
        const bool cWornBefore = std::find(acLastWanted.begin(), acLastWanted.end(), cPiece.Id) != acLastWanted.end();
        if (!cHas(acWanted, cPiece.Id) && ((cPiece.Slots & addedSlots) != 0 || cWornBefore) &&
            std::find(changes.Remove.begin(), changes.Remove.end(), cPiece.Id) == changes.Remove.end())
            changes.Remove.push_back(cPiece.Id);
    }
    return changes;
}
} // namespace WornSetRules
