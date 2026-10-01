#include "game/GameTools.h"

#include "core/Args.h"
#include "game/Convert.h"

#include "dao/Abilities.h"
#include "dao/ClientExoApp.h"
#include "dao/CommandEvents.h"
#include "dao/Events.h"
#include "dao/GameModes.h"
#include "dao/Messages.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Saves.h"
#include "dao/ScriptCall.h"
#include "dao/World.h"
#include "dao/generated/ScriptApi.h"
#include "dao/generated/ScriptConstants.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eluvian::game
{

namespace
{

namespace api = dao::script::api;
namespace k = dao::script::constants;
using dao::kInvalidObject;
using dao::ObjectId;

constexpr std::string_view kCreatureHint = "a creature, as inspect kind=creatures lists them";

// Script commands are not to be called while an area loads: objects come
// and go (lib-agent, 2026-09-30). Before the application is up there is
// nothing to ask.
void RequireWorld(const Watch &watch, std::string_view tool)
{
    const auto mode = watch.Mode();
    if (mode < 0)
    {
        throw ToolError(std::format("{}: the game is still starting; health says when it runs", tool));
    }
    if (mode == static_cast<std::int32_t>(dao::GameMode::Loading))
    {
        throw ToolError(
            std::format("{}: an area is loading; wait_for topic \"eluvian.mode.changed\" and try again", tool));
    }
}

Json Brief(ObjectId id)
{
    if (id == kInvalidObject)
    {
        return nullptr;
    }
    return Json{{"id", IdJson(id)}, {"name", Narrow(api::GetName(id))}, {"tag", Narrow(api::GetTag(id))}};
}

std::vector<ObjectId> Objects(const dao::script::Array &array)
{
    std::vector<ObjectId> ids;
    ids.reserve(array.Size());
    for (std::size_t i = 0; i < array.Size(); ++i)
    {
        ids.push_back(array.Object(i));
    }
    return ids;
}

float Distance(const dao::Vector &a, const dao::Vector &b)
{
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

Json Depletable(ObjectId id, std::int32_t property)
{
    return Json{{"current", api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_CURRENT)},
                {"max", api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_TOTAL)}};
}

// Whose side a creature is on, from the hero's view: the party, hostile, or
// neither.
std::string Side(ObjectId id, ObjectId hero, const std::set<ObjectId> &party)
{
    if (party.contains(id))
    {
        return "party";
    }
    if (hero != kInvalidObject && api::IsObjectHostile(hero, id) != 0)
    {
        return "hostile";
    }
    return "neutral";
}

Json PathJson(const dao::world::WalkPath &path, const dao::Vector &from, bool waypoints)
{
    Json answer = {{"length", path.Length()},
                   {"remaining", path.Remaining(from)},
                   {"waypointCount", path.waypoints.size()},
                   {"next", path.next}};
    if (waypoints)
    {
        Json points = Json::array();
        for (const auto &point : path.waypoints)
        {
            points.push_back(VectorJson(point));
        }
        answer["waypoints"] = std::move(points);
    }
    return answer;
}

// The command a creature is carrying out, if any (DAOLib's CCObject).
const dao::Command *CurrentCommand(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    const auto *object = app != nullptr ? app->GetObjectById(id) : nullptr;
    return object != nullptr ? object->CurrentCommand() : nullptr;
}

// What a creature is doing: its current command's type, whether the player
// gave it, and how many commands are queued.
Json CommandJson(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    const auto *object = app != nullptr ? app->GetObjectById(id) : nullptr;
    if (object == nullptr)
    {
        return nullptr;
    }
    const auto *command = object->CurrentCommand();
    Json current = nullptr;
    if (command != nullptr)
    {
        const auto name = dao::core::Name(command->Type());
        current = Json{{"type", name.empty() ? Json(static_cast<int>(command->Type())) : Json(std::string(name))},
                       {"playerIssued", command->PlayerIssued()}};
    }
    return Json{{"current", std::move(current)}, {"queued", object->CommandQueueSize()}};
}

bool IsMove(dao::core::CommandType type)
{
    using dao::core::CommandType;
    switch (type)
    {
    case CommandType::MoveToLocation:
    case CommandType::MoveToObject:
    case CommandType::MoveToMultipleLocations:
    case CommandType::MoveAwayFromObject:
    case CommandType::MoveAwayFromPoint:
    case CommandType::RandomWalk:
    case CommandType::ForceFollowObject:
        return true;
    default:
        return false;
    }
}

// The path the creature's movement holds, and whether it is walking it now.
// The movement keeps its last path after the walk: in play, 2026-09-30, a
// hero standing still since the save loaded held one 2.45 m long. So the
// path alone says nothing; the current command says whether it is walked.
Json PathNow(ObjectId id, const dao::Vector &from, bool waypoints)
{
    const auto path = dao::world::CurrentPath(id);
    if (!path)
    {
        return nullptr;
    }
    Json answer = PathJson(*path, from, waypoints);
    const auto *command = CurrentCommand(id);
    answer["beingWalked"] = command != nullptr && IsMove(command->Type());
    return answer;
}

Json Summary(ObjectId id, ObjectId hero, const std::set<ObjectId> &party)
{
    Json creature = Brief(id);
    creature["side"] = Side(id, hero, party);
    creature["position"] = VectorJson(api::GetPosition(id));
    creature["dead"] = api::IsDead(id) != 0;
    creature["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
    creature["manaStamina"] = Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA);
    creature["combatState"] = api::GetCombatState(id);
    creature["command"] = CommandJson(id);
    return creature;
}

// A script constant by its name ("EFFECT_TYPE_ROOT"); its number when
// nothing names it, as a script's own numbers are not.
template <class E> Json Named(E value)
{
    const auto name = dao::script::Name(value);
    return name.empty() ? Json(static_cast<std::int32_t>(value)) : Json(std::string(name));
}

// An effect's duration type by name. script.ldf names 0 to 3 and 6; 4 is
// an equipped item's effects, as seen in play on Aedan's rings and belt
// (2026-09-30; DAOLib 80d5f01 notes it at CGameEffect::DurationType).
Json DurationJson(dao::script::EffectDurationType type)
{
    if (static_cast<std::int32_t>(type) == 4)
    {
        return "4 (equipped item)";
    }
    return Named(type);
}

// An ability by id and its name in the player's language ("Fireball");
// null for none. The names come from the data tables, loaded by the main
// menu (dao/Abilities.h).
Json AbilityJson(std::int32_t ability)
{
    if (ability <= 0)
    {
        return nullptr;
    }
    Json answer = {{"id", ability}};
    if (const auto name = dao::abilities::Name(ability))
    {
        answer["name"] = Narrow(*name);
    }
    return answer;
}

Json Effects(ObjectId id)
{
    Json effects = Json::array();
    const auto array = api::GetEffects(id);
    for (std::size_t i = 0; i < array.Size(); ++i)
    {
        const auto *effect = array.Effect(i);
        if (effect == nullptr)
        {
            continue;
        }
        const bool timed = effect->DurationMs() > 0;
        effects.push_back(Json{
            {"effectId", effect->Id()},
            {"type", Named(effect->Type())},
            {"durationType", DurationJson(effect->DurationType())},
            {"creator", Brief(effect->Creator())},
            {"ability", AbilityJson(effect->AbilityId())},
            {"duration", timed ? Json(effect->Duration()) : Json(nullptr)},
            {"remaining", timed ? Json(effect->Remaining()) : Json(nullptr)},
        });
    }
    return effects;
}

std::set<ObjectId> PartyOf(ObjectId hero)
{
    if (hero == kInvalidObject)
    {
        return {};
    }
    const auto ids = Objects(api::GetPartyList(hero));
    return {ids.begin(), ids.end()};
}

// ---- inspect ---------------------------------------------------------------

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
               {"waypoints",
                {{"type", "boolean"}, {"description", "kind=geometry: list the points of the path being walked"}}},
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
                 "selected in the party bar, and the "
                 "event cursor."},
        [&bench, &watch](const Json &) -> ToolResult {
            Json answer = {{"mode", ModeName(watch.Mode())}, {"cursor", bench.Events().Next()}};
            const auto *app = dao::CClientExoApp::Get();
            answer["paused"] = app != nullptr && app->IsPaused();
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
        KindSpec{"creatures",
                 "Every creature in the area (or within `radius` of `near`), nearest first: id, name, tag, side "
                 "(party, hostile, neutral), position, distance, dead, health and mana or stamina, combat state, "
                 "and the command it is carrying out."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId hero = api::GetHero();
            const ObjectId near = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (near == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(near);
            const auto radius = args.Number("radius", -1);
            const auto side =
                args.Has("side") ? std::optional{args.Choice("side", {"party", "hostile", "neutral"})} : std::nullopt;
            const bool includeDead = args.Bool("includeDead", true);
            const bool includeInactive = args.Bool("includeInactive", false);
            const auto party = PartyOf(hero);

            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(near))))
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
            return Json{{"near", Brief(near)}, {"count", creatures.size()}, {"creatures", std::move(creatures)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"creature",
                 "One creature in full: its summary (with the command it is carrying out), facing, appearance, its "
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
                 "whether it is walking it now (`waypoints: true` lists its points). Not the path it would walk: that "
                 "search is the "
                 "creature's own, and asking it changed the creature's walking in play (2026-09-30)."},
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
            // DAOLib's FindPath is left out: in the play test of 2026-09-30 it
            // answered late, left its result as the creature's own path, and
            // may have stopped a creature walking (lib-agent). Reading the
            // path a creature walks is only a read.
            answer["path"] = PathNow(id, from, waypoints);
            return answer;
        });
}

