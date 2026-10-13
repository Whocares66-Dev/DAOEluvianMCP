#include "ghilana/Folders.h"

#include "ghilana/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#include <fstream>
#include <sstream>
#include <system_error>

namespace eluvian::ghilana
{

namespace
{

namespace fs = std::filesystem;

std::optional<std::wstring> RegistryText(HKEY root, const wchar_t *key, const wchar_t *value)
{
    DWORD size = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
    {
        return std::nullopt;
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    text.resize(size / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0')
    {
        text.pop_back();
    }
    return text;
}

std::optional<fs::path> KnownFolder(const KNOWNFOLDERID &id)
{
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, 0, nullptr, &path);
    std::optional<fs::path> folder;
    if (SUCCEEDED(result) && path != nullptr)
    {
        folder = fs::path(path);
    }
    CoTaskMemFree(path);
    return folder;
}

void Add(std::vector<Install> &installs, std::string store, const fs::path &folder)
{
    if (!IsInstall(folder))
    {
        return;
    }
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(folder, error);
    for (const auto &install : installs)
    {
        std::error_code same;
        if (fs::equivalent(install.folder, canonical, same))
        {
            return;
        }
    }
    installs.push_back(Install{std::move(store), error ? folder : canonical});
}

} // namespace

bool IsInstall(const fs::path &folder)
{
    std::error_code error;
    return fs::is_regular_file(folder / L"packages" / L"core" / L"data" / L"2da.erf", error);
}

std::vector<Install> FindInstalls()
{
    std::vector<Install> installs;
    if (const auto gog =
            RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1949616134", L"path"))
    {
        Add(installs, "gog", *gog);
    }
    Add(installs, "gog", L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Dragon Age Origins");

    std::vector<fs::path> steam;
    if (const auto root = RegistryText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"))
    {
        steam.emplace_back(*root);
    }
    steam.emplace_back(L"C:\\Program Files (x86)\\Steam");
    for (const auto &root : steam)
    {
        std::ifstream vdf(root / L"steamapps" / L"libraryfolders.vdf", std::ios::binary);
        std::ostringstream text;
        text << vdf.rdbuf();
        std::vector<fs::path> libraries{root};
        for (const auto &library : SteamLibraries(text.str()))
        {
            libraries.emplace_back(Wide(library));
        }
        for (const auto &library : libraries)
        {
            Add(installs, "steam", library / L"steamapps" / L"common" / L"Dragon Age Ultimate Edition");
        }
    }

    Add(installs, "ea", L"C:\\Program Files\\EA Games\\Dragon Age");
    // BioWare's key names the last installed, whatever its store: a folder
    // under steamapps is Steam's, any other EA's.
    if (const auto bioware = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\BioWare\\Dragon Age", L"Path"))
    {
        const bool isSteam = Lower(PathText(*bioware)).find("/steamapps/") != std::string::npos;
        Add(installs, isSteam ? "steam" : "ea", *bioware);
    }
    return installs;
}

std::optional<fs::path> GameDocuments()
{
    const auto documents = KnownFolder(FOLDERID_Documents);
    if (!documents)
    {
        return std::nullopt;
    }
    return *documents / L"BioWare" / L"Dragon Age";
}

std::optional<fs::path> LocalAppData()
{
    return KnownFolder(FOLDERID_LocalAppData);
}

} // namespace eluvian::ghilana
