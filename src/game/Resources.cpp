// `resources`: the game's resources by name, read from the
// index of every archive in the game's folder (core/Erf.h): creature
// templates for `stage_spawn`, items for `stage_equip`, plots for
// `plot`, cutscenes for `stage_cutscene`. Files only, never game
// memory, so it answers on the listener's thread; the index is read once,
// on the first call (some ten thousand archives), and kept.
#include "game/Shared.h"

#include "core/Erf.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace eluvian::game
{

namespace
{

namespace fs = std::filesystem;

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The game's folder: the one above bin_ship, where DAOrigins.exe runs.
fs::path GameFolder()
{
    std::wstring path(MAX_PATH, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return fs::path(path).parent_path().parent_path();
}

std::vector<std::string> NamesIn(const fs::path &archive)
{
    std::ifstream in(archive, std::ios::binary);
    std::vector<std::byte> head(kErfHeadBytes);
    if (!in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size())))
    {
        return {};
    }
    const auto length = ErfIndexBytes(head);
    constexpr std::size_t kMostIndex = 64U << 20;
    if (!length || *length > kMostIndex)
    {
        return {};
    }
    std::vector<std::byte> index(*length);
    in.seekg(0);
    in.read(reinterpret_cast<char *>(index.data()), static_cast<std::streamsize>(index.size()));
    index.resize(static_cast<std::size_t>(in.gcount()));
    return ErfNames(index);
}

struct Catalog
{
    fs::path root;
    std::map<std::string, std::string> where; // a resource's name, lower case, to its first archive
    std::size_t archives = 0;
};

const Catalog &Resources()
{
    static Catalog catalog;
    static std::once_flag read;
    std::call_once(read, [] {
        catalog.root = GameFolder();
        std::error_code error;
        for (fs::recursive_directory_iterator it(catalog.root, fs::directory_options::skip_permission_denied, error),
             end;
             it != end; it.increment(error))
        {
            if (error || !it->is_regular_file(error))
            {
                continue;
            }
            const std::string extension = Lower(it->path().extension().string());
            if (extension != ".erf" && extension != ".rim")
            {
                continue;
            }
            ++catalog.archives;
            const std::string archive = fs::relative(it->path(), catalog.root, error).string();
            for (auto &name : NamesIn(it->path()))
            {
                catalog.where.emplace(Lower(std::move(name)), archive);
            }
        }
    });
    return catalog;
}

} // namespace

namespace
{

// The resources kind's handler, registered by AddResources.
ToolResult KindResources(const Json &raw)
{
    const Args args(raw, "resources");
    const std::string type = Lower(args.String("type", ""));
    std::vector<std::string> words;
    {
        const std::string match = Lower(args.String("match", ""));
        std::size_t at = 0;
        while (at < match.size())
        {
            const auto end = std::min(match.find(' ', at), match.size());
            if (end > at)
            {
                words.push_back(match.substr(at, end - at));
            }
            at = end + 1;
        }
    }
    const auto most = static_cast<std::size_t>(args.Integer("most", 100, 1, 2000));
    const Catalog &catalog = Resources();
    Json found = Json::array();
    std::size_t count = 0;
    for (const auto &[name, archive] : catalog.where)
    {
        const auto dot = name.rfind('.');
        if (!type.empty() && (dot == std::string::npos || name.substr(dot + 1) != type))
        {
            continue;
        }
        if (!std::ranges::all_of(words,
                                 [&name](const std::string &word) { return name.find(word) != std::string::npos; }))
        {
            continue;
        }
        if (++count <= most)
        {
            found.push_back(Json{{"name", dot == std::string::npos ? name : name.substr(0, dot)},
                                 {"type", dot == std::string::npos ? "" : name.substr(dot + 1)},
                                 {"in", archive}});
        }
    }
    return Json{{"count", count},
                {"resources", std::move(found)},
                {"archives", catalog.archives},
                {"folder", catalog.root.string()}};
}

} // namespace

void AddResources(Bench &bench)
{
    bench.Tools().Add(
        Read("resources",
             "Searches the game's resources by name, across every archive in the game's folder. Use it to find "
             "the name to pass to another tool:\n\n"
             "- `utc`: creature templates, for `stage_spawn`.\n"
             "- `uti`: item templates, for `stage_equip`.\n"
             "- `plo`: plots, for `plot`.\n"
             "- `cut`: cutscenes, for `stage_cutscene`.\n\n"
             "Returns the matching names, sorted, each without its extension and with the archive it is in, up "
             "to `most`, and the `count` of all matches. The core and campaign archives are in every game; an "
             "add-in's resources only load where that add-in runs. The first call in a game reads every "
             "archive's index from disk and can take a minute or more, so give it a long timeout; later calls "
             "are fast.",
             Json{{"type",
                   {{"type", "string"},
                    {"description", "The file extension to list: utc, uti, plo or cut. Default: all types."}}},
                  {"match",
                   {{"type", "string"},
                    {"description", "Words that must all appear in the name, in any case, e.g. \"hurlock archer\"."}}},
                  {"most",
                   {{"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 2000},
                    {"description", "The most names to return. Default 100."}}}},
             {}, true, Thread::Listener),
        [](const Json &raw) -> ToolResult { return KindResources(raw); });
}

} // namespace eluvian::game