// ---- script ----------------------------------------------------------------

Json Describe(const dao::script::CommandInfo &command)
{
    Json params = Json::array();
    for (const auto &param : command.params)
    {
        Json p = {{"name", param.name}, {"type", dao::core::ScriptTypeName(param.type)}};
        if (const auto value = param.Default())
        {
            p["default"] = JsonOf(*value);
        }
        else if (param.defaultKind == dao::core::DefaultKind::Self)
        {
            p["default"] = "OBJECT_SELF";
        }
        params.push_back(std::move(p));
    }
    return Json{{"name", command.name},           {"id", command.id},
                {"signature", command.signature}, {"returns", dao::core::ScriptTypeName(command.returns)},
                {"params", std::move(params)},    {"callableByName", command.CallableByName()}};
}

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

void AddScript(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "script",
            "Call any of the game's 661 script commands by name and get its answer: the general tool when no "
            "other fits (GetCreatureProperty, GetRemainingCooldown, SetCreatureProperty, "
            "Screenshot). `action`: call (default), find (commands whose name contains "
            "`command`), describe (one command's parameters and defaults). Arguments are positional "
            "(`args`: [...]) or by parameter name (`args`: {\"oCreature\": \"0x7d9\"}); left off, they take "
            "their defaults. Objects are ids; null is OBJECT_INVALID. Commands taking or returning a "
            "location, effect, event or command cannot be called by name.",
            Json{{"type", "object"},
                 {"properties",
                  {{"action", {{"type", "string"}, {"enum", {"call", "find", "describe"}}}},
                   {"command", {{"type", "string"}, {"description", "The command's name, as script.ldf writes it"}}},
                   {"args", {{"description", "An array in parameter order, or an object by parameter name"}}}}},
                 {"required", {"command"}}},
            Thread::Frame},
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "script");
            const auto action = args.Choice("action", {"call", "find", "describe"}, "call");
            const auto name = args.String("command");
            if (action == "find")
            {
                const auto needle = Lower(name);
                Json found = Json::array();
                for (const auto &command : dao::script::table::kCommands)
                {
                    if (Lower(command.name).find(needle) != std::string::npos)
                    {
                        found.push_back(Json{{"name", command.name},
                                             {"signature", command.signature},
                                             {"callableByName", command.CallableByName()}});
                    }
                }
                return Json{{"count", found.size()}, {"commands", std::move(found)}};
            }
            const auto *command = dao::script::table::Find(name);
            if (command == nullptr)
            {
                throw ToolError(
                    std::format("script: no command \"{}\"; action \"find\" searches by part of the name", name));
            }
            if (action == "describe")
            {
                return Describe(*command);
            }
            if (!command->CallableByName())
            {
                throw ToolError(std::format("script: {} cannot be called by name ({}): a location, effect, event or "
                                            "command moves only through the typed wrappers",
                                            command->name, command->signature));
            }
            RequireWorld(watch, "script");

            std::vector<dao::script::Value> values;
            const Json given = args.Has("args") ? args.Raw().at("args") : Json::array();
            if (given.is_array())
            {
                if (given.size() > command->params.size())
                {
                    throw ToolError(std::format("script: {} takes {} arguments: {}", command->name,
                                                command->params.size(), command->signature));
                }
                for (std::size_t i = 0; i < given.size(); ++i)
                {
                    values.push_back(ValueOf(given[i], command->params[i].type, command->params[i].name));
                }
            }
            else if (given.is_object())
            {
                // By name: up to the last one given; those before it left off take their defaults.
                std::size_t last = 0;
                for (const auto &[key, value] : given.items())
                {
                    const auto it = std::ranges::find(command->params, key, &dao::core::ScriptParam::name);
                    if (it == command->params.end())
                    {
                        throw ToolError(std::format("script: {} has no parameter `{}`: {}", command->name, key,
                                                    command->signature));
                    }
                    last = std::max(last, static_cast<std::size_t>(it - command->params.begin()) + 1);
                }
                for (std::size_t i = 0; i < last; ++i)
                {
                    const auto &param = command->params[i];
                    const auto it = given.find(param.name);
                    if (it != given.end())
                    {
                        values.push_back(ValueOf(*it, param.type, param.name));
                    }
                    else if (const auto fallback = param.Default())
                    {
                        values.push_back(*fallback);
                    }
                    else if (param.defaultKind == dao::core::DefaultKind::Self)
                    {
                        values.push_back(dao::script::Value::Self());
                    }
                    else
                    {
                        throw ToolError(std::format("script: `{}` has no default and must be given: {}", param.name,
                                                    command->signature));
                    }
                }
            }
            else
            {
                throw ToolError("script: `args` is an array in parameter order, or an object by parameter name");
            }
            const auto result = dao::script::Call(*command, values);
            if (!result)
            {
                throw ToolError(std::format("script: {}: {}", command->name, result.error));
            }
            return Json{{"command", command->name}, {"value", JsonOf(result.value)}};
        });
}

