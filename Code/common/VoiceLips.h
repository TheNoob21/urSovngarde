#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// How far another player's copy opens its mouth while their voice plays here, with no game in it (VoiceService).
// The voice already arrives for playing, so nothing more is sent: the loudness of the stretch now being heard opens the mouth.
namespace VoiceLips
{
// The stretch of voice the mouth follows.
constexpr uint32_t kWindowMs = 40;
// Quieter than this is a closed mouth (room noise); this loud or louder opens it fully. In decibels below full scale.
constexpr float kQuietDb = -45.f;
constexpr float kLoudDb = -12.f;
// The widest the mouth opens, as a phoneme weight; fully open looks like a yawn.
constexpr float kWidest = 0.7f;
// How fast the mouth follows: it opens quicker than it closes, as a jaw does.
constexpr float kOpenPerSecond = 18.f;
constexpr float kClosePerSecond = 9.f;
// The phoneme the mouth opens with (Aah).
constexpr uint32_t kPhoneme = 0;

//! The loudness of aCount samples from aStart (as far as there are), 0 for silence to 1 for full scale.
inline float Loudness(const int16_t* apSamples, const size_t aSize, const size_t aStart, const size_t aCount) noexcept
{
    if (!apSamples || aStart >= aSize || aCount == 0)
        return 0.f;
    const size_t cEnd = std::min(aSize, aStart + aCount);
    double sum = 0.0;
    for (size_t i = aStart; i < cEnd; ++i)
    {
        const double cSample = apSamples[i] / 32768.0;
        sum += cSample * cSample;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(cEnd - aStart)));
}

//! How far the mouth should be open for a loudness: 0 closed to kWidest.
inline float Openness(const float aLoudness) noexcept
{
    if (!std::isfinite(aLoudness) || aLoudness <= 0.f)
        return 0.f;
    const float cDb = 20.f * std::log10(aLoudness);
    return std::clamp((cDb - kQuietDb) / (kLoudDb - kQuietDb), 0.f, 1.f) * kWidest;
}

//! The mouth aSeconds later, moving from aNow towards aTarget.
inline float Follow(const float aNow, const float aTarget, const float aSeconds) noexcept
{
    const float cSeconds = std::isfinite(aSeconds) ? std::clamp(aSeconds, 0.f, 0.25f) : 0.f;
    const float cTarget = std::isfinite(aTarget) ? std::clamp(aTarget, 0.f, kWidest) : 0.f;
    const float cStep = (cTarget > aNow ? kOpenPerSecond : kClosePerSecond) * cSeconds * kWidest;
    if (std::abs(cTarget - aNow) <= cStep)
        return cTarget;
    return aNow + (cTarget > aNow ? cStep : -cStep);
}
} // namespace VoiceLips
