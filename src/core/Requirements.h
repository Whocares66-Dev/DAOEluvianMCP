// An item's requirements for a creature, as the game itself says them: the
// engine's GetItemRequirements answers one string, "<0|1>;<text>;" for each
// requirement (its attributes', then table 107's, then
// "0;CANNOT_EQUIP_ON_CREATURE;" when the creature cannot wear the item at
// all), which the screens' DAExternalCommands splits on ';' (inventory.gfx;
// the handler read by lib-agent, 2026-10-05). An equip the game refused said
// nothing of why (#31); this is the why, as its inventory screen shows it.
#pragma once

#include "core/Tool.h"

#include <string_view>

namespace eluvian
{

// Each requirement as {met, text}, in the engine's order. A trailing piece
// with no pair is left out, as the screens' loop leaves it.
Json ItemRequirements(std::string_view answer);

} // namespace eluvian
