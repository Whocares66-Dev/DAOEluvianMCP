// A gift to a follower, answered once the game has taken it. The game's gift
// event, signalled from the frame thread, is handled a frame or more later:
// six gifts answered given: false with the approval unchanged, and the next
// call's approval before had the rise (turn-based-agent, 2026-10-09, #81).
// Processed at once, the event runs only the follower's script, which
// reckons the approval and signals the module; the module's handler moves
// the item and changes the approval later (lib-agent, 2026-10-09).
// So the tool gives (gift.give, on the frame thread), then reads the
// approval and the pack (gift.state) until the item has left it or the
// approval has moved, within a budget.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// Over the tools gift.give and gift.state (src/game, unlisted) and health
// (the mode: a special gift starts a conversation). `poll` is how often the
// state is read.
void AddGiftTool(Bench &bench, std::chrono::milliseconds poll = std::chrono::milliseconds(100));

} // namespace eluvian
