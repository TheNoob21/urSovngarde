#pragma once

#include <GameApi.h>
#include <glm/vec3.hpp>

#include <string_view>
struct GameId;
struct VRPose;
struct Actor;
namespace GameExtension
{
bool Chat(std::string_view acMessage);
void Module(uint32_t aId, const char* apFilename);
bool RejectMagic(const GameId& acForm) noexcept;
bool LocalMagic(uint32_t aForm) noexcept;
bool AllowsEffect(uint32_t aArmor, uint32_t aSlot, uint32_t aSpell, uint32_t aEffect) noexcept;
bool SharedSwitch(uint32_t aReference, uint32_t aBase, uint32_t aType) noexcept;
// The add-ons' merged word on whether an actor is this player's follower (FollowerPolicy): 0 none, 1 follower, 2 not.
uint32_t FollowerAnswer(uint32_t aActorForm, uint32_t aBaseForm, bool aTeammate) noexcept;
// An add-on's handling of another player's word for a follower the game's follower quest does not hold: 0 not handled, else the reply.
uint8_t FollowerWord(uint32_t aActorForm, uint32_t aCommand) noexcept;
bool ChecksMagic() noexcept;
void OnCreate(uint32_t aHandle) noexcept;
bool OnActivate(uint32_t aReference, uint32_t aActivator) noexcept;
void OnFrameEnd() noexcept;
void OnProjectile(const UrGameProjectile& acLaunch) noexcept;
void OnGameStep() noexcept;
bool CapturesRagdoll(uint32_t aForm, const void* apRoot) noexcept;
uint32_t TakeObjects(UrGameObjectObservation* apOut, uint32_t aCapacity) noexcept;
bool HasReactionObservers() noexcept;
void Unpaired(uint32_t aForm, uint32_t aKind, float aMagnitude, float aDirection) noexcept;
uint32_t GraphReaction(const char* apName) noexcept;
bool HasTouchObservers() noexcept;
uint32_t SelectTouch(const UrGameTouch* apPrevious, const UrGameTouch* apCurrent, uint32_t aHasPoint) noexcept;
void Impact(uint32_t aForm, uint32_t aSide, float aSpeed, uint64_t aTick, const float aPoint[3]) noexcept;
bool WantsFallbackPose() noexcept;
void CaptureFallbackPose(const uint32_t aBones[7]) noexcept;
bool Reserved(uint32_t aForm) noexcept;
bool BeforeDelete(uint32_t aForm) noexcept;
bool ReleaseFurnitureSeat(Actor* apActor) noexcept;
void PresentNode(uint32_t aForm, uint32_t aSide, const void* apRoot, const void* apNode, void* apContext, UrCommitPose aCommit) noexcept;
bool ReadPose(uint32_t aServerId, uint32_t aEpoch, uint64_t aTick, glm::vec3& aPosition, float& aHeading, VRPose& aPose) noexcept;
} // namespace GameExtension