// ---- order -----------------------------------------------------------------

std::int32_t AbilityArg(const Args &args)
{
    const auto ability =
        static_cast<std::int32_t>(args.Integer("ability", -1, 0, std::numeric_limits<std::int32_t>::max()));
    if (ability < 0)
    {
        throw ToolError("order: `ability` is the ability's id (the ABI_* row in the ability table)");
    }
    return ability;
}

// Through the engine's own click functions (dao/Orders.h): the command
// player-issued, queued as a click queues it. Any creature may obey.
bool OrderByCommand(const Args &args, const std::string &action, ObjectId who, Json &answer)
{
    if (action == "attack")
    {
        const ObjectId target = args.Id("target", kCreatureHint);
        answer["target"] = Brief(target);
        return dao::orders::Attack(who, target);
    }
    if (action == "move")
    {
        const auto point = VectorOf(args.Raw().value("point", Json()), "order: `point`");
        answer["point"] = VectorJson(point);
        return dao::orders::MoveTo(who, point);
    }
    if (action == "ability")
    {
        const auto ability = AbilityArg(args);
        const ObjectId target = args.OptionalId("target", kCreatureHint).value_or(kInvalidObject);
        const auto point = args.Has("point") ? VectorOf(args.Raw().at("point"), "order: `point`") : dao::Vector{};
        const ObjectId item = args.OptionalId("item", "an item").value_or(kInvalidObject);
        answer["ability"] = AbilityJson(ability);
        answer["target"] = Brief(target);
        return dao::orders::UseAbility(who, ability, target, item, point);
    }
    const ObjectId target = args.Id("target", "a placeable or other object to use");
    answer["target"] = Brief(target);
    return dao::orders::UseObject(who, target);
}

