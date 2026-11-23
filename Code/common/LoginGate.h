#pragma once

#include <Opcodes.h>

#include <cstdint>

//! What the server reads from a connection that has not logged in yet.
namespace LoginGate
{
//! True when a message with this opcode is read from a connection that has no player: only the login.
//! Any other message is dropped unread and the connection is closed, so its decoder never runs for a stranger.
constexpr bool IsReadBeforeLogin(const uint64_t aOpcode) noexcept
{
    return aOpcode == kAuthenticationRequest;
}
} // namespace LoginGate
