// A wait until the game takes input again: after an exit, a fight's end, a
// conversation's end or a load, a driver polled `health` in a shell loop
// in a dozen places (turn-based-agent's survey of its driver, 2026-10-06,
// #84). `wait_ready` answers when the game is in a mode a player acts in:
// explore, combat, dead, the main menu, a conversation at a choice, a
// message box, a full-screen interface; and what passed on the way.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

// Over the tools `health` (the mode), `dialogue action=look` (a choice's
// replies) and `gui` (a message box, a full screen).
void AddWaitReadyTool(Bench &bench);

} // namespace eluvian
