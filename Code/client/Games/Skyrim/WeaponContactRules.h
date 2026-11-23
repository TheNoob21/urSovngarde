#pragma once

#include <array>
#include <cstddef>

namespace WeaponContactRules
{
template <class Sample, class Capture, class Accept>
void OnContactPoint(const unsigned char* apEvent, size_t aBodiesOffset, size_t aPointOffset, Capture aCapture, Accept aAccept) noexcept
{
    if (!apEvent)
        return;
    Sample sample{};
    for (size_t i = 0; i < 2; ++i)
    {
        sample.pBodies[i] = *reinterpret_cast<void* const*>(apEvent + aBodiesOffset + i * sizeof(void*));
        if (!sample.pBodies[i])
            return;
        aCapture(sample, i);
    }
    if (const float* pPoint = *reinterpret_cast<const float* const*>(apEvent + aPointOffset))
        sample.Point = {pPoint[0], pPoint[1], pPoint[2]};
    aAccept(sample);
}

template <class Sample, size_t N> bool Record(std::array<Sample, N>& aSamples, size_t& aCount, const Sample& acSample) noexcept
{
    for (size_t i = 0; i < aCount; ++i)
        if (aSamples[i].pBodies[0] == acSample.pBodies[0] && aSamples[i].pBodies[1] == acSample.pBodies[1])
        {
            aSamples[i].Point = acSample.Point;
            return true;
        }
    if (aCount == N)
        return false;
    aSamples[aCount++] = acSample;
    return true;
}
} // namespace WeaponContactRules
