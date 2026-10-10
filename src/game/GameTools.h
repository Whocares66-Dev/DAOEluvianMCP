// The tools that read and drive the game, through DAOLib, on the frame
// thread, each in its own file (game/Shared.h lists them): the reads,
// `script`, `order`, `control`, the stage tools, `screenshot`, `saves`, `pause`, `press`, `input`, `camera` and
// `dialogue`. This file registers them, and puts the engine's events on the
// bus: loads, saves, commands, conversations, and the script events a test
// waits on.
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