// Through explore mode's own click messages, so the click's front half runs
// too: what turnbased's order watch and talent targeting hook (DAOLib
// 9ea5e2c). A click is the controlled creature's. The object under the
// cursor is set and the message sent in the same frame, before the mode
// picks again from the real cursor.
bool OrderByClick(const Args &args, const std::string &action, ObjectId who, Json &answer)
{
    auto *explore = dao::CClientExoApp::CurrentExplore();
    if (explore == nullptr)
    {
        throw ToolError("order: a click needs explore or combat mode; inspect kind=game says the mode");
    }
    if (action == "move")
    {
        throw ToolError("order: a move cannot be clicked yet (the cursor's ground hit has no safe setter); via "
                        "\"command\" moves as the click's own handler does");
    }
    if (action == "ability")
    {
        const auto ability = AbilityArg(args);
        const auto target = args.OptionalId("target", kCreatureHint);
        answer["ability"] = AbilityJson(ability);
        answer["target"] = target ? Brief(*target) : Json(nullptr);
        if (target)
        {
            explore->SelectTarget(*target); // as the cycle-target keys select it
        }
        auto message = dao::messages::Make<dao::messages::PerformAbilityMessage>();
        message.ability = ability;
        message.caster = who;
        message.target = target.value_or(kInvalidObject);
        return dao::messages::Send(explore, message);
    }
    // attack and use: the click's primary action on the object under the
    // cursor (attack an enemy, use a placeable, talk to a friend). Its
    // handler (0x007132B0) is a void function, so Send's answer is a leftover
    // register and says nothing; and it orders an attack from every party
    // member selected in the interface, not the controlled creature alone
    // (lib-agent, 2026-09-30, after a click in combat queued nothing). What
    // came of it is in the events: eluvian.command.started, and the mod's.
    const ObjectId target = args.Id("target", "the creature or object to click on");
    answer["target"] = Brief(target);
    explore->SetCursorObject(target);
    dao::messages::Send(explore, dao::messages::Make<dao::messages::PerformPrimaryActionOnTargetUnderCursorMessage>());
    Json selected = Json::array();
    for (const ObjectId member : dao::party::Selected())
    {
        selected.push_back(Brief(member));
    }
    answer["selected"] = std::move(selected);
    answer["note"] = "a click's primary action orders the party members selected (`selected`, the party bar's; the "
                     "engine's own list may differ); eluvian.command.started from `cursor` says who obeyed";
    return true;
}

