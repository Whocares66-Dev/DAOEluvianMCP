// `camera`: read it, face a target, follow a creature, turn, tilt and zoom:
// so that a picture shows what a test means it to.
#include "core/Sight.h"
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;

// The camera, so a picture shows what a test means it to: a tester asked
// for a way to turn it to a target before a screenshot (2026-10-01). The
// turn is at once, not eased: a test wants the frame after to be the view.
// DAOLib's eased moves are one plugin's (dao/CameraMoves.h); a turn from
// here looks to a mod easing the camera like the player's own, and stops
// its move, as a player's would.
Json CameraJson(const dao::CameraController &camera)
{
    Json answer = {{"following", Brief(camera.Target())},
                   {"focus", VectorJson(camera.Focus())},
                   {"focusOffset", VectorJson(camera.FocusOffset())},
                   {"tactical", camera.Tactical()},
                   {"zoom", camera.Zoom()},
                   {"zoomSetting", camera.ZoomSetting()}};
    if (const auto *scene = camera.Scene())
    {
        answer["position"] = VectorJson(scene->Position());
        answer["yawDegrees"] = scene->Yaw() * kDegrees;
        answer["pitchDegrees"] = scene->Pitch() * kDegrees;
    }
    return answer;
}

Point PointOf(const dao::Vector &v)
{
    return {v.x, v.y, v.z};
}

// Where to turn the camera, and where to tilt it if it is to be tilted.
struct Aim
{
    float yaw = 0;
    std::optional<float> pitch;
};

// The angles `turn` asks for: either or both.
Aim TurnOf(const Args &args, const dao::SceneCamera &scene)
{
    if (!args.Has("yawDegrees") && !args.Has("pitchDegrees"))
    {
        throw ToolError("camera: turn needs `yawDegrees`, `pitchDegrees` or both");
    }
    Aim aim{scene.Yaw(), std::nullopt};
    if (args.Has("yawDegrees"))
    {
        aim.yaw = static_cast<float>(args.Number("yawDegrees")) / kDegrees;
    }
    if (args.Has("pitchDegrees"))
    {
        const double pitch = args.Number("pitchDegrees");
        if (pitch < 0 || pitch > 180)
        {
            throw ToolError("camera: `pitchDegrees` is the angle from straight above, 0 to 180: 90 is level, "
                            "less looks down, more looks up");
        }
        aim.pitch = static_cast<float>(pitch) / kDegrees;
    }
    return aim;
}

// The angles `face` turns to. Without `tilt` the yaw alone, from the focus,
// as it was before #117: a creature faced keeps the tilt the player set.
// With it both, from the pivot the camera looks at, so the point is in the
// middle of the picture and not only ahead on the ground.
Aim FaceOf(const Args &args, const dao::CameraController &camera, const dao::SceneCamera &scene)
{
    const auto target = args.OptionalId("target", kCreatureHint);
    if (!target && !args.Has("point"))
    {
        throw ToolError("camera: face needs a `target` or a `point`");
    }
    const auto to = target ? api::GetPosition(*target) : VectorOf(args.Raw().at("point"), "camera: `point`");
    const auto focus = camera.Focus();
    if (args.Bool("tilt", false))
    {
        const Point pivot{focus.x, focus.y, PivotHeight(PointOf(scene.Position()), PointOf(focus), scene.Pitch())};
        const auto sight = SightTo(pivot, PointOf(to));
        if (!sight)
        {
            throw ToolError("camera: the camera looks at that point already, so there is no direction to "
                            "face; follow another creature first, or turn to `yawDegrees` and `pitchDegrees`");
        }
        return {sight->yaw, sight->pitch};
    }
    if (Distance(focus, to) < 0.1F)
    {
        throw ToolError("camera: the focus is on that point already, so there is no direction to "
                        "face; follow another creature first, or turn to `yawDegrees`");
    }
    // The yaw is the direction from the focus to the camera:
    // the far side of the focus from what is to be seen.
    return {std::atan2(focus.y - to.y, focus.x - to.x), std::nullopt};
}

} // namespace

