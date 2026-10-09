// `inspect kind=worldmap`: the world map's pins, where the party is, and
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
#include "game/Shared.h"

#include "dao/Data.h"
#include "dao/Gui.h"
#include "dao/Resources.h"
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
    RequireWorld(watch, "inspect");
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
// pin's first state; the story changes it, and kind=worldmap's pins are the
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
    RequireWorld(watch, "inspect");
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
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"maps",
                 "Every map's pins, the cities' too, as the game's designer maps hold them (`maps`: each map's "
                 "`tag`, its file and its `pins`, each with `name`, `tag` and the `area` it leads to): which map "
                 "a pin is on, from anywhere. These are the pins' first states; kind=worldmap gives the live "
                 "ones of the map up. travel `to` a pin on a city's map reads it to go there by the city."},
        [&watch](const Json &) -> ToolResult { return KindMaps(watch); });
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"worldmap",
                 "The world map: whether it is `up`, where the party is (`at`, an area's tag), and its `pins`, "
                 "each with its `index`, `name`, `tag`, the `area` it leads to and its `state` (active pins can "
                 "be travelled to; not active, greyed, unlockable, completed, destroyed). travel with `to` (a "
                 "pin's name, tag or index) crosses to one, with the map up (a travel to the area's world-map "
                 "exit opens it). The pins are read as the map screen reads them; with the map never opened "
                 "this game, there may be none. In a city the map up may be the city's: `shown` names the map "
                 "shown and `other` the one its toggle swaps to (input {\"message\": "
                 "\"WorldMap.WorldMapSwapMaps\"}, as the button does; travel `to` swaps by itself for a pin on "
                 "the other). The map is closed as its Close button closes it: input "
                 "{\"message\": \"GUI.HideFullScreenGUI\"} (WorldMap.WorldMapClosed is the screen's report "
                 "that it closed, not the close)."},
        [&watch](const Json &args) -> ToolResult { return KindWorldmap(watch, args); });
}

} // namespace eluvian::game