void AddOrder(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"order",
                 "Give an order as the player does, by id. `action`: attack `target`; move to `point`; ability "
                 "`ability` (an id from the ability table) on `target` or `point`; use `target` (a placeable). "
                 "`via`: \"command\" (the default) gives it through the engine's own click functions, so the "
                 "command is player-issued and anything watching the command queue sees one; any creature "
                 "`id` may obey. \"click\" sends explore mode's click message instead (the primary action on the "
                 "target as if the cursor were over it; an ability as the quickbar uses it, with `target` "
                 "selected), so the click's own handling runs too, and whatever hooks it (a mod watching clicks, "
                 "asking for an ability's target); "
                 "it is the controlled creature's, and cannot move. (Seen 2026-09-30: an ability by click works, the "
                 "mod asking for its target when none is given. An attack or use by click is the primary action "
                 "of every party member selected in the interface, and its answer cannot say whether anyone "
                 "obeyed: the events from `cursor` do.) Answers whether the order was taken, and "
                 "`cursor`: wait_for from it for what came of it (eluvian.command.started and ended, and a mod's "
                 "own events).",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"attack", "move", "ability", "use"}}}},
                        {"id", {{"description", "Who obeys: a creature id; default the one controlled"}}},
                        {"target", {{"description", "A creature or object id"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
                        {"ability", {{"type", "integer"}}},
                        {"item", {{"description", "For an ability from an item: the item's id"}}},
                        {"via",
                         {{"type", "string"},
                          {"enum", {"command", "click"}},
                          {"description", "command (default): the engine's click functions. click: explore "
                                          "mode's click message"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "order");
            const auto action = args.Choice("action", {"attack", "move", "ability", "use"});
            const auto via = args.Choice("via", {"command", "click"}, "command");
            RequireWorld(watch, "order");
            const ObjectId controlled = api::GetMainControlled();
            const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(controlled);
            if (who == kInvalidObject)
            {
                throw ToolError("order: nobody is controlled and no `id` was given");
            }
            if (via == "click" && who != controlled)
            {
                throw ToolError(std::format("order: a click is the controlled creature's ({}); via \"command\" "
                                            "orders any creature",
                                            controlled == kInvalidObject ? "nobody" : FormatId(controlled)));
            }
            const auto cursor = bench.Events().Next();
            Json answer = {{"action", action}, {"via", via}, {"creature", Brief(who)}};
            const bool taken =
                via == "click" ? OrderByClick(args, action, who, answer) : OrderByCommand(args, action, who, answer);
            if (!taken)
            {
                throw ToolError(via == "click"
                                    ? "order: explore mode did not take the ability's click"
                                    : "order: the game took no order: the creature is not there, or (for move) "
                                      "the game is not in explore or combat mode");
            }
            answer["given"] = true;
            answer["cursor"] = cursor;
            return answer;
        });
}

// ---- stage -----------------------------------------------------------------

// The conditions a test puts on a creature by name; any other EFFECT_TYPE_*
// by its number.
struct Condition
{
    std::string_view name;
    std::int32_t type;
};
constexpr Condition kConditions[] = {
    {"stun", k::EFFECT_TYPE_STUN},           {"knockdown", k::EFFECT_TYPE_KNOCKDOWN},
    {"paralyze", k::EFFECT_TYPE_PARALYZE},   {"root", k::EFFECT_TYPE_ROOT},
    {"sleep", k::EFFECT_TYPE_SLEEP},         {"daze", k::EFFECT_TYPE_DAZE},
    {"slip", k::EFFECT_TYPE_SLIP},           {"fear", k::EFFECT_TYPE_FEAR},
    {"confusion", k::EFFECT_TYPE_CONFUSION}, {"grabbed", k::EFFECT_TYPE_GRABBED},
};

std::optional<std::int32_t> EffectTypeArg(const Args &args)
{
    if (!args.Has("effect"))
    {
        return std::nullopt;
    }
    const Json &effect = args.Raw().at("effect");
    if (effect.is_number_integer())
    {
        return effect.get<std::int32_t>();
    }
    if (effect.is_string())
    {
        const auto &name = effect.get_ref<const std::string &>();
        for (const auto &condition : kConditions)
        {
            if (name == condition.name)
            {
                return condition.type;
            }
        }
    }
    std::string names;
    for (const auto &condition : kConditions)
    {
        names += names.empty() ? "" : ", ";
        names += condition.name;
    }
    throw ToolError("stage: `effect` is one of " + names + ", or an EFFECT_TYPE_* number");
}

ObjectId CreatureArg(const Args &args)
{
    const ObjectId id = args.Id("id", kCreatureHint);
    if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("stage: {} is not a creature; inspect kind=creatures lists them", FormatId(id)));
    }
    return id;
}

