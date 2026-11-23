#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

//! What the server relays of a player's voice: pieces of a size Steam's voice makes, and as many a second as it makes.
//! Every piece goes to every listener, party members at any distance, so the limits are on the bytes as well as the count.
//! Pure rules on plain values, so they can be tested (tests/voice_relay_rules.cpp).
namespace VoiceRelayRules
{
//! The most one piece may hold. Steam's compressed pieces are a few hundred bytes.
constexpr size_t kMaxPieceBytes = 1200;
//! Steam sends about 17 pieces a second while talking.
constexpr uint32_t kMaxPiecesPerSecond = 60;
//! Bytes a second from one speaker, about three times what talking needs.
constexpr uint32_t kMaxBytesPerSecond = 24576;

//! What one speaker has sent this second.
struct Budget
{
    std::chrono::steady_clock::time_point Since{};
    uint32_t Pieces{};
    uint32_t Bytes{};

    //! Whether a piece of aBytes may go on now; a piece refused does not count against the rest of the second.
    bool Allow(const std::chrono::steady_clock::time_point aNow, const size_t aBytes) noexcept
    {
        if (aBytes == 0 || aBytes > kMaxPieceBytes)
            return false;
        if (aNow - Since >= std::chrono::seconds(1))
        {
            Since = aNow;
            Pieces = 0;
            Bytes = 0;
        }
        if (Pieces >= kMaxPiecesPerSecond || Bytes + aBytes > kMaxBytesPerSecond)
            return false;
        ++Pieces;
        Bytes += static_cast<uint32_t>(aBytes);
        return true;
    }
};
} // namespace VoiceRelayRules
