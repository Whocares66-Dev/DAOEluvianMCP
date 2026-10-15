#include "core/Sight.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eluvian
{

namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegrees = 180.0F / kPi;
// Nearer than this, in metres, two points are one.
constexpr float kNoDistance = 0.001F;

} // namespace

std::optional<Sight> SightTo(const Point &pivot, const Point &to)
{
    const float dx = to[0] - pivot[0];
    const float dy = to[1] - pivot[1];
    const float dz = to[2] - pivot[2];
    const float flat = std::hypot(dx, dy);
    if (std::hypot(flat, dz) < kNoDistance)
    {
        return std::nullopt;
    }
    Sight sight;
    // The camera stands opposite the point: the yaw runs from the point to
    // the pivot, and on to the camera. The differences are taken that way
    // round, not negated: a zero negated is -0, and atan2(-0, -10) is a half
    // turn the other way from atan2(0, -10).
    sight.yaw = flat < kNoDistance ? 0.0F : std::atan2(pivot[1] - to[1], pivot[0] - to[0]);
    // A point above the pivot is seen from below it, past level.
    sight.pitch = (kPi / 2) + std::atan2(dz, flat);
    return sight;
}

float PivotHeight(const Point &position, const Point &focus, float pitch)
{
    const float sine = std::sin(pitch);
    // Within six degrees of straight above or below, the camera's distance
    // from the focus over the ground says little of how far it is.
    if (std::abs(sine) < 0.1F)
    {
        return focus[2];
    }
    const float flat = std::hypot(position[0] - focus[0], position[1] - focus[1]);
    return position[2] - (flat * std::cos(pitch) / sine);
}

float HeldPitch(float pitch)
{
    return std::clamp(pitch, kPi / 6, kPi * 5 / 6);
}

// The view looks from the camera through the pivot, a half turn from the
// yaw; a facing f looks along -90 - f degrees from +x toward +y. So the
// facing is 90 degrees less the yaw.
float FacingDegrees(float yaw)
{
    return std::remainder(90.0F - (yaw * kDegrees), 360.0F);
}

float YawOfFacing(float degrees)
{
    return (90.0F - degrees) / kDegrees;
}

float PitchDegrees(float pitch)
{
    return (pitch * kDegrees) - 90.0F;
}

float PitchOfDegrees(float degrees)
{
    return (degrees + 90.0F) / kDegrees;
}

} // namespace eluvian
