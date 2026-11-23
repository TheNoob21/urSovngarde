#pragma once

#include <Services/WorldObjectBodies.h>

#include <optional>

struct TESObjectREFR;

// The Havok bodies that hang below a placed object's root: read for the sender, driven with a speed for the receiver.
namespace PropHavok
{
//! The dynamic bodies below the object's 3D root, in the order its tree lists them, posed in the root's space.
//! The root's own body is left out: on a hanging sign it is the fixed bracket. Empty when there are none or too many,
//! Waiting while no fresh sample has been read between frames yet.
WorldObjectBodies::Sample Read(TESObjectREFR* apRef, WorldObjectBodies::Poses& aOut, uint32_t& aLayout) noexcept;

//! How many bodies below the root can swing (not fixed), checking every pointer: 0 for none, too many or no collision at all,
//! -1 while its collision is not in a world yet.
std::optional<int32_t> CountBodies(TESObjectREFR* apRef) noexcept;

void ClearSamples() noexcept;

//! Gives each body the speed that takes it to its target (WorldObjectBodies::LinearToward, AngularToward) plus the target's own speed,
//! and wakes it, under the world's write lock. False when nothing was written: another count of bodies, or a build that cannot write them.
//! aFrameSeconds: the time since the last drive, so a long frame does not overshoot.
bool Drive(TESObjectREFR* apRef, const WorldObjectBodies::Targets& acTargets, uint32_t aLayout, float aFrameSeconds) noexcept;

//! Flat: the swings the client's update drives, which runs on a job thread there; replaces what was queued.
void Queue(std::vector<std::pair<uint32_t, WorldObjectBodies::Driven>> aDrives) noexcept;

//! How a thrown object flies as its 3D root's own body has it: speed in game units a second, spin in radians a second about the
//! world's axes. False when the root has no dynamic body to read. Any thread: only checked reads.
bool ReadFlight(TESObjectREFR* apRef, glm::vec3& aVelocity, glm::vec3& aSpin) noexcept;

//! Gives a thrown object, let go here by another player's hand, that speed and spin once its bodies are dynamic again, from
//! the main loop (RunQueued). Any thread.
void Launch(uint32_t aFormId, const glm::vec3& acVelocity, const glm::vec3& acSpin) noexcept;

//! Drops that speed while it still waits for the bodies: the object was taken back or caught first. Any thread.
void CancelLaunch(uint32_t aFormId) noexcept;

//! Samples and drives queued objects between frames, outside the physics step.
void RunQueued() noexcept;
} // namespace PropHavok
