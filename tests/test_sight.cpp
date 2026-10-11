#include "core/Sight.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace eluvian;
using Catch::Approx;

namespace
{

constexpr float kPi = std::numbers::pi_v<float>;

float Degrees(float radians)
{
    return radians * 180.0F / kPi;
}

// Where a camera of these angles stands, `r` from its pivot, as the game
// places it (dao/Camera.h).
Point Standing(const Point &pivot, const Sight &sight, float r)
{
    return {pivot[0] + (r * std::sin(sight.pitch) * std::cos(sight.yaw)),
            pivot[1] + (r * std::sin(sight.pitch) * std::sin(sight.yaw)), pivot[2] + (r * std::cos(sight.pitch))};
}

} // namespace

TEST_CASE("a point on the pivot's own level is faced from the far side, level", "[sight]")
{
    // East of the pivot: the camera stands to the west.
    const auto east = SightTo({0, 0, 1.5F}, {10, 0, 1.5F});
    REQUIRE(east);
    CHECK(Degrees(east->yaw) == Approx(180).margin(0.01));
    CHECK(Degrees(east->pitch) == Approx(90).margin(0.01));
    // North of it: the camera stands to the south.
    const auto north = SightTo({0, 0, 1.5F}, {0, 10, 1.5F});
    REQUIRE(north);
    CHECK(Degrees(north->yaw) == Approx(-90).margin(0.01));
    CHECK(Degrees(north->pitch) == Approx(90).margin(0.01));
}

TEST_CASE("a point above the pivot is looked up at, from below level", "[sight]")
{
    // The sun of #117: 38 degrees up. The camera goes 38 degrees past level.
    const float up = std::tan(38.0F * kPi / 180.0F) * 100.0F;
    const auto sun = SightTo({5, 5, 2}, {105, 5, 2 + up});
    REQUIRE(sun);
    CHECK(Degrees(sun->pitch) == Approx(128).margin(0.01));
    CHECK(Degrees(sun->yaw) == Approx(180).margin(0.01));
    // And one below is looked down at.
    const auto floor = SightTo({5, 5, 2}, {7, 5, 0});
    REQUIRE(floor);
    CHECK(Degrees(floor->pitch) == Approx(45).margin(0.01));
}

TEST_CASE("the camera, the pivot and the point faced are on one line, the pivot between", "[sight]")
{
    const Point pivot{3, -4, 1.7F};
    const Point to{-9, 12, 6.2F};
    const auto sight = SightTo(pivot, to);
    REQUIRE(sight);
    const Point camera = Standing(pivot, *sight, 3.8F);
    // From the camera to the pivot is the way from the pivot to the point.
    const float reach = std::hypot(to[0] - pivot[0], to[1] - pivot[1], to[2] - pivot[2]);
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        CHECK((pivot[axis] - camera[axis]) / 3.8F == Approx((to[axis] - pivot[axis]) / reach).margin(0.0001));
    }
}

TEST_CASE("a point straight above the pivot has a pitch and no yaw to choose; the pivot itself has neither", "[sight]")
{
    const auto above = SightTo({1, 1, 1}, {1, 1, 9});
    REQUIRE(above);
    CHECK(Degrees(above->pitch) == Approx(180).margin(0.01));
    CHECK(above->yaw == 0.0F);
    const auto below = SightTo({1, 1, 1}, {1, 1, -9});
    REQUIRE(below);
    CHECK(Degrees(below->pitch) == Approx(0).margin(0.01));
    CHECK_FALSE(SightTo({1, 1, 1}, {1, 1, 1}));
    CHECK_FALSE(SightTo({1, 1, 1}, {1.0002F, 1, 1}));
}

TEST_CASE("the pivot's height is read back from where the camera stands", "[sight]")
{
    const Point focus{10, 20, 0.3F};
    const Point pivot{10, 20, 1.9F};
    // Close in, looking down a little; from below, looking up; the tactical view's 45 from afar.
    const std::array<std::array<float, 2>, 3> views{{{85.0F, 3.8F}, {128.0F, 3.8F}, {45.0F, 21.3F}}};
    for (const auto &[pitch, r] : views)
    {
        const Sight sight{0.7F, pitch * kPi / 180.0F};
        CHECK(PivotHeight(Standing(pivot, sight, r), focus, sight.pitch) == Approx(1.9F).margin(0.001));
    }
    // Straight above, the ground says nothing of the distance: the focus's own height.
    const Sight top{0.7F, 2.0F * kPi / 180.0F};
    CHECK(PivotHeight(Standing(pivot, top, 21.3F), focus, top.pitch) == 0.3F);
}

TEST_CASE("the close view keeps the tilt between 30 and 150 degrees", "[sight]")
{
    CHECK(Degrees(HeldPitch(10.0F * kPi / 180.0F)) == Approx(30).margin(0.01));
    CHECK(Degrees(HeldPitch(128.0F * kPi / 180.0F)) == Approx(128).margin(0.01));
    CHECK(Degrees(HeldPitch(170.0F * kPi / 180.0F)) == Approx(150).margin(0.01));
}
