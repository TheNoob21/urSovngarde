#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Helpers/CrashDumpPrune.h>

namespace CrashDumps
{
// Only handles returned by CREATE_NEW are passed here. A second lookup by
// filename could delete another run's file if the virtual file map changed.
inline void Discard(HANDLE aFile) noexcept
{
    if (aFile != INVALID_HANDLE_VALUE)
    {
        FILE_DISPOSITION_INFO info{TRUE};
        SetFileInformationByHandle(aFile, FileDispositionInfo, &info, sizeof(info));
        CloseHandle(aFile);
    }
}

template <size_t Count> class History
{
public:
    History() { m_files.fill(INVALID_HANDLE_VALUE); }
    History(const History&) = delete;
    History& operator=(const History&) = delete;
    ~History()
    {
        for (const HANDLE cFile : m_files)
            if (cFile != INVALID_HANDLE_VALUE)
                CloseHandle(cFile);
    }
    void Keep(HANDLE aFile) noexcept
    {
        if (aFile == INVALID_HANDLE_VALUE)
            return;
        Discard(m_files[m_next]);
        m_files[m_next] = aFile;
        m_next = (m_next + 1) % Count;
    }

private:
    std::array<HANDLE, Count> m_files;
    size_t m_next = 0;
};

class Directory
{
public:
    Directory() = default;
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    ~Directory()
    {
        for (const HANDLE cPin : m_pins)
            CloseHandle(cPin);
    }

