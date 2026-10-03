// `dialogue`: answer the conversation on the screen, once, or on through its
// lines to the next real choice. From one choice to the next took an agent
// a read, a continue, a wait and the read again, several times, with a retry
// for an answer the game ignored in the moment after the replies came: the
// playthrough driven by MCP alone kept a script verb for nothing else
// (turn-based-agent, 2026-10-03, #17).
//
// It decides over other tools' answers alone, so it lives here and is
// tested with stand-ins: `dialogue.step` (the game's thread's one act,
// unlisted), `inspect` kind=dialogue, `health`, and the bus's
// eluvian.dialogue.line, .replies and .chosen and eluvian.mode.changed.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddDialogueTool(Bench &bench);

} // namespace eluvian
