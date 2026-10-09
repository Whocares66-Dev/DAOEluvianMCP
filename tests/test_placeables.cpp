#include "core/Placeables.h"

#include <catch2/catch_test_macros.hpp>

using namespace eluvian;

TEST_CASE("a door's states are named as the scripts name them", "[placeables]")
{
    CHECK(PlaceableStateName("StateCnt_Door", 0) == "shut");
    CHECK(PlaceableStateName("StateCnt_Door", 1) == "locked"); // the Ruined Temple's wraith chamber door
    CHECK(PlaceableStateName("StateCnt_Door", 2) == "open");
    CHECK(PlaceableStateName("StateCnt_Door", 3) == "destroyed");
    CHECK(PlaceableStateName("StateCnt_Door", 4) == "open");
    CHECK(PlaceableStateName("StateCnt_Door", 5) == "destroyed");
}

TEST_CASE("a container's states, and a static container's, are named apart", "[placeables]")
{
    CHECK(PlaceableStateName("StateCnt_Container", 1) == "locked");
    CHECK(PlaceableStateName("StateCnt_Container", 2) == "open");
    CHECK(PlaceableStateName("StateCnt_Container", 3) == "destroyed");
    CHECK(PlaceableStateName("StateCnt_Container_Static", 0) == "shut");
    CHECK(PlaceableStateName("StateCnt_Container_Static", 2) == "destroyed"); // no open state of its own
}

TEST_CASE("another controller, or a number outside a controller's, has no word", "[placeables]")
{
    CHECK(PlaceableStateName("StateCnt_AreaTransition", 0).empty());
    CHECK(PlaceableStateName("StateCnt_Door", 6).empty());
    CHECK(PlaceableStateName("StateCnt_Door", -1).empty());
    CHECK(PlaceableStateName("StateCnt_Container_Static", 3).empty());
    CHECK(PlaceableStateName("", 1).empty());
}

TEST_CASE("a placeable's action is named as placeable_interactions names it", "[placeables]")
{
    CHECK(PlaceableActionName(kPlaceableActionLoot) == "loot");
    CHECK(PlaceableActionName(kPlaceableActionUnlock) == "unlock");
    CHECK(PlaceableActionName(1) == "open");
    CHECK(PlaceableActionName(5) == "use");
    CHECK(PlaceableActionName(6) == "examine");
    CHECK(PlaceableActionName(15) == "area transition");
    CHECK(PlaceableActionName(18) == "repair");
    CHECK(PlaceableActionName(0).empty());
    CHECK(PlaceableActionName(9).empty());
    CHECK(PlaceableActionName(19).empty());
    CHECK(PlaceableActionName(-1).empty());
}
