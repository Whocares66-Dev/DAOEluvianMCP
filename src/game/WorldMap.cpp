// `worldmap`: the world map's pins, where the party is, and
// whether the map is up, read as the map screen reads them; travel's `to`
// (core/Travel.h) goes to a pin by them. A playthrough driven by MCP alone
// met the map at the Korcari Wilds' edge and had to read its ActionScript
// to cross it (turn-based-agent, 2026-10-03, #24).
//
// From worldmap.gfx (SharedLibrary/MapPinIcon.as PinClicked, WorldMap.as),
// read and tried in play by turn-based-agent that day: the pins are the
// data tree's WorldMap.PrimaryMap.Locations.<n>, each with State, Tag,
// Name, AreaTag, Type, X and Y; State 1 is not active, 2 active, 3 greyed,
// 4 unlockable, 5 completed, 6 destroyed. WorldMap.PrimaryMap.
// PlayerLocation.AreaTag is where the party is. A click on an active pin
// sends "GameModeController.AreaTransition <PlayerLocation.AreaTag>
// <index>", which took the party to Lothering through a random encounter
// on the way, as a click does. A list's member is found by its index as a
// path component (DAOLib 5e10b31's notes), so the pins are read until one
// is not there.
//
// A pin of the world's map is travelled to by DAOLib's dao::worldmap::Travel
// since #184 (DAOLib #259, f9754a7, lib-agent-3): the click's own steps, its
// closed-by-travel mark, the index and the transition, refused as
// PinClicked refuses, through the unlisted worldmap.travel.
#include "game/Shared.h"

#include "dao/Data.h"
#include "dao/Gui.h"
#include "dao/Resources.h"
#include "dao/WorldMap.h"
#include "dao/core/Gff4.h"

#include <algorithm>
#include <cwctype>
#include <format>

