// `options`: the game's options screen worked as a player works it: opened
// from the start menu or Escape's menu with that menu's backing panel, a tab
// shown by its own button, a plugin's control set as a click on it does, and
// OK or Cancel. Agents drove the screen through movie calls, which showed a
// page under another tab's light and with no panel (the user,
// DAOElgaraShaders #68, 2026-10-30; #190).
//
// Each step is DAOLib's dao::options (DAOLib #272), through the unlisted
// options.state, options.open, options.tab, options.set, options.ok and
// options.cancel. The screen opens a few frames after it is asked for and a
// plugin's control takes a click a frame after it, so the waiting is decided
// here, over those tools' answers, and tested with stand-ins. The game's own
// controls are listed and set too since DAOLib #273, with their tab shown.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddOptionsTool(Bench &bench);

} // namespace eluvian