void AddStage(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "stage",
            "Set up a test's case, where a fight rarely brings it: `action` effect puts a condition on "
            "creature `id` (stun, knockdown, paralyze, root, sleep, daze, slip, fear, confusion, grabbed, or "
            "an EFFECT_TYPE_* number) for `seconds`, as from `creator` and `ability` if given; "
            "remove_effects takes them off (by `effect`, `ability`, `creator`, or all); health sets its "
            "current health to `value`; kill deals it more damage than it has; spawn makes a creature from "
            "`template` (a creature resource, as its tag reads, e.g. \"bhn200cr_howe_soldier\") at `point` "
            "facing `facing` degrees, in the controlled creature's area. Answers the creature after: its "
            "effects, health or id. The effects are the engine's bare ones, applied as scripts apply them; "
            "a death or a fall comes a frame or more later, so wait_for eluvian.creature.died.",
            Json{{"type", "object"},
                 {"properties",
                  {{"action", {{"type", "string"}, {"enum", {"effect", "remove_effects", "health", "kill", "spawn"}}}},
                   {"id", {{"description", "The creature"}}},
                   {"effect", {{"description", "A condition's name or an EFFECT_TYPE_* number"}}},
                   {"seconds", {{"type", "number"}, {"description", "action=effect: how long; default 6"}}},
                   {"creator", {{"description", "Whom the effect is from; default none"}}},
                   {"ability", {{"type", "integer"}, {"description", "The ability it is from; default none"}}},
                   {"value", {{"type", "number"}, {"description", "action=health: the current health"}}},
                   {"template", {{"type", "string"}, {"description", "action=spawn: the creature's resource"}}},
                   {"point", {{"description", "action=spawn: {x, y, z} or [x, y, z]"}}},
                   {"facing", {{"type", "number"}, {"description", "action=spawn: degrees; default 0"}}}}},
                 {"required", {"action"}}},
            Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "stage");
            const auto action = args.Choice("action", {"effect", "remove_effects", "health", "kill", "spawn"});
            RequireWorld(watch, "stage");
            Json answer = {{"action", action}, {"cursor", bench.Events().Next()}};
            if (action == "spawn")
            {
                const auto name = args.String("template");
                const auto point = VectorOf(args.Raw().value("point", Json()), "stage: `point`");
                const ObjectId near = api::GetMainControlled();
                if (near == kInvalidObject)
                {
                    throw ToolError("stage: nobody is controlled, so there is no area to spawn in");
                }
                const auto facing = static_cast<float>(args.Number("facing", 0));
                const ObjectId id = api::CreateObject(k::OBJECT_TYPE_CREATURE, dao::Resource{Wide(name)},
                                                      api::Location(api::GetArea(near), point, facing));
                if (id == kInvalidObject)
                {
                    throw ToolError(std::format("stage: no creature from template \"{}\"; a creature's tag usually "
                                                "names its template",
                                                name));
                }
                answer["creature"] = Summary(id, api::GetHero(), PartyOf(api::GetHero()));
                return answer;
            }
            const ObjectId id = CreatureArg(args);
            answer["creature"] = Brief(id);
            if (action == "effect")
            {
                const auto type = EffectTypeArg(args);
                if (!type)
                {
                    throw ToolError("stage: action effect needs `effect`, a condition's name or a number");
                }
                const double seconds = args.Number("seconds", 6);
                if (seconds <= 0 || seconds > 600)
                {
                    throw ToolError("stage: `seconds` must be more than 0 and at most 600");
                }
                const ObjectId creator = args.OptionalId("creator", kCreatureHint).value_or(kInvalidObject);
                const auto ability = static_cast<std::int32_t>(args.Integer("ability", 0, 0, 1 << 30));
                const auto effect = api::Effect(*type);
                if (effect.get() == nullptr)
                {
                    throw ToolError("stage: the engine made no effect of that type");
                }
                api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_TEMPORARY, *effect.get(), id,
                                                static_cast<float>(seconds), creator, ability);
                answer["applied"] = Json{{"type", Named(static_cast<dao::script::EffectType>(*type))},
                                         {"seconds", seconds},
                                         {"creator", Brief(creator)},
                                         {"ability", AbilityJson(ability)}};
                answer["effects"] = Effects(id);
            }
            else if (action == "remove_effects")
            {
                const auto type = EffectTypeArg(args).value_or(k::EFFECT_TYPE_INVALID);
                const auto ability =
                    static_cast<std::int32_t>(args.Integer("ability", k::ABILITY_INVALID, -1, 1 << 30));
                const ObjectId creator = args.OptionalId("creator", kCreatureHint).value_or(kInvalidObject);
                api::RemoveEffectsByParameters(id, type, ability, creator);
                answer["effects"] = Effects(id);
            }
            else if (action == "health")
            {
                const double value = args.Number("value");
                if (value < 0)
                {
                    throw ToolError("stage: `value` is the health to set, 0 or more");
                }
                api::SetCreatureProperty(id, k::PROPERTY_DEPLETABLE_HEALTH, static_cast<float>(value),
                                         k::PROPERTY_VALUE_CURRENT);
                answer["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
            }
            else
            {
                const float health =
                    api::GetCreatureProperty(id, k::PROPERTY_DEPLETABLE_HEALTH, k::PROPERTY_VALUE_CURRENT);
                const auto damage = api::EffectDamage(health + 1000);
                if (damage.get() == nullptr)
                {
                    throw ToolError("stage: the engine made no damage effect");
                }
                api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_INSTANT, *damage.get(), id, 0.0F,
                                                kInvalidObject, 0);
                answer["dealt"] = health + 1000;
                answer["note"] = "the death comes a frame or more later: wait_for eluvian.creature.died from cursor";
            }
            return answer;
        });
}

