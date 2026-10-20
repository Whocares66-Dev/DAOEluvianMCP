// `warp`: the game put in an area at a waypoint without a save there (#157).
// DAOParagonEngine's comparisons could look at a place only where a save
// was near it: the Tower of Ishal's top floor could not be compared on
// 2026-10-19 for want of one. The engine's own jump is the scripts'
// DoAreaTransition(area, waypoint): from Lyna's save in the Dalish origin it
// loaded the tower's top floor in a second, and the area's scripts ran its
// cutscene and its fight as on arriving by the stairs. A waypoint the area
// lacks is not refused by the engine: the party stood at (0, 0, 0). So the
// area and the waypoint are checked against the area's file first, and an
// agent is told the names that are there.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// Over warp.area (src/game, unlisted: the area's file, or every area's
// name), script (DoAreaTransition), health (the mode, the cursor), state
// (the area the party is in) and creature (where the hero stands). `poll` is
// how often the load is looked at, `settle` how long after arriving the mode
// is read again: an area's scripts start a cutscene a moment after.
void AddWarpTool(Bench &bench, std::chrono::milliseconds poll = std::chrono::milliseconds(250),
                 std::chrono::milliseconds settle = std::chrono::milliseconds(1500));

} // namespace eluvian
