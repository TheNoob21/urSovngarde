#pragma once

#include <array>

#include <Structs/Quaternion_NetQuantize.h>

//! Where each rigid body of a ragdoll is, for a body the humanoid pose cannot bend: a wolf, a chicken, a spider.
//! Bodies are in the order the owner's game finds them from the 3D root; the same skeleton gives the same order on every
//! game, and Layout tells a different one apart (RagdollBodiesPose).
struct RagdollPose
{
    //! At most this many bodies are sent; a message with more fails whole.
    static constexpr size_t kMaxBodies = 32;
    //! One step of Body::Offset, in game units.
    static constexpr float kOffsetStep = 1.f / 8.f;

    struct Body
    {
        bool operator==(const Body& acRhs) const noexcept { return Offset == acRhs.Offset && Rotation == acRhs.Rotation; }
        bool operator!=(const Body& acRhs) const noexcept { return !(*this == acRhs); }

        //! From Root, along the world's axes, in kOffsetStep.
        std::array<int16_t, 3> Offset{};
        //! In the world.
        Quaternion_NetQuantize Rotation{};
    };

    bool operator==(const RagdollPose& acRhs) const noexcept;
    bool operator!=(const RagdollPose& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    //! A message with more bodies than kMaxBodies, or than BodyCount, fails whole (DecodeGuard).
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    //! The owner's clock when the bodies were read, in milliseconds.
    uint64_t Tick{};
    //! Which skeleton the owner's bodies make (RagdollBodiesPose::Layout).
    uint32_t Layout{};
    //! How many bodies the owner's ragdoll has; the first Bodies.size() of them are sent.
    uint8_t BodyCount{};
    //! The 3D root's world position.
    float Root[3]{};
    TiltedPhoques::Vector<Body> Bodies{};
};
