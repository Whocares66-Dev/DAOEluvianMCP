// `warp.area`, unlisted: an area's file as core's `warp` needs it (#157).
// The engine's DoAreaTransition takes a waypoint the area lacks and puts the
// party at (0, 0, 0) (seen 2026-10-19, the Forest Clearing), so a waypoint
// is checked against the area's file first. An area is a GFF3 .are whose
// WaypointList has each waypoint's Tag and position. A module keeps every
// one of its areas in its designerareas.erf (modules\<module>\data, and an
// add-in's addins\<name>\module\data), besides each in its al_<area>.rim:
// ten archives here (2026-10-19), where `resources`' index of every archive
// in the game's folder took more than the bridge's 90 s on its first call.
// An add-in's archive is enciphered, and its areas' files are not read: the
// area is known by name, and its waypoints are not. Files only, never game
// memory: on the listener's thread.
#include "game/Shared.h"

#include <dao/core/Erf.h>
#include <dao/core/Gff3.h>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
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

struct Place
{
    fs::path archive;
    std::string module; // the module's folder: "single player", or an add-in's name
};

// Each module's designerareas.erf: the campaign's own before the add-ins'.
std::vector<Place> AreaArchives()
{
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    const fs::path game = fs::path(exe).parent_path().parent_path();
    std::vector<Place> archives;
    std::error_code error;
    for (const auto &[root, data] :
         {std::pair{game / "modules", fs::path("data")}, std::pair{game / "addins", fs::path("module") / "data"}})
    {
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        {
            const fs::path archive = it->path() / data / "designerareas.erf";
            if (fs::is_regular_file(archive, error))
            {
                archives.push_back(Place{archive, it->path().filename().string()});
            }
        }
    }
    std::ranges::stable_partition(archives, [](const Place &p) { return Lower(p.module) == "single player"; });
    return archives;
}

// The areas' names in an archive, lower case, without ".are".
std::vector<std::string> AreaNamesIn(const fs::path &archive)
{
    std::vector<std::string> names;
    const auto opened = dao::core::erf::Archive::Open(archive);
    if (!opened)
    {
        return names;
    }
    for (const auto &entry : opened->Entries())
    {
        std::string name;
        for (const wchar_t letter : entry.name)
        {
            name += letter < 0x80 ? static_cast<char>(letter) : '?';
        }
        name = Lower(name);
        if (name.size() > 4 && name.ends_with(".are"))
        {
            names.push_back(name.substr(0, name.size() - 4));
        }
    }
    return names;
}

// Every area's name, lower case, to the first designerareas.erf that has it.
const std::map<std::string, Place> &Areas()
{
    static std::map<std::string, Place> areas;
    static std::once_flag read;
    std::call_once(read, [] {
        for (const auto &place : AreaArchives())
        {
            for (auto &name : AreaNamesIn(place.archive))
            {
                areas.emplace(std::move(name), place);
            }
        }
    });
    return areas;
}

// The area's waypoints, those with a tag: the engine finds one by its tag.
Json Waypoints(const dao::core::gff3::Struct &root)
{
    Json waypoints = Json::array();
    for (const auto &waypoint : root.List("WaypointList"))
    {
        const std::string tag = waypoint.String("Tag").value_or("");
        if (tag.empty())
        {
            continue;
        }
        waypoints.push_back(Json{{"tag", tag},
                                 {"position",
                                  {{"x", waypoint.Get<float>("XPosition").value_or(0.0F)},
                                   {"y", waypoint.Get<float>("YPosition").value_or(0.0F)},
                                   {"z", waypoint.Get<float>("ZPosition").value_or(0.0F)}}}});
    }
    return waypoints;
}

ToolResult AreaFile(const Json &raw)
{
    const Args args(raw, "warp");
    const std::string area = Lower(args.String("area"));
    const auto &areas = Areas();
    const auto found = areas.find(area);
    if (found == areas.end())
    {
        Json names = Json::array();
        for (const auto &[name, place] : areas)
        {
            names.push_back(name);
        }
        return Json{{"found", false}, {"areas", std::move(names)}};
    }
    const auto archive = dao::core::erf::Archive::Open(found->second.archive);
    const auto bytes = archive ? archive->Read(std::wstring(area.begin(), area.end()) + L".are") : std::nullopt;
    const auto file = bytes ? dao::core::gff3::File::Parse(*bytes) : std::nullopt;
    const auto root = file ? file->Root() : std::nullopt;
    if (!root)
    {
        // An add-in's archives are enciphered (DAOLib does not read them yet):
        // the area is there, its waypoints are not known.
        return Json{{"found", true}, {"tag", area}, {"module", found->second.module}, {"readable", false}};
    }
    return Json{{"found", true},
                {"tag", root->String("Tag").value_or(area)},
                {"module", found->second.module},
                {"waypoints", Waypoints(*root)}};
}

} // namespace

void AddWarpArea(Bench &bench)
{
    ToolSpec spec{"warp.area", "", {}, Thread::Listener};
    spec.listed = false;
    bench.Tools().Add(spec, &AreaFile);
}

} // namespace eluvian::game
