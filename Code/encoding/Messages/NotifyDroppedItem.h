#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>
#include <TiltedCore/Stl.hpp>

//! An item lying in the world: the receiver adopts the one already on its floor, or places it.
//! Sent when an item is dropped, and again to anyone entering its cell.
struct NotifyDroppedItem final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDroppedItem;

    NotifyDroppedItem()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDroppedItem& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && Item == acRhs.Item && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId &&
               Position == acRhs.Position && Rotation == acRhs.Rotation && RequestId == acRhs.RequestId && LegacyItemData == acRhs.LegacyItemData && Replay == acRhs.Replay &&
               Fixed == acRhs.Fixed && EarlierPlaces == acRhs.EarlierPlaces;
    }

    uint32_t Id{};
    Inventory::Entry Item{};
    GameId CellId{};
    GameId WorldSpaceId{};
    Vector3_NetQuantize Position{};
    //! Euler angles in radians, as the reference holds them.
    //! Plain floats: the packed position type is only about 1 unit precise, too coarse for an angle.
    glm::vec3 Rotation{};
    //! The add's RequestId, in the copy sent to the client that added the item; 0 in every other copy.
    uint32_t RequestId{};
    //! A v1/v2 server ledger did not record ItemData; match only its known fields until a saved copy enriches it.
    bool LegacyItemData{};
    //! A known or reannounced item can already have a copy in this save; a fresh peer drop cannot.
    bool Replay{};
    //! Left held still where it lies by a script (a weapon rack's weapon): placed there and kept out of physics. Appended last.
    bool Fixed{};
    //! A message with more earlier places than this is damaged and dropped whole (DecodeGuard); the server keeps four.
    static constexpr uint64_t kMaxEarlierPlaces = 8;
    //! Where it rested before it was moved away, first where it was dropped: a save made while it lay there still has it there,
    //! and its copy is found there and moved here instead of a second one being placed. Appended last.
    TiltedPhoques::Vector<Vector3_NetQuantize> EarlierPlaces{};
};
