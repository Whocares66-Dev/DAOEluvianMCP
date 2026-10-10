// `loot`: take what a container holds, in one call: walk to it, open it,
// take all, and answer what was taken. Before it a test that met a chest or
// a body's remains was stuck in the loot panel with no mouse to press its
// buttons (turn-based-agent's playthrough, 2026-10-02, #1).
//
// It decides over other tools' answers alone, so it lives here and is
// tested with stand-ins, as travel is: `travel` (the walk, doors opened on
// the way), `order` (the use, by click, which opens a container),
// `gui` (the panel and what it holds, a message box, the
// mode), `press` (Take All, as the panel's own button does it)
// and the bus's eluvian.command.ended. It stops and says why where the
// container does not open: locked, a fight, a conversation, a message box.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// `show`: how long it lingers on the panel before Take All unless a call
// says (`showMs`): the plugin's is a second, so whoever watches sees it (#14).
void AddLootTool(Bench &bench, std::chrono::milliseconds show = {});

} // namespace eluvian
