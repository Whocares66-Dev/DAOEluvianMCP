// `inspect`: the game read, by `kind`: the game itself, its message box,
// the conversation on the screen, the placeables, the creatures, one
// creature in full, and the geometry between two. Reads only; plugins add
// kinds of their own through the C ABI.
#include "game/Shared.h"

#include "game/Capture.h"

#include "dao/Data.h"

namespace eluvian::game
{

void AddInspect(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(ToolSpec{
        "inspect",
        "Read the game: its state, the creatures in the area, one creature in full, the geometry between "
        "two. Creatures are named by id (\"0x7d9\", as the logs write them); pass those ids to "
        "every other tool.",
        Json{{"type", "object"},
             {"properties",
              {{"id", {{"description", "kind=creature and kind=geometry: the creature; default the one controlled"}}},
               {"target", {{"description", "kind=geometry: the creature to measure to"}}},
               {"point", {{"description", "kind=geometry: the point to measure to, {x, y, z} or [x, y, z]"}}},
               {"path", {{"type", "string"}, {"description", "kind=data: a dotted path in the data tree"}}},
               {"paths", {{"type", "array"}, {"description", "kind=data: several paths, answered together"}}},
               {"full", {{"type", "boolean"}, {"description", "kind=inventory: each item in full"}}},
               {"waypoints", {{"type", "boolean"}, {"description", "kind=geometry: list the paths' points"}}},
               {"walk",
                {{"type", "boolean"},
                 {"description", "kind=geometry: also search the path the creature would walk there: its length, "
                                 "or what is in the way"}}},
               {"trace",
                {{"type", "boolean"},
                 {"description", "kind=geometry with walk: the search's own record (each slice's answer, a line "
                                 "for a log), for whoever reads the pathfinder"}}},
               {"near",
                {{"description", "kind=creatures: measure distances from this creature; default "
                                 "the one controlled"}}},
               {"radius",
                {{"type", "number"}, {"description", "kind=creatures: only those within this many metres of `near`"}}},
               {"side",
                {{"type", "string"},
                 {"enum", {"party", "hostile", "neutral"}},
                 {"description", "kind=creatures: only this side"}}},
               {"includeDead", {{"type", "boolean"}, {"description", "kind=creatures: default true"}}},
               {"includeInactive",
                {{"type", "boolean"},
                 {"description", "kind=creatures: creatures the area keeps hidden; default false"}}}}}},
        Thread::Frame, true});

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"game",
                 "The game mode, pause, the area, the hero, the creature controlled, the party and who of it is "
                 "selected in the party bar, the game's message box if one is up (`popup`: its text and "
                 "buttons), and the event cursor."},
        [&bench, &watch](const Json &) -> ToolResult {
            Json answer = {{"mode", ModeName(watch.Mode())}, {"cursor", bench.Events().Next()}};
            const auto *app = dao::CClientExoApp::Get();
            answer["paused"] = app != nullptr && app->IsPaused();
            answer["popup"] = PopupJson();
            RequireWorld(watch, "inspect");
            const ObjectId hero = api::GetHero();
            answer["hero"] = Brief(hero);
            answer["controlled"] = Brief(api::GetMainControlled());
            answer["area"] = hero == kInvalidObject ? Json(nullptr) : Brief(api::GetArea(hero));
            Json party = Json::array();
            for (const ObjectId member : PartyOf(hero))
            {
                party.push_back(Brief(member));
            }
            answer["party"] = std::move(party);
            Json selected = Json::array();
            for (const ObjectId member : dao::party::Selected())
            {
                selected.push_back(Brief(member));
            }
            answer["selected"] = std::move(selected);
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"data",
                 "The game's data tree, which its interface is bound to: the value at a dotted `path` "
                 "(\"GUI.SelectedChar\", \"Map.ShowAllLabels\", \"GUIPopupLayer.DialogBoxText\"), as `text` "
                 "and as `number`, and its `type`; or several at once with `paths`. `found` is false for a path "
                 "the tree has not. What the interface shows of the game is read here without a picture. "
                 "Reading only. Answers in any mode."},
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "inspect");
            if (watch.Mode() < 0)
            {
                throw ToolError("inspect: the game is still starting; health says when it runs");
            }
            const auto read = [](const std::string &path) {
                Json value = {{"path", path}};
                const auto *attribute = dao::data::Find(Wide(path));
                value["found"] = attribute != nullptr;
                if (attribute != nullptr)
                {
                    using Type = dao::data::Attribute::Type;
                    const auto type = attribute->Kind();
                    value["type"] = type == Type::Bool          ? "bool"
                                    : type == Type::Int         ? "int"
                                    : type == Type::UnsignedInt ? "unsigned"
                                    : type == Type::Short       ? "short"
                                    : type == Type::Byte        ? "byte"
                                    : type == Type::Float       ? "float"
                                                                : "other";
                    value["text"] = Narrow(attribute->Text());
                    value["number"] = attribute->Number();
                }
                return value;
            };
            if (args.Has("paths"))
            {
                const auto &paths = args.Raw().at("paths");
                if (!paths.is_array() || paths.size() > 64)
                {
                    throw ToolError("inspect: `paths` is a list of up to 64 dotted paths");
                }
                Json values = Json::array();
                for (const auto &path : paths)
                {
                    if (!path.is_string())
                    {
                        throw ToolError("inspect: each of `paths` is a string, like \"GUI.SelectedChar\"");
                    }
                    values.push_back(read(path.get<std::string>()));
                }
                return Json{{"values", std::move(values)}};
            }
            return read(args.String("path"));
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"cursor",
                 "What the game says is under the real mouse cursor now, in explore and combat mode: `object`, "
                 "the one a click would act on (null over none), the `ground` point under it and whether it is "
                 "`walkable`, and `hits`, everything the cursor's ray meets, nearest first. After moving the "
                 "real mouse, ask before clicking: a click beside a creature is a move order. The game picks "
                 "again 15 times a second. No `hits` at all, not even ground, means the game cast no ray: the "
                 "cursor is over a piece of the interface (a creature's name label among them) or outside "
                 "the picture."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto *explore = dao::CClientExoApp::CurrentExplore();
            if (explore == nullptr)
            {
                throw ToolError("inspect: the cursor picks in explore and combat mode only (health says the mode)");
            }
            const auto object = explore->CursorObject();
            const auto ground = explore->CursorGround();
            Json hits = Json::array();
            for (const auto &hit : explore->CursorHits())
            {
                if (hits.size() == 8)
                {
                    break;
                }
                hits.push_back(Json{{"kind", hit.kind == dao::world::Hit::Kind::Object   ? "object"
                                             : hit.kind == dao::world::Hit::Kind::Ground ? "ground"
                                                                                         : "nothing"},
                                    {"object", Brief(hit.object)},
                                    {"position", VectorJson(hit.position)}});
            }
            return Json{{"object", object ? Brief(*object) : Json(nullptr)},
                        {"ground", ground ? VectorJson(*ground) : Json(nullptr)},
                        {"walkable", explore->CursorGroundWalkable()},
                        {"hits", std::move(hits)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"window",
                 "The game's window on the desktop, for a test that must use the real mouse: `client`, its "
                 "picture's top left corner in desktop pixels and its size, and whether it is minimised or the "
                 "`foreground` window (the one the mouse and keys go to). A place in a screenshot, as a fraction "
                 "of the picture, times the size, plus the corner, is where to click. Answers in any mode."},
        [](const Json &) -> ToolResult {
            const auto place = GameWindowPlace();
            if (!place.found)
            {
                throw ToolError("inspect: the game has no window yet");
            }
            return Json{{"client", {{"x", place.x}, {"y", place.y}, {"width", place.width}, {"height", place.height}}},
                        {"minimised", place.minimised},
                        {"foreground", place.foreground}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"popup",
                 "The game's own message box, if one is up: `popup` with its text and each button's label and "
                 "what pressing it sends; null with none. Answers in any mode, the main menu too. A box holds "
                 "the game until it is answered, and nothing else says it is there: ask when a load or another "
                 "order was taken and nothing followed. game action=press presses a button."},
        [](const Json &) -> ToolResult { return Json{{"popup", PopupJson()}}; });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{
            "dialogue",
            "The conversation on the screen (health's mode is dialog while one is): the `line` spoken last, "
            "with its speaker and text, and the `replies` the player may choose from now, in their order, "
            "each with its `place`, text and icon. No replies while the other speaks. The same comes as "
            "events: eluvian.dialogue.line, .replies and .chosen. `cutscene` and `movie` say whether one is playing. "
            "The dialogue tool answers."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto *conversation = dao::dialogue::Current();
            Json answer = {{"active", conversation != nullptr},
                           {"cutscene", dao::dialogue::InCutscene()},
                           {"movie", dao::dialogue::InMovie()}};
            if (conversation == nullptr)
            {
                answer["note"] = "no conversation is on the screen";
                return answer;
            }
            const bool female = HeroIsFemale();
            const auto current = conversation->CurrentLine();
            const auto line = current ? conversation->LineAt(*current) : std::nullopt;
            answer["line"] = line ? LineJson(*line, female) : Json(nullptr);
            answer["replies"] = RepliesJson(dao::dialogue::Replies(), female);
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"placeables",
                 "The doors, chests, levers and other placeables in the area (or within `radius` of `near`), "
                 "nearest first: id, name, tag, template, position, distance, `state` (the placeable's own "
                 "number: a door's or a chest's changes when it opens), whether it is active and can be "
                 "interacted with, what its tooltip says, and its lock: whether it needs a key, the key's tag, "
                 "and the level to pick it. order action=use uses one."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(around);
            const auto radius = args.Number("radius", -1);
            const bool includeInactive = args.Bool("includeInactive", false);
            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                if (api::GetObjectType(id) != k::OBJECT_TYPE_PLACEABLE)
                {
                    continue;
                }
                const bool active = api::GetObjectActive(id) != 0;
                const auto position = api::GetPosition(id);
                const float distance = Distance(from, position);
                if ((!includeInactive && !active) || (radius >= 0 && distance > radius))
                {
                    continue;
                }
                Json placeable = Brief(id);
                placeable["template"] = Narrow(api::GetResRef(id));
                placeable["position"] = VectorJson(position);
                placeable["distance"] = distance;
                placeable["state"] = api::GetPlaceableState(id);
                placeable["active"] = active;
                placeable["interactive"] = api::GetObjectInteractive(id) != 0;
                placeable["popup"] = Narrow(api::GetPlaceablePopupText(id));
                placeable["lock"] = Json{{"keyRequired", api::GetPlaceableKeyRequired(id) != 0},
                                         {"keyTag", Narrow(api::GetPlaceableKeyTag(id))},
                                         {"pickLevel", api::GetPlaceablePickLockLevel(id)}};
                found.emplace_back(distance, std::move(placeable));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json placeables = Json::array();
            for (auto &[distance, placeable] : found)
            {
                placeables.push_back(std::move(placeable));
            }
            return Json{{"near", Brief(around)}, {"count", placeables.size()}, {"placeables", std::move(placeables)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"creatures",
                 "Every creature in the area (or within `radius` of `near`), nearest first: id, name, tag, side "
                 "(party, hostile, neutral: the hero's view), its `group` (the game's own: 0 the player's, 1 "
                 "hostile, 2 friendly), position, distance, dead, health and mana or stamina, combat state, "
                 "whom it attacks, its `room` (number; `seen` by the party: never, which the game draws "
                 "black with what stands in it, in view, or before; `shown`; and how much the game wants "
                 "the room and whether it is in memory), and the command it is carrying out."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId hero = api::GetHero();
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(around);
            const auto radius = args.Number("radius", -1);
            const auto side =
                args.Has("side") ? std::optional{args.Choice("side", {"party", "hostile", "neutral"})} : std::nullopt;
            const bool includeDead = args.Bool("includeDead", true);
            const bool includeInactive = args.Bool("includeInactive", false);
            const auto party = PartyOf(hero);

            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE ||
                    (!includeInactive && api::GetObjectActive(id) == 0))
                {
                    continue;
                }
                const float distance = Distance(from, api::GetPosition(id));
                if (radius >= 0 && distance > radius)
                {
                    continue;
                }
                Json creature = Summary(id, hero, party);
                if ((side && creature["side"] != *side) || (!includeDead && creature["dead"] == true))
                {
                    continue;
                }
                creature["distance"] = distance;
                found.emplace_back(distance, std::move(creature));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json creatures = Json::array();
            for (auto &[distance, creature] : found)
            {
                creatures.push_back(std::move(creature));
            }
            return Json{{"near", Brief(around)}, {"count", creatures.size()}, {"creatures", std::move(creatures)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"creature",
                 "One creature in full: its summary (with the command it is carrying out), facing, appearance, the "
                 "abilities on its quickbar and in its tactics with each one's cooldown (seconds left, and of how "
                 "many), its "
                 "movement's path (`beingWalked` says whether it is walking it now), and every effect "
                 "on it: its type (EFFECT_TYPE_*), creator, ability (id and name) and time left (seconds; null "
                 "for effects that do not run out)."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
            if (id == kInvalidObject || api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
            {
                throw ToolError(std::format("inspect: {} is not a creature; inspect kind=creatures lists them",
                                            id == kInvalidObject ? "nothing" : FormatId(id)));
            }
            const ObjectId hero = api::GetHero();
            Json creature = Summary(id, hero, PartyOf(hero));
            creature["active"] = api::GetObjectActive(id) != 0;
            creature["facing"] = api::GetFacing(id);
            creature["appearance"] = api::GetAppearanceType(id);
            creature["effects"] = Effects(id);
            creature["abilities"] = Abilities(id);
            creature["path"] = PathNow(id, api::GetPosition(id), false);
            return creature;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"geometry",
                 "Between a creature (`id`, default the one controlled) and a creature (`target`) or a point "
                 "(`point`): the straight distance, the line of sight as the engine judges it (eye height to eye "
                 "height; where it is cut, if it is; only walls and placeables block, not creatures), and the "
                 "creature's movement path with its length and what is left of it; `beingWalked` says "
                 "whether it is walking it now (`waypoints: true` lists its points). `walk: true` also searches the "
                 "path it would walk there, as the game's pathfinder finds it now: `found` with its `length`, or "
                 "why not, and for a shut door or the like in the way, `blocker`: use that first, then move. The "
                 "creature is left as it is."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
            if (id == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `id` was given");
            }
            const bool waypoints = args.Bool("waypoints", false);
            const auto from = api::GetPosition(id);
            Json answer = {{"from", Brief(id)}, {"position", VectorJson(from)}};

            dao::Vector to;
            dao::world::Sight sight;
            if (const auto target = args.OptionalId("target", kCreatureHint))
            {
                to = api::GetPosition(*target);
                answer["target"] = Brief(*target);
                sight = dao::world::LineOfSight(id, *target);
            }
            else if (args.Has("point"))
            {
                to = VectorOf(args.Raw().at("point"), "inspect: `point`");
                const auto eye = [](dao::Vector v) {
                    v.z += dao::world::kEyeHeight;
                    return v;
                };
                sight = dao::world::LineOfSight(eye(from), eye(to), id);
            }
            else
            {
                throw ToolError("inspect: kind=geometry needs a `target` (a creature id) or a `point`");
            }
            answer["to"] = VectorJson(to);
            answer["distance"] = Distance(from, to);
            answer["lineOfSight"] =
                Json{{"clear", sight.clear}, {"blockedAt", sight.blockedAt ? VectorJson(*sight.blockedAt) : Json()}};
            answer["path"] = PathNow(id, from, waypoints);
            // The walk there, when asked for. DAOLib's first FindPath drove the
            // creature's own search and left creatures unable to walk (in play,
            // 2026-09-30); its Pathfind since searches apart from any creature's
            // and was seen in play with the creature walking as before
            // throughout (lib-agent, 2026-10-01, 1fe01a3 on). It names what is
            // in the way of a walk, which a move's COMMAND_FAILED_PATH_ACTION_
            // REQUIRED does not: a test's walk across the Elven Ruins failed
            // ten times on a shut door it could not name. Still the caller's
            // to choose: a search is up to 30 ms of the frame.
            if (args.Bool("walk", false))
            {
                const auto search =
                    dao::world::Pathfind(id, to, args.OptionalId("target", kCreatureHint).value_or(kInvalidObject));
                Json walk = {{"found", search.path.has_value()}};
                const char *said = search.result == 1   ? (search.path ? "found" : "gave up part way")
                                   : search.result == 5 ? "none"
                                   : search.result == 6 ? "something is in the way"
                                   : search.result == 8 ? "no walkable ground there"
                                                        : "not finished in time";
                walk["result"] = said;
                if (search.path)
                {
                    walk["length"] = search.path->Length();
                    walk["waypointCount"] = search.path->waypoints.size();
                    if (waypoints)
                    {
                        Json points = Json::array();
                        for (const auto &point : search.path->waypoints)
                        {
                            points.push_back(VectorJson(point));
                        }
                        walk["waypoints"] = std::move(points);
                    }
                }
                else if (search.partial)
                {
                    walk["reached"] = search.partial->waypoints.empty() ? Json(nullptr)
                                                                        : VectorJson(search.partial->waypoints.back());
                    walk["lengthSoFar"] = search.partial->Length();
                }
                if (search.result == 6)
                {
                    walk["blocker"] = Brief(search.blocker);
                    walk["note"] = search.blocker == kInvalidObject
                                       ? "a door or the like stands in the way and was not found by name; inspect "
                                         "kind=placeables near `reached` lists what is there"
                                       : "a move there ends COMMAND_FAILED_PATH_ACTION_REQUIRED; order action=use "
                                         "on `blocker` first (a shut door opens), then move";
                }
                else if (!search.path && search.blocker != kInvalidObject)
                {
                    // Not the pathfinder's own word: where a search gives up
                    // part way, DAOLib names the placeable lying across the
                    // way just ahead of where it stopped (c8d7270). A strong
                    // hint, not a fact.
                    walk["blocker"] = Brief(search.blocker);
                    walk["note"] = "the search gave up short of something lying across the way: likely `blocker`, "
                                   "a shut door perhaps. order action=use on it, then ask again or move";
                }
                // For DAOLib's own reading of a search that went wrong: each
                // slice's answer and the search's line for a log.
                if (args.Bool("trace", false))
                {
                    if (search.partial)
                    {
                        Json tail = Json::array();
                        const auto &points = search.partial->waypoints;
                        for (std::size_t i = points.size() > 3 ? points.size() - 3 : 0; i < points.size(); ++i)
                        {
                            tail.push_back(VectorJson(points[i]));
                        }
                        walk["partialTail"] = std::move(tail);
                        walk["partialWaypoints"] = points.size();
                    }
                    walk["answers"] = search.answers;
                    walk["slices"] = search.slices;
                    walk["trace"] = search.trace;
                }
                answer["walk"] = std::move(walk);
            }
            return answer;
        });
}

} // namespace eluvian::game
