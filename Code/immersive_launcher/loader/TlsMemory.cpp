
/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include <cstdint>

// Changes:
// - 2021/2/23: use TLS touched class to automatically initialize TLS.
// - 2021/2/24: Lowered necessary TLS pool size for Fallout4 and SkyrimSE.

#define DECLARE_TLS_VARS(i) __declspec(thread) uint8_t tls[sizeof(int) * i]

DECLARE_TLS_VARS(7096);

#include <Windows.h>

struct TlsToucher
{
    TlsToucher() { *reinterpret_cast<int*>(&tls[4]) = static_cast<int>(GetTickCount64()); }
};

TlsToucher toucher;

// The game's TLS is copied to the start of this thread's block, over the placeholder (ExeLoader::LoadTLS).
// So the placeholder has to come first: a thread local of ours ahead of it shares memory with the game's.
size_t TlsPlaceholderOffset(const void* apTlsBase) noexcept
{
    return static_cast<size_t>(reinterpret_cast<const uint8_t*>(&tls[0]) - static_cast<const uint8_t*>(apTlsBase));
}

size_t TlsPlaceholderSize() noexcept
{
    return sizeof(tls);
}