// ---- game ------------------------------------------------------------------

void AddGame(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        // No game speed here. Each plugin's copy of DAOLib hooks the frame's
        // timer on its first SetSpeed, so two plugins setting a speed
        // multiply, and each reads back only its own (DAOLib ed55ab8,
        // 2026-09-30). One plugin owns the speed: in a turn-based test the
        // mod, through turnbased_settings' ai_turn_speed.
        ToolSpec{"game",
                 "Pause or unpause the game. Answers the pause after. (Not the game's speed: each plugin's copy "
                 "of DAOLib scales the clock by its own, so two setting it multiply; a mod that sets speed owns "
                 "it, through its own tools.)",
                 Json{{"type", "object"},
                      {"properties", {{"action", {{"type", "string"}, {"enum", {"pause", "unpause"}}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "game");
            const auto action = args.Choice("action", {"pause", "unpause"});
            auto *app = dao::CClientExoApp::Get();
            if (app == nullptr || watch.Mode() < 0)
            {
                throw ToolError("game: the game is still starting; health says when it runs");
            }
            app->SetPaused(action == "pause");
            return Json{{"paused", app->IsPaused()}};
        });
}

// ---- engine events ---------------------------------------------------------

struct ScriptEventTopic
{
    dao::script::EventType type;
    std::string_view name;
};

// The script events a test waits on, by the name of their topic,
// eluvian.script.<name>. Not heartbeats and perception, which come by the
// dozen a second and say little.
constexpr ScriptEventTopic kScriptEvents[] = {
    {dao::script::EventType::Damaged, "damaged"},
    {dao::script::EventType::Death, "death"},
    {dao::script::EventType::AttackImpact, "attack_impact"},
    {dao::script::EventType::CombatInitiated, "combat_initiated"},
    {dao::script::EventType::AbilityCastStart, "ability_cast_start"},
    {dao::script::EventType::AbilityCastImpact, "ability_cast_impact"},
    {dao::script::EventType::ApplyEffect, "apply_effect"},
    {dao::script::EventType::RemoveEffect, "remove_effect"},
    {dao::script::EventType::CommandComplete, "command_complete"},
    {dao::script::EventType::GamemodeChange, "gamemode_change"},
    {dao::script::EventType::ArealoadPostloadexit, "area_loaded"},
    {dao::script::EventType::PartymemberAdded, "partymember_added"},
    {dao::script::EventType::OnOrderReceived, "order_received"},
};

} // namespace

void AddGameTools(Bench &bench, const Watch &watch)
{
    AddInspect(bench, watch);
    AddScript(bench, watch);
    AddOrder(bench, watch);
    AddStage(bench, watch);
    AddGame(bench, watch);
}

namespace
{

// A creature's, not a placeable's or a trigger's: their commands are the
// world's own business and would fill the buffer.
bool IsCreature(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    auto *object = app != nullptr ? app->GetGameObjectById(id) : nullptr;
    return object != nullptr && object->AsCreature() != nullptr;
}

// A script event as the bus carries it. `receiver` is the object the event
// is delivered to, which is not always the one it is about: an attack's
// impact goes to the attacker (seen in play, 2026-09-30, Aedan's impact
// delivered to Aedan). So the event's own parameters come too, as the
// scripts read them with GetEventObject, GetEventInteger and GetEventFloat:
// the first four of each, without the defaults that trail (-1, 0, 0.0),
// which is what reading past the end gives.
Json ScriptEventPayload(ObjectId receiver, const dao::CScriptEvent &event)
{
    constexpr std::int32_t kRead = 4;
    Json objects = Json::array();
    Json integers = Json::array();
    Json floats = Json::array();
    for (std::int32_t i = 0; i < kRead; ++i)
    {
        objects.push_back(IdJson(event.Object(i)));
        integers.push_back(event.Integer(i));
        floats.push_back(event.Float(i));
    }
    const auto trim = [](Json &list, const Json &fallback) {
        while (!list.empty() && list.back() == fallback)
        {
            list.erase(list.end() - 1);
        }
    };
    trim(objects, Json(nullptr));
    trim(integers, Json(0));
    trim(floats, Json(0.0F));
    Json payload = {{"receiver", IdJson(receiver)}, {"creator", IdJson(event.Creator())}};
    // The parameters' meanings, from the game's own rules_core.nss
    // (lib-agent, 2026-09-30).
    if (event.Type() == dao::script::EventType::AttackImpact)
    {
        payload["attacker"] = IdJson(event.Object(0));
        payload["target"] = IdJson(event.Object(1));
        payload["hit"] = Named(static_cast<dao::script::CombatResult>(event.Integer(0)));
        payload["effectId"] = event.Integer(1);
    }
    else if (event.Type() == dao::script::EventType::Damaged)
    {
        payload["damager"] = IdJson(event.Creator());
        payload["amount"] = event.Float(0);
        payload["damageType"] = Named(static_cast<dao::script::DamageType>(event.Integer(0)));
        payload["ability"] = AbilityJson(event.Integer(1));
    }
    if (!objects.empty())
    {
        payload["objects"] = std::move(objects);
    }
    if (!integers.empty())
    {
        payload["integers"] = std::move(integers);
    }
    if (!floats.empty())
    {
        payload["floats"] = std::move(floats);
    }
    return payload;
}

Json CommandPayload(ObjectId id, const dao::Command &command)
{
    const auto name = dao::core::Name(command.Type());
    return Json{{"id", IdJson(id)},
                {"type", name.empty() ? Json(static_cast<int>(command.Type())) : Json(std::string(name))},
                {"playerIssued", command.PlayerIssued()}};
}

} // namespace

void AddEngineEvents(Bench &bench)
{
    // Each command a creature starts and ends, and how it ended: what a test
    // waits on after an order ("did the attack happen"), which the scripts'
    // COMMAND_COMPLETE reaches only sometimes (DAOLib 38eea39). On the game's
    // thread inside the command step: emit and return.
    dao::commands::OnStart([&bench](ObjectId id, const dao::Command &command) {
        if (IsCreature(id))
        {
            bench.Events().Emit("eluvian.command.started", CommandPayload(id, command));
        }
    });
    dao::commands::OnEnd([&bench](ObjectId id, const dao::Command &command, std::int32_t result) {
        if (IsCreature(id))
        {
            Json payload = CommandPayload(id, command);
            const auto name = dao::commands::ResultName(result);
            payload["result"] = name.empty() ? Json(result) : Json(std::string(name));
            payload["success"] = result == dao::commands::kSuccess;
            bench.Events().Emit("eluvian.command.ended", std::move(payload));
        }
    });

    dao::saves::OnGameLoaded([&bench](const std::optional<dao::saves::Slot> &slot) {
        Json payload = {{"newGame", !slot.has_value()}};
        if (slot)
        {
            payload["character"] = Narrow(slot->character);
            payload["kind"] = dao::saves::Name(slot->kind);
            payload["number"] = slot->number;
        }
        bench.Events().Emit("eluvian.game.loaded", std::move(payload));
    });
    dao::saves::OnSaved([&bench](const dao::saves::Slot &slot) {
        bench.Events().Emit("eluvian.game.saved", Json{{"character", Narrow(slot.character)},
                                                       {"kind", dao::saves::Name(slot.kind)},
                                                       {"number", slot.number}});
    });
    for (const auto &[type, name] : kScriptEvents)
    {
        dao::events::OnScriptEvent(
            type, [&bench, topic = std::format("eluvian.script.{}", name),
                   death = type == dao::script::EventType::Death](ObjectId target, const dao::CScriptEvent &event) {
                bench.Events().Emit(topic, ScriptEventPayload(target, event));
                if (death)
                {
                    bench.Events().Emit("eluvian.creature.died", Json{{"id", IdJson(target)},
                                                                      {"name", Narrow(api::GetName(target))},
                                                                      {"killer", IdJson(event.Creator())}});
                }
            });
    }
}

} // namespace eluvian::game
