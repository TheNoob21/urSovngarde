#pragma once

#ifndef SKYRIMVR
#include <cstdint>
#include <Systems/RemotePoseState.h>

struct Actor;

// Exact SE 1.7.104 presentation of remote VR players' bodies: up to eight copies at once, one slot each (RemoteBodySlots).
// Publications contain weak numeric identity hints; native retained-root passes qualify all writes.
namespace SEBodyPresentation
{
// Call on the interpolation/game owner thread, while Actor and its current 3D are live.
// aNowNetworkMs has the SAME local monotonic epoch as Sample endpoint ReceivedAtMs.
// acIdentity: the receiver's pose identity of this copy (session, owner epoch, server id << 32 | FormID). Read only by the
// body commit's slot choice and committed-pose grace: another copy of the same remote player may take its slot over.
// Commit-only grace freezes the last successfully committed immutable inputs for at most 200 ms beyond their
// original source deadline, for the same trusted identity/generation/root/tree in a qualified native retained-root pass.
// Missing samples never renew receive provenance, source freshness or the committed grace deadline.
// After a presented body withdraws, re-entry requires >=750 ms AND >=45 actual BeforePresent callbacks plus
// a fresh eligible source. Each slot's dwell survives Forget/Clear/rebind and all pose generations.
void Publish(Actor* apActor, const RemotePose::Sample& acSample, double aNowNetworkMs, const RemotePose::Identity& acIdentity) noexcept;
// Registration stores weak numeric identity hints only; it does not claim ownership of skeleton lifetime.
// Forget/Clear are lifecycle resets: the caller also invalidates the component's pose generation.
void Forget(uint32_t aFormId) noexcept;
void ForgetForGeneration(uint32_t aFormId, uint64_t aGeneration) noexcept;
void Clear() noexcept;
void Tick() noexcept;
// True while the native commit poses this copy: committed this engine frame or the one before, with no native update of
// it since that the commit did not follow. Every other remote VR player is posed at frame end by VRBodySync, so the two
// never pose the same copy, and a copy the commit stops posing is posed at frame end in that same frame.
bool Presents(uint32_t aFormId) noexcept;
// Advances the presentation-frame clock used by withdrawal/re-entry dwell; reads no skeleton.
void BeforePresent() noexcept;
} // namespace SEBodyPresentation
#endif
