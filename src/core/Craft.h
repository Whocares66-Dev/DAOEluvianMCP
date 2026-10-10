// `craft`: poisons, potions and traps made as the game's crafting screen
// makes them, so that its rules hold (the recipes the party knows, the
// skill each needs, the ingredients). Before it a playthrough driven by MCP
// alone had ingredients and two poison-makers and nothing to coat a blade
// with: no merchant sells a made poison, and the screen could not be driven
// (turn-based-agent, 2026-10-05, #59).
//
// The game's own way, read from crafting.gfx and the scripts and seen in
// play on Lyna (2026-10-05): a crafting skill's use runs the scripts'
// ShowCraftingGUI(ability), which opens the screen ("Crafting") for the
// character selected; the screen lists its recipes in the data tree
// (GUICrafting.Recipes.<i>: ID, Name, ProductName, Skill, HaveAllIngredients,
// Ingredients.<k> with Name, Need and Have), lets one be made while
// HaveAllIngredients holds, and its Create button sends
// "GUICrafting.CraftItem <recipe ID> <character>"; GUI.HideFullScreenGUI
// closes it. It decides over other tools' answers alone (`script`, `input`,
// the reads, `control`), so it lives here and is tested with stand-ins, as
// levelup is.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// `show`: how long it lingers on the screen before it crafts and after,
// unless a call says (`showMs`): the plugin's is a second, so whoever
// watches sees it.
void AddCraftTool(Bench &bench, std::chrono::milliseconds show = {});

} // namespace eluvian