namespace
{

// The camera tool's handler, registered by AddCamera.
ToolResult ToolCamera(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "camera");
    const Args args(raw, "camera");
    const auto action = args.Choice("action", {"read", "face", "follow", "turn", "zoom"}, "read");
    auto *camera = dao::camera::Current();
    if (camera == nullptr)
    {
        throw ToolError("camera: there is no camera to move now: it moves in explore and combat mode, not "
                        "in a menu, a conversation, a cutscene or while loading (health says the mode)");
    }
    auto *scene = camera->Scene();
    std::string note = "the view is as asked from the next frame: screenshot after this call";
    if (action == "follow")
    {
        const ObjectId target = args.Id("target", kCreatureHint);
        camera->Follow(target);
        // The tactical view keeps the focus where it was, as an offset from
        // whatever is followed, which the player's panning moves. Following
        // is to look at the target, so the offset goes: seen 2026-10-11
        // (#117), the party 9 m from the focus and in the picture's corner,
        // then at its middle after this, with the cursor held off the edge.
        camera->SetFocusOffset({});
        camera->SnapToTarget();
    }
    else if (action == "zoom")
    {
        const float least = camera->ZoomLeast();
        const float most = camera->ZoomMost();
        const auto zoom = static_cast<float>(args.Number("zoom"));
        if (zoom < least || zoom > most)
        {
            throw ToolError(std::format("camera: `zoom` is from {} (the tactical view) to {} (close in)", least, most));
        }
        camera->SetZoomSetting(zoom);
        note = "the camera eases to that zoom as it does for the mouse wheel, within about a second: `zoom` "
               "here is where it is now and `zoomSetting` where it is going. Before a screenshot, read "
               "again or wait_until {\"tool\": \"camera\", \"pointer\": \"/zoom\", ...}";
        if (zoom < 0)
        {
            // Seen 2026-10-11 (#117): the party in the picture's corner each time, 9 m from
            // the focus, with the real mouse on another monitor; centred once `pointer` held
            // the cursor in the middle. The game pans by the cursor at the screen's edge
            // (DAOLib's dao/Addresses.h, ReadInput) and then only keeps the party in view.
            note += ". That is the tactical view, where the game pans while the mouse is at the picture's "
                    "edge or off it: hold the cursor inside first (`pointer` with a `pixel` near the middle), "
                    "then `follow` a creature to centre on it";
        }
    }
    else if (action == "face" || action == "turn")
    {
        if (scene == nullptr)
        {
            throw ToolError("camera: the game has no scene camera to turn now");
        }
        const Aim aim = action == "turn" ? TurnOf(args, *scene) : FaceOf(args, *camera, *scene);
        scene->SetYaw(aim.yaw);
        if (aim.pitch)
        {
            scene->SetPitch(*aim.pitch);
            // What the game does with it the next frame, so the answer says
            // what will be seen. Seen 2026-10-11: 170 asked close in read 150
            // a frame later, and 70 asked in the tactical view read 45.
            const float held = HeldPitch(*aim.pitch);
            if (camera->Tactical())
            {
                note = "the turn shows from the next frame, the tilt does not: the tactical view keeps its own "
                       "tilt of 45 degrees. Zoom in first (`zoom` above 0) to tilt";
            }
            else if (std::abs(held - *aim.pitch) > 0.001F)
            {
                note += std::format(". Close in, the game keeps the tilt between 30 and 150 degrees, so this one "
                                    "shows as {:.0f}",
                                    held * kDegrees);
            }
        }
    }
    Json answer = CameraJson(*camera);
    answer["action"] = action;
    if (action != "read")
    {
        answer["note"] = note;
    }
    return answer;
}

} // namespace

void AddCamera(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"camera",
                 "Reads or moves the camera, in explore or combat mode. `action`:\n\n"
                 "- `read` (the default): returns what it is `following`, its `focus` and how far that is from "
                 "what it follows (`focusOffset`), its `position`, `yawDegrees` and `pitchDegrees`, whether it "
                 "is `tactical`, `zoom` (1 close in, -1 the tactical view) and `zoomSetting`, the zoom it is "
                 "easing toward.\n"
                 "- `face`: turns it at once so that `target` or `point` is straight ahead, beyond the focus, "
                 "for a screenshot that shows it. With `tilt` true it tilts as well, so the target is in the "
                 "middle of the picture: use it for something above or below, such as the sky. A creature's "
                 "point is at its feet, so one standing near is looked down on.\n"
                 "- `follow`: makes it follow `target` and jump there at once, as selecting a party member does "
                 "(nothing gets selected). Use it to look at a creature elsewhere, or to centre the tactical "
                 "view on one.\n"
                 "- `turn`: turns it to `yawDegrees`, tilts it to `pitchDegrees`, or both. The tactical view "
                 "keeps its own tilt of 45 degrees, so tilt close in.\n"
                 "- `zoom`: sets `zoom` as the mouse wheel does, from -1 (the tactical view, high above) to 1 "
                 "(close in). The camera eases there within about a second. In the tactical view (below 0) the "
                 "game pans the camera while the mouse is at the picture's edge or off it, so the party drifts "
                 "to a corner (`focusOffset` grows): first hold the cursor inside the picture with `pointer` "
                 "(a `pixel` near the middle), then `follow` a creature to centre on it.\n\n"
                 "Every action returns the camera as `read` does. A turn or tilt shows from the next frame, so "
                 "take the screenshot after this call; after `zoom`, read until `zoom` has arrived. A mod that "
                 "smooths camera moves treats a move from here as the player's and stops its own.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"read", "face", "follow", "turn", "zoom"}},
                          {"description", "What to do. Default read."}}},
                        {"target", {{"description", "With `face` or `follow`: the creature or object, by id."}}},
                        {"point", {{"description", "With `face`: a world point, {x, y, z} or [x, y, z]."}}},
                        {"tilt",
                         {{"type", "boolean"},
                          {"description", "With `face`: also tilt, so the target is in the middle of the picture "
                                          "and not only ahead. Default false: the tilt stays as it is."}}},
                        {"yawDegrees",
                         {{"type", "number"},
                          {"description", "With `turn`: the direction from the focus to the camera, in degrees, "
                                          "measured from +x toward +y."}}},
                        {"pitchDegrees",
                         {{"type", "number"},
                          {"description", "With `turn`: the camera's angle from straight above, 0 to 180. 90 is "
                                          "level, less looks down, more looks up. Close in, the game keeps it "
                                          "between 30 and 150."}}},
                        {"zoom",
                         {{"type", "number"},
                          {"description", "With `zoom`: from -1 (the tactical view) to 1 (close in)."}}}}}},
                 Thread::Frame},
        [&watch](const Json &raw) -> ToolResult { return ToolCamera(watch, raw); });
}

} // namespace eluvian::game