    bool InitializeForCurrentProcess()
    {
        // GetModuleFileName(NULL) reports the selected Skyrim executable in
        // the immersive launcher. The kernel process image is the launcher.
        std::wstring image(32768, L'\0');
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image.data(), &length))
            return false;
        image.resize(length);
        return Initialize(std::filesystem::path(image).parent_path());
    }

    bool Initialize(const std::filesystem::path& acLauncher)
    {
        if (!m_pins.empty() || !acLauncher.is_absolute() || acLauncher.root_name().native().size() != 2 || acLauncher != acLauncher.lexically_normal())
            return false;
        auto current = acLauncher.root_path();
        const auto cLogs = acLauncher / L"logs";
        Directory pending;
        if (!pending.Pin(current))
            return false;
        for (const auto& cComponent : acLauncher.relative_path())
        {
            current /= cComponent;
            if (!pending.Pin(current))
                return false;
        }
        // CrashHandler is a member constructed before the client log sink
        // creates logs on the first launch.
        if (!CreateDirectoryW(cLogs.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        if (!pending.Pin(cLogs))
            return false;
        // Before this launch's folder exists, so it is never among the runs pruned.
        PruneEarlierRuns(cLogs);

        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const auto cTicks = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        static std::atomic<unsigned> s_sequence{0};
        auto run = cLogs / (L"crash-run-" + std::to_wstring(cTicks) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(s_sequence++));
        if (!CreateDirectoryW(run.c_str(), nullptr) || !pending.Pin(run))
            return false;
        m_path = std::move(run);
        m_pins.swap(pending.m_pins);
        return true;
    }

    const std::filesystem::path& Path() const noexcept { return m_path; }

    HANDLE CreateFileForRun(const std::filesystem::path& acLeaf) const
    {
        const auto& cName = acLeaf.native();
        if (m_path.empty() || cName.empty() || acLeaf != acLeaf.filename() || cName.back() == L'.' || cName.back() == L' ' ||
            cName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        {
            SetLastError(ERROR_INVALID_NAME);
            return INVALID_HANDLE_VALUE;
        }
        const auto cPath = m_path / acLeaf;
        HANDLE file = CreateFileW(cPath.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE && !MatchesPhysicalPath(file, cPath))
        {
            CloseHandle(file);
            SetLastError(ERROR_BAD_PATHNAME);
            return INVALID_HANDLE_VALUE;
        }
        return file;
    }

private:
    /**
     * Removes the dumps of earlier launches that CrashDumpPrune does not keep, and the run folders left empty.
     * Only plain files directly inside `crash-run-<n>-<n>-<n>` folders are touched, never a link or a junction. A file
     * or folder another running game still holds (opened without delete sharing) cannot be removed and is left.
     * Best effort: a failure here never stops the crash output.
     */
    static void PruneEarlierRuns(const std::filesystem::path& acLogs) noexcept
    {
        try
        {
            struct Found
            {
                std::filesystem::path Folder;
                std::vector<std::pair<std::wstring, CrashDumpPrune::DumpKind>> Files;
            };
            std::vector<Found> found;
            std::vector<CrashDumpPrune::Run> runs;

            const auto forEach = [](const std::filesystem::path& acPattern, const auto& acVisit)
            {
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileExW(acPattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
                if (find == INVALID_HANDLE_VALUE)
                    return;
                do
                {
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        acVisit(data);
                } while (FindNextFileW(find, &data));
                FindClose(find);
            };

            forEach(
                acLogs / L"crash-run-*",
                [&](const WIN32_FIND_DATAW& acRun)
                {
                    const auto cKey = CrashDumpPrune::ParseRunName(acRun.cFileName);
                    if (!(acRun.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !cKey)
                        return;
                    Found entry{acLogs / acRun.cFileName, {}};
                    CrashDumpPrune::Run run{*cKey};
                    forEach(
                        entry.Folder / L"*",
                        [&](const WIN32_FIND_DATAW& acFile)
                        {
                            if (acFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                                return;
                            const auto cKind = CrashDumpPrune::KindOf(acFile.cFileName);
                            run.HasFull |= cKind == CrashDumpPrune::DumpKind::Full;
                            run.HasSmall |= cKind == CrashDumpPrune::DumpKind::Small;
                            entry.Files.emplace_back(acFile.cFileName, cKind);
                        });
                    found.push_back(std::move(entry));
                    runs.push_back(run);
                });

            // A run younger than this may be another game starting right now, its folder made but not yet pinned: left alone.
            constexpr unsigned long long cRecentTicks = 5ull * 60 * 10'000'000;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            const auto cNowTicks = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;

            const auto cVerdicts = CrashDumpPrune::Decide(runs);
            for (size_t i = 0; i < found.size(); ++i)
            {
                if (runs[i].Key.Ticks + cRecentTicks > cNowTicks)
                    continue;
                for (const auto& [cName, cKind] : found[i].Files)
                {
                    if ((cKind == CrashDumpPrune::DumpKind::Full && cVerdicts[i].RemoveFull) || (cKind == CrashDumpPrune::DumpKind::Small && cVerdicts[i].RemoveSmall))
                        DeleteFileW((found[i].Folder / cName).c_str());
                }
                // Fails unless the folder is empty now, and while another running game holds it.
                RemoveDirectoryW(found[i].Folder.c_str());
            }
        }
        catch (...)
        {
        }
    }

    static bool MatchesPhysicalPath(HANDLE aHandle, const std::filesystem::path& acPath)
    {
        // DOS paths returned by this API include "\\?\" (four characters).
        const size_t cCapacity = acPath.native().size() + 5;
        auto actual = std::unique_ptr<wchar_t[]>(new (std::nothrow) wchar_t[cCapacity]);
        if (!actual)
            return false;
        const DWORD cCount = GetFinalPathNameByHandleW(aHandle, actual.get(), static_cast<DWORD>(cCapacity), 0);
        return cCount && cCount < cCapacity && wcsncmp(actual.get(), L"\\\\?\\", 4) == 0 && CompareStringOrdinal(actual.get() + 4, -1, acPath.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool Pin(const std::filesystem::path& acPath)
    {
        HANDLE dir = CreateFileW(
            acPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (dir == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(dir, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !MatchesPhysicalPath(dir, acPath))
        {
            CloseHandle(dir);
            return false;
        }
        try
        {
            m_pins.push_back(dir);
        }
        catch (...)
        {
            CloseHandle(dir);
            throw;
        }
        return true;
    }
    std::vector<HANDLE> m_pins;
    std::filesystem::path m_path;
};
} // namespace CrashDumps
