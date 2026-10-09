// What a placeable's state means: a number whose meaning is its state
// controller's. The scripts read the controller from the placeable types
// table (93) by the placeable's appearance (core_h.nss,
// GetPlaceableStateCntTable), and name each controller's numbers in
// 2da_constants_h.nss. A door's 1 is locked where a shut door beside it
// reads 0 (turn-based-agent, 2026-10-04, #44).
#pragma once

#include <string_view>

namespace eluvian
{

// The word for `state` under `controller` (StateCnt_Door, StateCnt_Container,
// StateCnt_Container_Static): shut, locked, open or destroyed. Empty for
// another controller or a number it does not name.
std::string_view PlaceableStateName(std::string_view controller, int state);

// What a placeable's use does now, as the scripts' GetPlaceableAction
// answers it: the action of its current state in its state controller's
// table (placeable_interactions), so a locked chest's is unlock until it is
// open (lib-agent, read from the game's 2da.erf, 2026-10-06, #88).
inline constexpr int kPlaceableActionUnlock = 3;
inline constexpr int kPlaceableActionLoot = 11;

// The word for a placeable's action: open, close, unlock, destroy, use,
// examine, disarm, flip cover, dialog, loot, trigger trap, area
// transition, turn left, repair. Empty for a number not named.
std::string_view PlaceableActionName(int action);

} // namespace eluvian
