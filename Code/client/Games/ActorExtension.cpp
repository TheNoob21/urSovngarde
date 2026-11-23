#include <TiltedOnlinePCH.h>

#include <Games/ActorExtension.h>

bool ActorExtension::IsRemote() const noexcept
{
    return onlineFlags & kRemote;
}

bool ActorExtension::IsLocal() const noexcept
{
    return !IsRemote();
}

bool ActorExtension::IsPlayer() const noexcept
{
    return onlineFlags & kPlayer;
}

bool ActorExtension::IsRemotePlayer() const noexcept
{
    return IsRemote() && IsPlayer();
}

bool ActorExtension::IsLocalPlayer() const noexcept
{
    return IsLocal() && IsPlayer();
}

bool ActorExtension::IsFollowerCopy() const noexcept
{
    return onlineFlags & kFollowerCopy;
}

bool ActorExtension::IsOthersFollower() const noexcept
{
    return (onlineFlags & (kFollowerCopy | kOthersFollower)) != 0;
}

bool ActorExtension::IsHiddenHere() const noexcept
{
    return onlineFlags & kHiddenHere;
}

bool ActorExtension::IsOtherGame() const noexcept
{
    return onlineFlags & kOtherGame;
}

void ActorExtension::SetRemote(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kRemote;
    else
        onlineFlags &= ~kRemote;
}

void ActorExtension::SetPlayer(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kPlayer;
    else
        onlineFlags &= ~kPlayer;
}

void ActorExtension::SetFollowerCopy(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kFollowerCopy;
    else
        onlineFlags &= ~kFollowerCopy;
}

void ActorExtension::SetOthersFollower(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kOthersFollower;
    else
        onlineFlags &= ~kOthersFollower;
}

void ActorExtension::SetHiddenHere(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kHiddenHere;
    else
        onlineFlags &= ~kHiddenHere;
}

void ActorExtension::ForgetCopyBleedout() noexcept
{
    OwnerWordOfCopy.store(0, std::memory_order_relaxed);
    KneelsInGraph.store(false, std::memory_order_relaxed);
    EngineStopPlayed.store(0, std::memory_order_relaxed);
    OwnerFallTick.store(0, std::memory_order_relaxed);
}

void ActorExtension::SetOtherGame(bool aSet) noexcept
{
    if (aSet)
        onlineFlags |= kOtherGame;
    else
        onlineFlags &= ~kOtherGame;
}
