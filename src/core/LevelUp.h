// `levelup`: spend a creature's level-up points as the game's level-up
// screen does, so that its rules hold (what a class may take, each talent's
// prerequisites, the derived stats). Before it the points of a playthrough
// driven by MCP alone stayed banked: spending them by script would skip the
// screen's rules (turn-based-agent, 2026-10-02, #5).
//
// Since #180 the steps are DAOLib's dao::levelup (DAOLib #259), with no
// screen: levelup.offers, levelup.take, levelup.raise, levelup.auto and
// levelup.finish, each refused by the engine's own test where it pressed
// the screen's icons through its movie. The screen is only shown, for
// whoever watches (#14): "GUI.GoToLevelUp <id> 0" opens it for a creature,
// a pick shows on it, and its exit point set and "GUI.HideFullScreenGUI"
// end it as its Done does (levelup.gfx, seen in play on Lyna, 2026-10-02).
// It decides over other tools' answers alone (those, `input`, `movie` and
// `gui` for the screen, character and abilities before and after), so it
// lives here and is tested with stand-ins, as travel and loot are.
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
