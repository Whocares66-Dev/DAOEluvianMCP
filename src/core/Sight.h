// Where the game's camera must stand to have a point in the middle of its
// view: the arithmetic of `camera`'s face and turn, apart from the game so a
// test holds its signs. A check of the sun's size compared a waterfall's
// mist instead, because face turned the camera toward a point 38 degrees up
// and left it looking along the ground (shaders-agent, 2026-10-11, #117).
//
// The camera, as DAOLib's dao/Camera.h reads it: it looks at a pivot, which
// sits above the focus (the followed creature's place), from a point on a
// sphere about that pivot,
//     x = r sin(pitch) cos(yaw), y = r sin(pitch) sin(yaw), z = r cos(pitch).
// So the yaw is the direction from the pivot to the camera, from +x toward
// +y, and the pitch is the angle from straight above: a quarter turn is
// level, less looks down, more looks up from below the pivot. A point is in
// the middle of the view when the camera is on the far side of the pivot
// from it.
#pragma once

#include <array>
#include <optional>

namespace eluvian
{

using Point = std::array<float, 3>; // x, y, z, in the world's metres

// The camera's two angles, in radians, as dao::SceneCamera measures them.
struct Sight
{
    float yaw = 0;
    float pitch = 0;
};

// The angles that put `to` in the middle of the view of a camera whose pivot
// is at `pivot`. Nothing when the two are one point: there is no direction.
// Straight above or below the pivot the yaw is 0, since any yaw sees it.
std::optional<Sight> SightTo(const Point &pivot, const Point &to);

// How high the pivot is, from where the camera stands (`position`), the
// focus it is above and its pitch: the camera is a horizontal r sin(pitch)
// from the focus and r cos(pitch) above the pivot. The focus's own height
// when the camera is too nearly straight above or below to tell.
float PivotHeight(const Point &position, const Point &focus, float pitch);

// The pitch the game keeps a close view at: it clamps the tilt to between
// 30 and 150 degrees every frame (the explore controller's, DAOLib's
// dao/Camera.h, SetPitch), so one set outside them is not what is seen.
float HeldPitch(float pitch);

// The angles every tool reads and writes (#143), one measure for a creature
// and the camera alike, so a creature's facing given to the camera looks the
// way it does. `facingDegrees` is the game's own facing, the scripts'
// GetFacing: 0 looks along -y and it grows clockwise seen from above, -90
// along +x, 90 along -x, 180 along +y (the hero turned by SetFacing and
// walked, GetOrientation read with it, 2026-10-15). `pitchDegrees` is 0
// level, up positive, -90 straight down. Facings are wrapped to -180..180.
float FacingDegrees(float yaw);
float YawOfFacing(float degrees);
float PitchDegrees(float pitch);
float PitchOfDegrees(float degrees);

} // namespace eluvian
