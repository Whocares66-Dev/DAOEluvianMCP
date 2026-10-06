// `camera`: read it, face a target, follow a creature, turn: so that a
// picture shows what a test means it to.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

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
                   {"tactical", camera.Tactical()},
                   {"zoom", camera.Zoom()}};
    if (const auto *scene = camera.Scene())
    {
        answer["position"] = VectorJson(scene->Position());
        answer["yawDegrees"] = scene->Yaw() * 180.0F / std::numbers::pi_v<float>;
        answer["pitchDegrees"] = scene->Pitch() * 180.0F / std::numbers::pi_v<float>;
    }
    return answer;
}

} // namespace

namespace
{

// The camera tool's handler, registered by AddCamera.
ToolResult ToolCamera(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "camera");
    const Args args(raw, "camera");
    const auto action = args.Choice("action", {"read", "face", "follow", "turn"}, "read");
    auto *camera = dao::camera::Current();
    if (camera == nullptr)
    {
        throw ToolError("camera: there is no camera to move now: it moves in explore and combat mode, not "
                        "in a menu, a conversation, a cutscene or while loading (health says the mode)");
    }
    auto *scene = camera->Scene();
    if (action == "follow")
    {
        const ObjectId target = args.Id("target", kCreatureHint);
        camera->Follow(target);
        camera->SnapToTarget();
    }
    else if (action == "face" || action == "turn")
    {
        if (scene == nullptr)
        {
            throw ToolError("camera: the game has no scene camera to turn now");
        }
        float yaw = 0;
        if (action == "turn")
        {
            if (!args.Has("yawDegrees"))
            {
                throw ToolError("camera: turn needs `yawDegrees`");
            }
            yaw = static_cast<float>(args.Number("yawDegrees", 0)) * std::numbers::pi_v<float> / 180.0F;
        }
        else
        {
            const auto target = args.OptionalId("target", kCreatureHint);
            if (!target && !args.Has("point"))
            {
                throw ToolError("camera: face needs a `target` or a `point`");
            }
            const auto to = target ? api::GetPosition(*target) : VectorOf(args.Raw().at("point"), "camera: `point`");
            const auto focus = camera->Focus();
            if (Distance(focus, to) < 0.1F)
            {
                throw ToolError("camera: the focus is on that point already, so there is no direction to "
                                "face; follow another creature first, or turn to `yawDegrees`");
            }
            // The yaw is the direction from the focus to the camera:
            // the far side of the focus from what is to be seen.
            yaw = std::atan2(focus.y - to.y, focus.x - to.x);
        }
        scene->SetYaw(yaw);
    }
    Json answer = CameraJson(*camera);
    answer["action"] = action;
    if (action != "read")
    {
        answer["note"] = "the view is as asked from the next frame: screenshot after this call";
    }
    return answer;
}

} // namespace

void AddCamera(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"camera",
                 "The camera in explore or combat mode. `action`: read (the default), what it follows, its focus, "
                 "position, yaw and pitch in degrees, zoom (1 close in, -1 the tactical view); face, turn it at "
                 "once so that `target` (a creature or object) or `point` lies ahead of the focus, for a "
                 "screenshot that shows it; follow, have it follow `target` and put it there at once, as "
                 "selecting a party member does (nothing is selected), to look at a creature elsewhere; turn, to "
                 "`yawDegrees`. The view is as asked from the next frame: take the screenshot after. A mod "
                 "easing the camera takes a turn from here for the player's and lets go.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"read", "face", "follow", "turn"}}}},
                        {"target", {{"description", "A creature or object id"}}},
                        {"point", {{"description", "action=face: {x, y, z} or [x, y, z]"}}},
                        {"yawDegrees",
                         {{"type", "number"},
                          {"description", "action=turn: the direction from the focus to the camera, from +x "
                                          "toward +y"}}}}}},
                 Thread::Frame},
        [&watch](const Json &raw) -> ToolResult { return ToolCamera(watch, raw); });
}

} // namespace eluvian::game
