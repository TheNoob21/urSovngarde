#pragma once

namespace Base
{
// true when the game runs under Wine or Proton (their ntdll exports wine_get_version)
bool IsRunningUnderWine() noexcept;
} // namespace Base
