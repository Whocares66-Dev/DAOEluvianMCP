#include "core/Requirements.h"

#include <catch2/catch_test_macros.hpp>

using namespace eluvian;

TEST_CASE("an item's requirements are read as the screens split the engine's answer", "[requirements]")
{
    // A strength requirement not met, a class met, and a creature that
    // cannot wear the item at all, as the engine answers them.
    const auto requirements = ItemRequirements("0;Strength 34;1;Warrior;0;CANNOT_EQUIP_ON_CREATURE;");
    REQUIRE(requirements.size() == 3);
    CHECK(requirements[0] == Json{{"met", false}, {"text", "Strength 34"}});
    CHECK(requirements[1] == Json{{"met", true}, {"text", "Warrior"}});
    CHECK(requirements[2] == Json{{"met", false}, {"text", "CANNOT_EQUIP_ON_CREATURE"}});
}

TEST_CASE("an item with no requirements, or a piece left over, reads as the screens read it", "[requirements]")
{
    CHECK(ItemRequirements("").empty());
    CHECK(ItemRequirements("1;Strength 10").size() == 1);   // no closing ';'
    CHECK(ItemRequirements("1;Strength 10;0").size() == 1); // a lone piece after
    CHECK(ItemRequirements("2;Odd;")[0]["met"] == false);   // only "1" is met
}
