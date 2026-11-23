#pragma once

#include <filesystem>
#include <string>

namespace oobe
{
// The folder the game's executable is in. For a game at the root of a drive it is "A:\", not "A:": that names the
// current folder on that drive, so the game's DLLs would not be found there.
inline std::wstring TitlePathOf(const std::wstring& acExePath)
{
    return std::filesystem::path(acExePath).parent_path().wstring();
}
} // namespace oobe
