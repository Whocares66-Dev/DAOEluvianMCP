// `travel`: walk a creature to a target or a point, opening what is shut on
// the way. One call for what a test otherwise writes by hand each time: ask
// for the walk, use the door it names, wait for the command's end, ask
// again, move. A night's tests each did this themselves, and one failed ten
// moves on a door it could not name (2026-10-01).
//
// It decides over other tools' answers alone, so it lives here and is
// tested with stand-ins: `geometry` with `walk` (found, or
// `blocker`, or `reached`), state (who is controlled), `order` (move,
// use), `health` (the mode, paused) and the bus's eluvian.command.ended.
// It stops and says why where the journey is no longer its to make: a
// fight began, a conversation or a cutscene took the screen, the game is
// paused, the time ran out.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddTravelTool(Bench &bench);

} // namespace eluvian
