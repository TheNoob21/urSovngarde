#include <catch2/catch.hpp>

#include <VoiceLips.h>

#include <cmath>
#include <limits>
#include <vector>

namespace
{
// A tone of aPeak (full scale 32767) for aCount samples.
std::vector<int16_t> Tone(const int16_t aPeak, const size_t aCount)
{
    std::vector<int16_t> samples(aCount);
    for (size_t i = 0; i < aCount; ++i)
        samples[i] = static_cast<int16_t>(aPeak * std::sin(static_cast<double>(i) * 0.3));
    return samples;
}
} // namespace

TEST_CASE("Voice loudness reads the stretch now playing, and nothing past the buffer", "[voicelips]")
{
    using namespace VoiceLips;

    std::vector<int16_t> samples(960, 0);
    const auto cLoud = Tone(16000, 480);
    std::copy(cLoud.begin(), cLoud.end(), samples.begin() + 480);

    CHECK(Loudness(samples.data(), samples.size(), 0, 480) == 0.f);
    CHECK(Loudness(samples.data(), samples.size(), 480, 480) == Approx(16000.0 / 32768.0 / std::sqrt(2.0)).margin(0.02));
    // A window running past the end reads only what is there.
    CHECK(Loudness(samples.data(), samples.size(), 900, 480) > 0.2f);
    CHECK(Loudness(samples.data(), samples.size(), 960, 480) == 0.f);
    CHECK(Loudness(nullptr, 10, 0, 10) == 0.f);
    CHECK(Loudness(samples.data(), samples.size(), 0, 0) == 0.f);
}

TEST_CASE("Room noise keeps the mouth shut, speech opens it, and it never opens past the widest", "[voicelips]")
{
    using namespace VoiceLips;

    CHECK(Openness(0.f) == 0.f);
    CHECK(Openness(-1.f) == 0.f);
    CHECK(Openness(std::numeric_limits<float>::quiet_NaN()) == 0.f);
    CHECK(Openness(std::numeric_limits<float>::infinity()) == 0.f);
    // -50 dB: a quiet room.
    CHECK(Openness(0.00316f) == 0.f);
    // -24 dB: ordinary speech.
    const float cSpeech = Openness(0.063f);
    CHECK(cSpeech > 0.2f);
    CHECK(cSpeech < kWidest);
    CHECK(Openness(1.f) == kWidest);
    CHECK(Openness(0.5f) == kWidest);
    // Louder opens wider.
    CHECK(Openness(0.03f) < Openness(0.1f));
}

TEST_CASE("The mouth opens quicker than it closes and settles on its target", "[voicelips]")
{
    using namespace VoiceLips;

    const float cOpened = Follow(0.f, kWidest, 0.02f);
    const float cClosed = kWidest - Follow(kWidest, 0.f, 0.02f);
    CHECK(cOpened > 0.f);
    CHECK(cOpened > cClosed);

    float mouth = 0.f;
    for (int i = 0; i < 30; ++i)
        mouth = Follow(mouth, 0.4f, 1.f / 60.f);
    CHECK(mouth == 0.4f);
    for (int i = 0; i < 60; ++i)
        mouth = Follow(mouth, 0.f, 1.f / 60.f);
    CHECK(mouth == 0.f);

    // A long hitch or a bad clock moves it no further than a quarter second would.
    CHECK(Follow(kWidest, 0.f, 10.f) == Follow(kWidest, 0.f, 0.25f));
    CHECK(Follow(0.f, 0.f, 10.f) == 0.f);
    CHECK(Follow(0.f, kWidest, 0.001f) < Follow(0.f, kWidest, 0.01f));
    CHECK(Follow(0.2f, kWidest, std::numeric_limits<float>::quiet_NaN()) == 0.2f);
    CHECK(Follow(0.2f, 0.2f, -1.f) == 0.2f);
    // A target past the widest is held at the widest.
    CHECK(Follow(kWidest, 5.f, 0.1f) == kWidest);
}
