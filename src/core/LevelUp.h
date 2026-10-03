// `levelup`: spend a creature's level-up points as the game's level-up
// screen does, so that its rules hold (what a class may take, each talent's
// prerequisites, the derived stats). Before it the points of a playthrough
// driven by MCP alone stayed banked: spending them by script would skip the
// screen's rules (turn-based-agent, 2026-10-02, #5).
//
// The screen's own messages, read from levelup.gfx and portraits.gfx and
// seen in play on Lyna (2026-10-02): "GUI.GoToLevelUp <id> 0" opens it for
// a creature, "GUILevelUp.AutoLevelUp <id>" spends every point as its Auto
// Level Up does, "GUILevelUp.CharGenEnd <id> 1 1" ends it complete, and
// "GUI.HideFullScreenGUI" closes it. It decides over other tools' answers
// alone (`input` sends those, `inspect` kind=gui says the screen is up,
// kind=character and kind=abilities read the creature before and after),
// so it lives here and is tested with stand-ins, as travel and loot are.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// `show`: how long it lingers on the screen before its Auto Level Up and on
// the result after, unless a call says (`showMs`): the plugin's is a
// second, so whoever watches sees it (#14).
void AddLevelUpTool(Bench &bench, std::chrono::milliseconds show = {});

} // namespace eluvian
