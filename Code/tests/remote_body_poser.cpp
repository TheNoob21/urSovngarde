#include <catch2/catch.hpp>

#include <Systems/RemoteBodyPoser.h>

#include <array>
#include <cstdint>

namespace
{
// The copies frame end poses, out of aCopies, given the native commit's word for each slot.
template <size_t N, size_t S> size_t PosedAtFrameEnd(const std::array<uint32_t, N>& acCopies, const std::array<uint64_t, S>& acWords, const uint32_t aFrame)
{
    size_t posed = 0;
    for (const uint32_t cActor : acCopies)
    {
        bool native = false;
        for (const uint64_t cWord : acWords)
            native = native || RemoteBodyPoser::NativeCommitPoses(cWord, cActor, aFrame);
        posed += !native;
    }
    return posed;
}
template <size_t N> size_t PosedAtFrameEnd(const std::array<uint32_t, N>& acCopies, const uint64_t aWord, const uint32_t aFrame)
{
    return PosedAtFrameEnd(acCopies, std::array<uint64_t, 1>{aWord}, aFrame);
}
} // namespace

TEST_CASE("Every remote VR player is posed on a flat screen, each copy by one poser", "[remote_body_poser]")
{
    const std::array<uint32_t, 3> cCopies{0xFF000801u, 0xFF000802u, 0xFF000803u};

    // No commit yet, or the commit is unavailable on this game: frame end poses all of them.
    CHECK(PosedAtFrameEnd(cCopies, 0, 100) == 3);

    // The commit posed the second copy this frame or the one before: frame end poses the other two.
    const uint64_t cWord = RemoteBodyPoser::CommitWord(cCopies[1], 100);
    for (const uint32_t cFrame : {100u, 101u})
    {
        CHECK(PosedAtFrameEnd(cCopies, cWord, cFrame) == 2);
        CHECK(RemoteBodyPoser::NativeCommitPoses(cWord, cCopies[1], cFrame));
        CHECK_FALSE(RemoteBodyPoser::NativeCommitPoses(cWord, cCopies[0], cFrame));
        CHECK_FALSE(RemoteBodyPoser::NativeCommitPoses(cWord, cCopies[2], cFrame));
    }

    // The commit stopped without a word of it: frame end takes its copy back on the next frame.
    CHECK(PosedAtFrameEnd(cCopies, cWord, 102) == 3);
}

TEST_CASE("Every copy that holds a slot is posed by the native commit, the rest at frame end", "[remote_body_poser]")
{
    const std::array<uint32_t, 4> cCopies{0xFF000801u, 0xFF000802u, 0xFF000803u, 0xFF000804u};
    // Three slots committed this frame or the one before; the fourth copy holds none.
    const std::array<uint64_t, 3> cWords{RemoteBodyPoser::CommitWord(cCopies[0], 100), RemoteBodyPoser::CommitWord(cCopies[1], 101), RemoteBodyPoser::CommitWord(cCopies[2], 101)};
    CHECK(PosedAtFrameEnd(cCopies, cWords, 101) == 1);
    // A slot whose commit is two frames old hands its copy back.
    CHECK(PosedAtFrameEnd(cCopies, cWords, 102) == 2);
}

TEST_CASE("A copy the native commit stops posing is posed at frame end in that same frame", "[remote_body_poser]")
{
    const std::array<uint32_t, 3> cCopies{0xFF000801u, 0xFF000802u, 0xFF000803u};
    std::array<uint64_t, 2> words{RemoteBodyPoser::CommitWord(cCopies[0], 100), RemoteBodyPoser::CommitWord(cCopies[1], 100)};
    CHECK(PosedAtFrameEnd(cCopies, words, 100) == 1);

    // Frame 101: the second copy's native update runs and the commit does not follow it. Without the withdrawal the word
    // still answers for frame 101 and nobody would pose that copy.
    CHECK(RemoteBodyPoser::NativeCommitPoses(words[1], cCopies[1], 101));
    for (auto& word : words)
        word = RemoteBodyPoser::Withdrawn(word, cCopies[1]);
    CHECK(words[1] == 0);
    CHECK_FALSE(RemoteBodyPoser::NativeCommitPoses(words[1], cCopies[1], 101));
    // The first copy's word is untouched; frame end poses the withdrawn copy and the slotless one.
    CHECK(RemoteBodyPoser::NativeCommitPoses(words[0], cCopies[0], 101));
    CHECK(PosedAtFrameEnd(cCopies, words, 101) == 2);

    // Withdrawing every copy (a session reset) hands all of them to frame end.
    for (auto& word : words)
        word = RemoteBodyPoser::Withdrawn(word, 0);
    CHECK(PosedAtFrameEnd(cCopies, words, 101) == 3);
}

TEST_CASE("The native commit word survives the engine frame counter wrapping", "[remote_body_poser]")
{
    const uint64_t cWord = RemoteBodyPoser::CommitWord(0x14, 0xFFFFFFFFu);
    CHECK(RemoteBodyPoser::NativeCommitPoses(cWord, 0x14, 0xFFFFFFFFu));
    CHECK(RemoteBodyPoser::NativeCommitPoses(cWord, 0x14, 0));
    CHECK_FALSE(RemoteBodyPoser::NativeCommitPoses(cWord, 0x14, 1));
    CHECK_FALSE(RemoteBodyPoser::NativeCommitPoses(cWord, 0, 0xFFFFFFFFu));
}
