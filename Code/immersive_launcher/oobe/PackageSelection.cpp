#include "PackageSelection.h"
#include "utils/ComUtils.h"
#include "BuildInfo.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <cryptopp/sha.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace fs = std::filesystem;
constexpr DWORD kMaxOptions = 8;

int ShowError(const wchar_t* apText)
{
    MessageBoxW(nullptr, apText, L"urSovngarde packages", MB_OK | MB_ICONERROR);
    return 1;
}

std::wstring ReadValue(const fs::path& acCatalog, const wchar_t* apSection, const std::wstring& acKey)
{
    wchar_t value[256]{};
    const auto length = GetPrivateProfileStringW(apSection, acKey.c_str(), L"", value, ARRAYSIZE(value), acCatalog.c_str());
    return length < ARRAYSIZE(value) - 1 ? std::wstring(value, length) : std::wstring{};
}

bool MatchesArchive(const fs::path& acFile, const std::wstring& acExpected)
{
    if (acExpected.size() != CryptoPP::SHA256::DIGESTSIZE * 2)
        return false;
    std::ifstream input(acFile, std::ios::binary);
    if (!input)
        return false;
    CryptoPP::SHA256 hash;
    std::array<CryptoPP::byte, 65536> buffer;
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        hash.Update(buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if (!input.eof() || input.bad())
        return false;
    std::array<CryptoPP::byte, CryptoPP::SHA256::DIGESTSIZE> digest;
    hash.Final(digest.data());
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring text;
    for (auto byte : digest)
    {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 15]);
    }
    return text == acExpected;
}
} // namespace

int oobe::SelectPackage()
{
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable))
        return ShowError(L"Could not find the launcher folder.");
    const fs::path folder = fs::path(executable).parent_path() / L"Packages";
    const auto catalog = folder / L"catalog.ini";
    const auto protocol = ReadValue(catalog, L"Package", L"Protocol");
#ifdef SKYRIMVR
    constexpr auto kEdition = L"VR";
#else
    constexpr auto kEdition = L"SE";
#endif
    const auto count = GetPrivateProfileIntW(L"Package", L"Options", kMaxOptions + 1, catalog.c_str());
    if (ReadValue(catalog, L"Package", L"Edition") != kEdition || std::string(protocol.begin(), protocol.end()) != BUILD_PROTOCOL || count > kMaxOptions)
        return ShowError(L"Place the Packages folder from this launcher's release beside the executable.");

    ComScope com;
    ComPtr<IFileSaveDialog> dialog;
    ComPtr<IFileDialogCustomize> options;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()))) || FAILED(dialog.As(&options)))
        return ShowError(L"The Windows package selection dialog could not be opened. The release also includes a FOMOD installer.");
    const COMDLG_FILTERSPEC filter{L"Mod manager archive", L"*.zip"};
    if (FAILED(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR)) || FAILED(dialog->SetFileTypes(1, &filter)) ||
        FAILED(dialog->SetDefaultExtension(L"zip")) || FAILED(dialog->SetFileName((std::wstring(L"urSovngarde-") + kEdition + L".zip").c_str())) ||
        FAILED(dialog->SetTitle(L"Choose packages for Mod Organizer 2 or Vortex")) || FAILED(dialog->SetOkButtonLabel(L"Save mod archive")))
        return ShowError(L"Could not initialize the package selection dialog.");
    const auto baseName = ReadValue(catalog, L"Package", L"Base");
    if (baseName.empty() || FAILED(options->AddCheckButton(1, baseName.c_str(), TRUE)) || FAILED(options->SetControlState(1, CDCS_VISIBLE)))
        return ShowError(L"The required base package could not be displayed.");
    for (UINT i = 0; i < count; ++i)
    {
        const auto name = ReadValue(catalog, L"Options", std::to_wstring(i));
        if (name.empty() || FAILED(options->AddCheckButton(i + 2, name.c_str(), FALSE)))
            return ShowError(L"Could not read the optional package list.");
    }
    const auto result = dialog->Show(nullptr);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return 0;
    ComPtr<IShellItem> item;
    PWSTR pPath{};
    if (FAILED(result) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
        return ShowError(L"Choose a local destination for the archive.");
    const fs::path destination(pPath);
    CoTaskMemFree(pPath);
    UINT mask{};
    for (UINT i = 0; i < count; ++i)
    {
        BOOL selected{};
        if (FAILED(options->GetCheckButtonState(i + 2, &selected)))
            return ShowError(L"Could not read the package selection.");
        if (selected)
            mask |= 1u << i;
    }
    const auto slot = GetPrivateProfileIntW(L"Selection", std::to_wstring(mask).c_str(), -1, catalog.c_str());
    if (slot >= (1u << count))
        return ShowError(L"This release does not contain the selected combination.");
    const auto source = folder / (std::to_wstring(slot) + L".zip");
    const auto hash = ReadValue(catalog, L"SHA256", std::to_wstring(slot));
    if (_wcsicmp(destination.extension().c_str(), L".zip") != 0)
        return ShowError(L"Save the mod archive with a .zip extension.");
    std::error_code ec;
    if (fs::equivalent(destination.parent_path(), folder, ec) || ec)
        return ShowError(L"Choose a destination outside the launcher's Packages folder.");
    wchar_t temp[MAX_PATH];
    if (!GetTempFileNameW(destination.parent_path().c_str(), L"urs", 0, temp))
        return ShowError(L"Could not create a temporary archive in the destination folder.");
    if (!CopyFileW(source.c_str(), temp, FALSE) || !MatchesArchive(temp, hash) || !MoveFileExW(temp, destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp);
        return ShowError(L"The package could not be saved. Check the release files and destination folder.");
    }
    MessageBoxW(
        nullptr, L"Install the saved archive as a mod in Mod Organizer 2 or Vortex. Replace the previous urSovngarde mod when changing packages.", L"Package ready",
        MB_OK | MB_ICONINFORMATION);
    return 0;
}
