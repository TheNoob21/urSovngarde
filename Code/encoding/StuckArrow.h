#pragma once

#include <cstddef>

//! What a stuck arrow's message may carry (RequestStuckArrow, NotifyStuckArrow).
namespace StuckArrow
{
//! Longest bone name of a skeleton the game ships is well below this; a longer one is a damaged or crafted message.
constexpr size_t kMaxBoneNameLength = 64;
} // namespace StuckArrow
