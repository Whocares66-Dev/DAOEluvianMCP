// `craft`: poisons, potions, traps and runes made as the game's crafting
// screen makes them, so that its rules hold (the recipes the party knows,
// the skill each needs, the ingredients). Before it a playthrough driven by
// MCP alone had ingredients and two poison-makers and nothing to coat a blade
// with: no merchant sells a made poison, and the screen could not be driven
// (turn-based-agent, 2026-10-05, #59).
//
// Since #186 its steps are DAOLib's dao::crafting (DAOLib #259), with no
// screen, through the unlisted craft.recipes and craft.make: the recipes as
// the screen groups and shows them to a crafter, and one made as its Create
// does, refused where the screen greys it out. Until then craft opened the
// screen by the scripts' ShowCraftingGUI, gave the maker control (the screen
// crafts for whom the interface selects), read GUICrafting.Recipes from the
// data tree, grouped them by a copy of crafting.gfx's BaseCraftingSkillOf and
// sent "GUICrafting.CraftItem <recipe> <character>". It decides over other
// tools' answers alone (those, `state` and `character`), so it lives here
// and is tested with stand-ins, as levelup is.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddCraftTool(Bench &bench);

} // namespace eluvian
