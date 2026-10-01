// `screen`: where a thing is in the game's picture, for a test that must
// use the real mouse: a pixel to click a creature or an object at, a
// world point's pixel, or what lies under a pixel. Each place comes in the
// window's client pixels, as a fraction of the picture, and as a desktop
// pixel. DAOLib's dao::world::ToScreen, AimAt and PickAt (48df3b4).
#include "game/Shared.h"

#include "game/Capture.h"

namespace eluvian::game
{

namespace
{

// A place in the view three ways: the game's own pixels, a fraction of the
// picture (what survives a screenshot's scaling), and the desktop's pixel a
// real mouse is sent to. The tester's tools work in a scaled frame of the
// desktop, so fractions and the window's rectangle are what it converts
// from (computer-use-agent, 2026-10-01).
Json PlaceJson(float x, float y, const dao::world::ViewSize &view, const WindowPlace &window)
{
    Json place = {{"x", x}, {"y", y}};
    if (view.width > 0 && view.height > 0)
    {
        const float fx = x / static_cast<float>(view.width);
        const float fy = y / static_cast<float>(view.height);
        place["fx"] = fx;
        place["fy"] = fy;
        if (window.found)
        {
            place["desktop"] = Json{{"x", window.x + static_cast<int>(fx * static_cast<float>(window.width))},
                                    {"y", window.y + static_cast<int>(fy * static_cast<float>(window.height))}};
        }
    }
    return place;
}

Json PointJson(const dao::world::ScreenPoint &point, const dao::world::ViewSize &view, const WindowPlace &window)
{
    Json json = PlaceJson(point.x, point.y, view, window);
    json["inFront"] = point.inFront;
    json["inView"] = point.inView;
    return json;
}

Json HitsJson(std::span<const dao::world::Hit> hits)
{
    Json list = Json::array();
    for (const auto &hit : hits)
    {
        if (list.size() == 8)
        {
            break;
        }
        list.push_back(Json{{"kind", hit.kind == dao::world::Hit::Kind::Object   ? "object"
                                     : hit.kind == dao::world::Hit::Kind::Ground ? "ground"
                                                                                 : "nothing"},
                            {"object", Brief(hit.object)},
                            {"position", VectorJson(hit.position)}});
    }
    return list;
}

} // namespace

void AddScreen(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"screen",
                 "Where a thing is in the game's picture, for the real mouse. `target` (a creature or object id): "
                 "`aim`, a pixel to click it at, on its body and where nothing else is in front (null if it is "
                 "out of view or hidden), and `position`, where its feet are. `point` ({x, y, z}): that world "
                 "point's pixel. `pixel` ([x, y] in the game's own pixels): `hits`, what lies under it, nearest "
                 "first, the first object being what a click there would act on. With none of them: the view's "
                 "size and the window. Each place is given as `x`, `y` (the game's pixels from the picture's top "
                 "left), `fx`, `fy` (fractions of the picture) and `desktop` (the pixel to send a real mouse "
                 "to). After moving the mouse, inspect kind=cursor says what the game has under it.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"target", {{"description", "A creature or object id"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z] in the world"}}},
                        {"pixel", {{"description", "[x, y] in the game's pixels"}}}}}},
                 Thread::Frame, true},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "screen");
            const Args args(raw, "screen");
            const auto view = dao::world::ScreenSize();
            if (!view)
            {
                throw ToolError("screen: the game has no view to measure now (health says the mode)");
            }
            const auto window = GameWindowPlace();
            Json answer = {{"view", {{"width", view->width}, {"height", view->height}}}};
            if (window.found)
            {
                answer["window"] = Json{
                    {"client", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height}}},
                    {"minimised", window.minimised},
                    {"foreground", window.foreground}};
            }
            if (const auto target = args.OptionalId("target", "a creature or object id"))
            {
                answer["target"] = Brief(*target);
                const auto aim = dao::world::AimAt(*target);
                answer["aim"] = aim ? PointJson(*aim, *view, window) : Json(nullptr);
                const auto feet = dao::world::ToScreen(api::GetPosition(*target));
                answer["position"] = feet ? PointJson(*feet, *view, window) : Json(nullptr);
                if (!aim)
                {
                    answer["note"] = "no pixel to click it at: it is out of view, or another object is in front of "
                                     "it wherever it shows; camera action=face or follow may bring it into view";
                }
            }
            if (args.Has("point"))
            {
                const auto point = dao::world::ToScreen(VectorOf(args.Raw().at("point"), "screen: `point`"));
                answer["point"] = point ? PointJson(*point, *view, window) : Json(nullptr);
            }
            if (args.Has("pixel"))
            {
                const auto &pixel = args.Raw().at("pixel");
                if (!pixel.is_array() || pixel.size() != 2 || !pixel[0].is_number() || !pixel[1].is_number())
                {
                    throw ToolError("screen: `pixel` is [x, y] in the game's pixels, from the picture's top left");
                }
                const auto x = static_cast<std::int32_t>(pixel[0].get<double>());
                const auto y = static_cast<std::int32_t>(pixel[1].get<double>());
                answer["pixel"] = PlaceJson(static_cast<float>(x), static_cast<float>(y), *view, window);
                answer["hits"] = HitsJson(dao::world::PickAt(x, y));
            }
            return answer;
        });
}

} // namespace eluvian::game
