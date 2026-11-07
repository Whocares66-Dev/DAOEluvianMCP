// `safe_location`: the scripts' GetSafeLocation (659) and IsLocationSafe
// (456) for a point, typed. Two readings of GetSafeLocation's handler
// (0x0065B0D0) disagree on what a script gets back, the location given on a
// find and the one found on a failure or the reverse (DAOLib's
// PATHFINDING.md, paragon-agent-2, 2026-11-01, #192); `script` cannot pass a
// location, those commands not being callable by name, so this asks them
// with one built by the scripts' own Location (454).
//
// Seen 2026-11-07 in the forest clearing (84 points): no unsafe point came
// back moved in x or y; each came back as itself (3 m from a safe one too),
// as zeros, or at the ground's height. So the handler copies the search's
// `out` only when it answers 0, as the code reads (the first reading;
// DAOParagonEngine #440).
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

Json LocationJson(const dao::CScriptLocation &location)
{
    return Json{{"area", Brief(api::GetAreaFromLocation(location))},
                {"point", VectorJson(api::GetPositionFromLocation(location))}};
}

} // namespace

void AddSafeLocation(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("safe_location",
             "Asks the game whether a point is a safe place to stand and where the nearest one is, as the scripts "
             "do: `safe` is IsLocationSafe's answer for the point, and `result` the location GetSafeLocation "
             "gives for it (its area and point), with `moved` its distance from the point. Pass `point`, {x, y, "
             "z} or [x, y, z], and optionally `area`, an area's id (default the hero's). The command gives back "
             "the point itself when its search found a safe spot (the spot found is not handed to the script), "
             "and zeros or the point at the ground's height when it found none, so `result` says whether a "
             "search found something, not where. IsLocationSafe counts creatures: a point a creature stands on "
             "is not safe. Off the walk grid it answers safe.",
             Json{{"point", {{"description", "The point: {x, y, z} or [x, y, z]."}}},
                  {"area", {{"type", "string"}, {"description", "The area, by id. Default: the hero's."}}}},
             {"point"}),
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "safe_location");
            const Args args(raw, "safe_location");
            const dao::Vector point = VectorOf(args.Raw().at("point"), "safe_location: `point`");
            const ObjectId area = args.OptionalId("area", "an area").value_or(api::GetArea(api::GetHero()));
            const dao::CScriptLocation asked = api::Location(area, point, 0.0F);
            const dao::CScriptLocation found = api::GetSafeLocation(asked);
            const dao::Vector at = api::GetPositionFromLocation(found);
            return Json{{"input", LocationJson(asked)},
                        {"result", LocationJson(found)},
                        {"moved", Distance(point, at)},
                        {"safe", api::IsLocationSafe(asked) != 0}};
        });
}

} // namespace eluvian::game
