// Where the game is on this machine, and the user's folders: the one file
// of ghilana that asks Windows (its registry and known folders), so the
// rest reads paths it is given and is tested over folders a test makes.
//
// The installs, as found on this machine 2026-10-12 (dev/GHILANA.md, "The
// game on disk"): GOG's, whose registry key (HKLM\SOFTWARE\WOW6432Node\
// GOG.com\Games\1949616134, `path`) the GOG installer writes; Steam's,
// under a library its libraryfolders.vdf lists, as steamapps\common\Dragon
// Age Ultimate Edition (Steam's own folder from HKCU\Software\Valve\Steam,
// `SteamPath`); and EA's, at C:\Program Files\EA Games\Dragon Age. BioWare's
// key (HKLM\SOFTWARE\WOW6432Node\BioWare\Dragon Age, `Path`) names the last
// one installed, whichever store's: here Steam's.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eluvian::ghilana
{

struct Install
{
    std::string store; // "gog", "steam", "ea"
    std::filesystem::path folder;
};

// Every install found, GOG's first, then Steam's, then EA's; an install is
// a folder with packages\core\data\2da.erf in it.
std::vector<Install> FindInstalls();

// The user's Documents\BioWare\Dragon Age, from Windows' Documents folder
// (under OneDrive on this machine); nothing if Windows names none.
std::optional<std::filesystem::path> GameDocuments();

// The user's local application data (%LOCALAPPDATA%).
std::optional<std::filesystem::path> LocalAppData();

// Whether a folder holds the game's data, as FindInstalls asks.
bool IsInstall(const std::filesystem::path &folder);

// The library folders a libraryfolders.vdf lists, its "path" values, with
// their backslashes as the file escapes them read (Steam.cpp).
std::vector<std::string> SteamLibraries(std::string_view vdf);

} // namespace eluvian::ghilana
