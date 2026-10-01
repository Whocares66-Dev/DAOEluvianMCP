// The tools that read and drive the game, through DAOLib, on the frame
// thread: `inspect` (the game, the creatures, one creature in full, the
// geometry between two: distance, line of sight, the path being walked),
// `script` (any script command by name), `order` (the player's orders, as
// a click gives them), `stage` (a test's set-up: conditions, health, a
// death, a spawn) and `game` (pause). And the engine's events
// on the bus: loads, saves, and the script events a test waits on.
//
// Verified by playing, not by tests: what the engine answers is the point.
#pragma once

#include "core/Bench.h"
#include "game/Health.h"

namespace eluvian::game
{

void AddGameTools(Bench &bench, const Watch &watch);

// Subscribes to DAOLib's engine events and emits them as eluvian.*. At
// plugin load.
void AddEngineEvents(Bench &bench);

} // namespace eluvian::game