namespace eluvian::game
{

namespace
{

constexpr int kMostPins = 256;
// The world maps table (worldmaps.gda): a map's Type is its row, its Label
// the map's name (worldmap.gfx's _2DA.TABLE_WORLDMAPS).
constexpr std::int32_t kTableWorldMaps = 171;

std::string PinState(int state)
{
    switch (state)
    {
    case 1:
        return "not active";
    case 2:
        return "active";
    case 3:
        return "greyed";
    case 4:
        return "unlockable";
    case 5:
        return "completed";
    case 6:
        return "destroyed";
    default:
        return std::to_string(state);
    }
}

// The screen the map is on, if it is shown: the one whose movie is
// worldmap.gfx. Whether the game counts it a full-screen GUI was not yet
// seen (DAOLib's screens notes, 2026-10-02), so it is looked for among all
// the screens shown.
std::optional<dao::gui::Screen> MapScreen()
{
    for (const dao::gui::Screen &screen : dao::gui::ShownScreens())
    {
        std::wstring movie = screen.movie;
        std::ranges::transform(movie, movie.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        // The Fade's own map is one too: fademap.gfx, a Fade Pedestal's use
        // opens it (turn-based-agent, 2026-10-03, #34).
        if (movie == L"worldmap" || movie == L"fademap")
        {
            return screen;
        }
    }
    return std::nullopt;
}

std::string TreeText(std::wstring_view path)
{
    return Narrow(dao::data::Text(path).value_or(L""));
}

} // namespace

namespace
{

// The worldmap kind's handler, registered by AddWorldMap.
ToolResult KindWorldmap(const Watch &watch, const Json & /*args*/)
{
    RequireWorld(watch, "worldmap");
    const auto screen = MapScreen();
    std::wstring movie = screen ? screen->movie : L"";
    std::ranges::transform(movie, movie.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const bool fade = movie == L"fademap";
    // The Fade's map keeps its place under WorldMap.FadeMap; its pins
    // there too if the tree has them, else the primary map's, which
    // listed the Fade's ten pins in play.
    const std::wstring branch =
        fade && dao::data::Find(L"WorldMap.FadeMap.Locations.0.Tag") != nullptr ? L"FadeMap" : L"PrimaryMap";
    Json pins = Json::array();
    for (int index = 0; index < kMostPins; ++index)
    {
        const std::wstring base = std::format(L"WorldMap.{}.Locations.{}", branch, index);
        if (dao::data::Find(base + L".Tag") == nullptr)
        {
            break;
        }
        pins.push_back(Json{{"index", index},
                            {"name", TreeText(base + L".Name")},
                            {"tag", TreeText(base + L".Tag")},
                            {"area", TreeText(base + L".AreaTag")},
                            {"state", PinState(static_cast<int>(dao::data::Number(base + L".State").value_or(0)))},
                            {"type", dao::data::Number(base + L".Type").value_or(0)}});
    }
    // Two maps at once in a city: the map shown is the primary,
    // the other the secondary, and the map's toggle button sends
    // WorldMap.WorldMapSwapMaps, enabled while the secondary is not
    // 0. Each map's Type is a row of the world maps table (171),
    // whose Label names it (worldmap.gfx's GetCurrentMap,
    // GetOtherMap and UpdateWorldMap, read 2026-10-04). Denerim's
    // market opened the city's map, and a pin of the world's was
    // not on it (turn-based-agent, 2026-10-04, #42).
    const auto mapName = [](std::wstring_view map) -> Json {
        const auto type = dao::data::Number(std::wstring(map) + L".Type");
        if (!type)
        {
            return nullptr;
        }
        const std::string label =
            Narrow(api::GetM2DAString(kTableWorldMaps, L"Label", static_cast<std::int32_t>(*type)));
        return label.empty() ? Json(std::format("map {}", static_cast<int>(*type))) : Json(label);
    };
    // The other map is there when its Type is not 0, as the movie's
    // UpdateWorldMap reads GetOtherMap() + ".Type": WorldMap.
    // SecondaryMap itself is a node with no number, and read so no
    // second map was ever said, Denerim's swap never made
    // (turn-based-agent, 2026-10-05, #42).
    const bool other = !fade && dao::data::Number(L"WorldMap.SecondaryMap.Type").value_or(0) != 0;
    return Json{{"up", screen.has_value()},
                {"screen", screen ? Json(Narrow(screen->name)) : Json(nullptr)},
                {"map", screen ? Json(fade ? "fade" : "world") : Json(nullptr)},
                {"shown", fade ? Json(nullptr) : mapName(L"WorldMap.PrimaryMap")},
                {"other", other ? mapName(L"WorldMap.SecondaryMap") : Json(nullptr)},
                {"at", TreeText(fade ? L"WorldMap.FadeMap.PlayerLocation.AreaTag"
                                     : L"WorldMap.PrimaryMap.PlayerLocation.AreaTag")},
                {"count", pins.size()},
                {"pins", std::move(pins)}};
}

} // namespace

namespace
{

// Every map's pins, outside a city too, as the game's designer maps hold
// them (modules\single player\data\designermaps.erf): GFF4 of type MAPO,
// MapTag and MapPinList, each pin a MAPP with its name (a talk-table
// string), tag and the area it leads to (lib-agent, read 2026-10-09, #91).
// The labels are xoreos's (DAOLib's dev/gff4_fields.csv). The files hold a
// pin's first state; the story changes it, and worldmap's pins are the
// live ones.
constexpr std::array<const wchar_t *, 5> kMapFiles = {L"wide_open_world.map", L"denerim.map", L"denerim_climax.map",
                                                      L"underground_map.map", L"fade_map.map"};
constexpr std::uint32_t kMapTag = 24000;
constexpr std::uint32_t kMapPinList = 24002;
constexpr std::uint32_t kMapPinName = 24006;
constexpr std::uint32_t kMapPinTag = 24007;
constexpr std::uint32_t kMapPinAreatag = 24008;
constexpr std::uint32_t kNoString = 0xFFFFFFFF;

Json MapPins(const dao::core::gff4::Struct &root)
{
    Json pins = Json::array();
    for (const auto &pin : root.Structs(kMapPinList))
    {
        Json entry = {{"tag", Narrow(pin.String(kMapPinTag).value_or(L""))},
                      {"area", Narrow(pin.String(kMapPinAreatag).value_or(L""))}};
        if (const auto name = pin.Tlk(kMapPinName); name && name->strref != kNoString)
        {
            entry["name"] = Narrow(dao::tlk::String(name->strref));
        }
        pins.push_back(std::move(entry));
    }
    return pins;
}

ToolResult KindMaps(const Watch &watch)
{
    RequireWorld(watch, "worldmap");
    Json maps = Json::array();
    for (const wchar_t *file : kMapFiles)
    {
        const auto bytes = dao::resources::Read(file);
        if (!bytes)
        {
            continue;
        }
        const auto parsed = dao::core::gff4::File::Parse(*bytes);
        const auto root = parsed ? parsed->Root() : std::nullopt;
        if (!root)
        {
            continue;
        }
        Json pins = MapPins(*root);
        maps.push_back(Json{{"file", Narrow(file)},
                            {"tag", Narrow(root->String(kMapTag).value_or(L""))},
                            {"count", pins.size()},
                            {"pins", std::move(pins)}});
    }
    return Json{{"count", maps.size()}, {"maps", std::move(maps)}};
}

} // namespace

void AddWorldMap(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("worldmap",
             "Returns the world map: whether it is open (`up`), where the party is (`at`, an area tag), and "
             "its `pins`. Each pin has an `index`, `name`, `tag`, the `area` it leads to, and a `state`: "
             "`active`, `not active`, `greyed`, `unlockable`, `completed` or `destroyed`. Only active pins can be "
             "traveled to.\n\n"
             "To go to a pin, call `travel` with `to` (the pin's name, tag or index), either with the map "
             "open or together with the area's world map exit, which opens it. If the map has never been "
             "opened this game, there may be no pins yet.\n\n"
             "In a city, the open map may be the city's own: `shown` names the map on screen and `other` "
             "the one its toggle button switches to. To switch, call `input` with "
             "{\"message\": \"WorldMap.WorldMapSwapMaps\"}; `travel` with `to` switches by itself when the "
             "pin is on the other map. To close the map, call `input` with "
             "{\"message\": \"GUI.HideFullScreenGUI\"}, as its Close button does. (WorldMap.WorldMapClosed "
             "only reports that the map closed; it does not close it.)\n\n"
             "With `all: true`, it returns every map's pins instead, cities included, from the game's map "
             "files (`maps`: each map's `tag`, `file` and `pins` with `name`, `tag` and `area`). This works "
             "from anywhere and tells you which map a pin is on, but shows each pin's starting state, not "
             "its current one.",
             Json{{"all",
                   {{"type", "boolean"},
                    {"description", "Return every map's pins, cities included, from the game's files (starting "
                                    "states) instead of the open map. Default false."}}}},
             {}),
        [&watch](const Json &args) -> ToolResult {
            return Args(args, "worldmap").Bool("all", false) ? KindMaps(watch) : KindWorldmap(watch, args);
        });

    ToolSpec travel =
        Spec("worldmap.travel",
             "Travels to a pin of the world's map, by its `index`, as a click on it does: `taken`, or the "
             "`refusal` (not shown, read only, no pin, not active).",
             Json{{"index", {{"type", "integer"}, {"minimum", 0}}}}, {"index"});
    travel.listed = false;
    bench.Tools().Add(std::move(travel), [&watch](const Json &raw) -> ToolResult {
        RequireWorld(watch, "worldmap.travel");
        const Args args(raw, "worldmap.travel");
        const auto refusal = dao::worldmap::Travel(static_cast<std::int32_t>(args.Integer("index", 0, 0, 9999)));
        if (refusal == dao::worldmap::Refusal::None)
        {
            return Json{{"taken", true}};
        }
        return Json{{"taken", false},
                    {"refusal", Label(refusal, {{dao::worldmap::Refusal::NotShown, "not shown"},
                                                {dao::worldmap::Refusal::ReadOnly, "read only"},
                                                {dao::worldmap::Refusal::NoPin, "no pin"},
                                                {dao::worldmap::Refusal::NotActive, "not active"}})}};
    });
}

} // namespace eluvian::game
