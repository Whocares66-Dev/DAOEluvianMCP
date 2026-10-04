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

#include <algorithm>
#include <cwctype>

namespace eluvian::game
{

namespace
{

constexpr int kMostPins = 256;

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

void AddWorldMap(Bench &bench, const Watch &watch)
{
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"worldmap",
                 "The world map: whether it is `up`, where the party is (`at`, an area's tag), and its `pins`, "
                 "each with its `index`, `name`, `tag`, the `area` it leads to and its `state` (active pins can "
                 "be travelled to; not active, greyed, unlockable, completed, destroyed). travel with `to` (a "
                 "pin's name, tag or index) crosses to one, with the map up (a travel to the area's world-map "
                 "exit opens it). The pins are read as the map screen reads them; with the map never opened "
                 "this game, there may be none."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto screen = MapScreen();
            std::wstring movie = screen ? screen->movie : L"";
            std::ranges::transform(movie, movie.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
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
                pins.push_back(
                    Json{{"index", index},
                         {"name", TreeText(base + L".Name")},
                         {"tag", TreeText(base + L".Tag")},
                         {"area", TreeText(base + L".AreaTag")},
                         {"state", PinState(static_cast<int>(dao::data::Number(base + L".State").value_or(0)))},
                         {"type", dao::data::Number(base + L".Type").value_or(0)}});
            }
            return Json{{"up", screen.has_value()},
                        {"screen", screen ? Json(Narrow(screen->name)) : Json(nullptr)},
                        {"map", screen ? Json(fade ? "fade" : "world") : Json(nullptr)},
                        {"at", TreeText(fade ? L"WorldMap.FadeMap.PlayerLocation.AreaTag"
                                             : L"WorldMap.PrimaryMap.PlayerLocation.AreaTag")},
                        {"count", pins.size()},
                        {"pins", std::move(pins)}};
        });
}

} // namespace eluvian::game
