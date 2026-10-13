// Steam's libraryfolders.vdf read (Folders.h): the one part of finding the
// installs that is no question to Windows, so it is tested.
#include "ghilana/Folders.h"

namespace eluvian::ghilana
{

std::vector<std::string> SteamLibraries(std::string_view vdf)
{
    std::vector<std::string> paths;
    std::size_t at = 0;
    while ((at = vdf.find("\"path\"", at)) != std::string_view::npos)
    {
        at += 6;
        const auto open = vdf.find('"', at);
        if (open == std::string_view::npos)
        {
            break;
        }
        // Only blanks between the key and its value.
        if (vdf.substr(at, open - at).find_first_not_of(" \t") != std::string_view::npos)
        {
            continue;
        }
        std::string path;
        std::size_t p = open + 1;
        for (; p < vdf.size() && vdf[p] != '"'; ++p)
        {
            if (vdf[p] == '\\' && p + 1 < vdf.size())
            {
                ++p;
            }
            path += vdf[p];
        }
        if (!path.empty())
        {
            paths.push_back(std::move(path));
        }
        at = p + 1;
    }
    return paths;
}

} // namespace eluvian::ghilana
