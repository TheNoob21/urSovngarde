#pragma once

#include <cstddef>
#include <cstdint>

#include <FollowerCommand.h>

// How add-ons' follower answers (UrGameFollowerPolicy) are merged with core's own rule, with no game in them.
namespace FollowerPolicy
{
constexpr uint32_t kDefer = 0;
constexpr uint32_t kFollower = 1;
constexpr uint32_t kNotFollower = 2;

//! The first add-on answer that decides, in registration order; anything but 1 or 2 defers.
constexpr uint32_t Merge(const uint32_t* apAnswers, std::size_t aCount) noexcept
{
    for (std::size_t i = 0; i < aCount; ++i)
        if (apAnswers[i] == kFollower || apAnswers[i] == kNotFollower)
            return apAnswers[i];
    return kDefer;
}

//! Whether the actor is this player's follower: the add-ons' merged answer, else core's own rule.
constexpr bool Decide(uint32_t aMerged, bool aCoreRule) noexcept
{
    return aMerged == kFollower ? true : aMerged == kNotFollower ? false : aCoreRule;
}

static_assert(FollowerCommand::kDone == 1 && FollowerCommand::kBusy == 2 && FollowerCommand::kGone == 4, "UrGameFollowerPolicy::FollowerWord's replies");

//! An add-on's answer to a word: kept only when it is a reply a follower gives (done, busy, gone); anything else is not handled (0).
constexpr uint8_t WordReply(uint32_t aAnswer) noexcept
{
    return aAnswer == 1 || aAnswer == 2 || aAnswer == 4 ? static_cast<uint8_t>(aAnswer) : 0;
}
} // namespace FollowerPolicy
