#include "core/Placeables.h"

#include <array>

namespace eluvian
{

std::string_view PlaceableStateName(std::string_view controller, int state)
{
    // PLC_STATE_DOOR_*: UNLOCKED, LOCKED, OPEN, DEAD, OPEN_2, DEAD_2.
    static constexpr std::array<std::string_view, 6> kDoor = {"shut",      "locked", "open",
                                                              "destroyed", "open",   "destroyed"};
    // PLC_STATE_CONTAINER_*: UNLOCKED, LOCKED, OPEN, DEAD.
    static constexpr std::array<std::string_view, 4> kContainer = {"shut", "locked", "open", "destroyed"};
    // PLC_STATE_CONTAINER_STATIC_*: UNLOCKED, LOCKED, DEAD.
    static constexpr std::array<std::string_view, 3> kStatic = {"shut", "locked", "destroyed"};
    // A state below zero, cast, is beyond every list.
    const auto name = [state](const auto &names) -> std::string_view {
        return static_cast<std::size_t>(state) < names.size() ? names[static_cast<std::size_t>(state)]
                                                              : std::string_view();
    };
    if (controller == "StateCnt_Door")
    {
        return name(kDoor);
    }
    if (controller == "StateCnt_Container")
    {
        return name(kContainer);
    }
    if (controller == "StateCnt_Container_Static")
    {
        return name(kStatic);
    }
    return {};
}

std::string_view PlaceableActionName(int action)
{
    // placeable_interactions' rows; 9, 13, 14 and 17 name nothing used.
    static constexpr std::array<std::string_view, 19> kActions = {
        "",           "open", "close",  "unlock", "destroy",      "use", "examine", "disarm",
        "flip cover", "",     "dialog", "loot",   "trigger trap", "",    "",        "area transition",
        "turn left",  "",     "repair"};
    return static_cast<std::size_t>(action) < kActions.size() ? kActions[static_cast<std::size_t>(action)]
                                                              : std::string_view();
}

} // namespace eluvian
