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

// What each of Eluvian's own topics means, into the catalogue a mod's go
// into too (`events describe=true`). AddEngineEvents calls it; so does
// eluvian_describe, for the docs, without the hooks (#109).
void DescribeTopics(EventCatalog &topics);

// Subscribes to DAOLib's engine events and emits them as eluvian.*. At
// plugin load.
void AddEngineEvents(Bench &bench);

// Places DAOLib's watch on the game's draws, for frame_capture (#160). At
// plugin load, before the game makes its device: one made before is not
// watched.
void WatchDraws();

} // namespace eluvian::game
