#include "game/GameTools.h"

#include "core/Args.h"
#include "core/Base64.h"
#include "game/Capture.h"
#include "game/Convert.h"

#include "dao/Abilities.h"
#include "dao/CameraMoves.h"
#include "dao/ClientExoApp.h"
#include "dao/CommandEvents.h"
#include "dao/Dialogue.h"
#include "dao/Events.h"
#include "dao/Frame.h"
#include "dao/GameModes.h"
#include "dao/Gui.h"
#include "dao/Log.h"
#include "dao/Messages.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Saves.h"
#include "dao/ScriptCall.h"
#include "dao/Tlk.h"
#include "dao/World.h"
#include "dao/generated/ScriptApi.h"
#include "dao/generated/ScriptConstants.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
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

// The game's own message box, if one is up: its text, and each button with
// what pressing it sends. A box holds the game until it is answered and no
// event tells of it: a load asked for a save that needed add-ins no longer
// registered stopped at one, "Force Load / Cancel", and the call had
// answered `asked` (2026-10-01). DAOLib's dao::gui::CurrentPopup (788079b).
Json PopupJson()
{
    const auto popup = dao::gui::CurrentPopup();
    if (!popup)
    {
        return nullptr;
    }
    Json buttons = Json::array();
    for (const auto &button : popup->buttons)
    {
        buttons.push_back(Json{{"label", Narrow(button.label)}, {"sends", Narrow(button.callback)}});
    }
    return Json{{"text", Narrow(popup->text)}, {"buttons", std::move(buttons)}};
}

// A conversation's line as the screen has it: its place in the file, who
// speaks as the file writes it (a tag, or OWNER or PLAYER), and its text in
// the player's language, in the form for the hero's sex. A line may have no
// text: an NPC line that only routes, a "continue" reply (DAOLib's
// Dialogue.h, seen at Highever, 2026-09-30). The talk table's markup
// (<emp>) is left in.
Json LineJson(const dao::dialogue::Line &line, bool female)
{
    Json json = {{"index", line.index},
                 {"speaker", Narrow(line.speaker)},
                 {"text", line.text == 0xFFFFFFFF ? std::string() : Narrow(dao::tlk::String(line.text, female))}};
    if (line.kind == dao::dialogue::Line::Kind::Player)
    {
        json["icon"] = line.icon;
    }
    return json;
}

bool HeroIsFemale()
{
    return api::GetCreatureGender(api::GetHero()) == k::GENDER_FEMALE;
}

// The replies on the screen, each with its `place` among them: the number
// a choice names.
Json RepliesJson(std::span<const dao::dialogue::Line> replies, bool female)
{
    Json list = Json::array();
    for (std::size_t place = 0; place < replies.size(); ++place)
    {
        Json reply = LineJson(replies[place], female);
        reply["place"] = place;
        list.push_back(std::move(reply));
    }
    return list;
}

// The room a creature stands in and whether the party has seen it: the
// game draws a room never seen black, with what stands in it (DAOLib's
// dao::world::Room, seen in Castle Cousland, 2026-10-01). Null where the
// area has no rooms to ask. Outdoors the "rooms" are the terrain's cells:
// in the Forest Clearing their numbers read 262147, 196611 and the like,
// `seen` in view everywhere, and none ever loaded, the party's own included
// (the same day), so `shown` goes by `seen` alone (DAOLib 66d5567). `wanted`
// and `load` are the game's own two states, by name where DAOLib names
// them and by number where it does not.
Json RoomJson(ObjectId id)
{
    using dao::world::Room;
    const auto room = dao::world::RoomOf(id);
    if (!room)
    {
        return nullptr;
    }
    const char *seen = room->seen == Room::kNeverSeen ? "never" : room->seen == Room::kInView ? "in view" : "before";
    const Json wanted = room->state == Room::kDropped     ? Json("dropped")
                        : room->state == Room::kWanted    ? Json("kept")
                        : room->state == Room::kWantedNow ? Json("now")
                                                          : Json(room->state);
    const Json load = room->load == Room::kLoading     ? Json("loading")
                      : room->load == Room::kLoaded    ? Json("loaded")
                      : room->load == Room::kNotLoaded ? Json("not loaded")
                                                       : Json(room->load);
    return Json{{"number", room->number}, {"seen", seen}, {"shown", room->Shown()}, {"wanted", wanted}, {"load", load}};
}

Json Summary(ObjectId id, ObjectId hero, const std::set<ObjectId> &party)
{
    Json creature = Brief(id);
    creature["side"] = Side(id, hero, party);
    creature["position"] = VectorJson(api::GetPosition(id));
    creature["dead"] = api::IsDead(id) != 0;
    creature["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
    creature["manaStamina"] = Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA);
    creature["template"] = Narrow(api::GetResRef(id)); // what stage spawn takes; the tag is the instance's
    // `side` is the hero's view and has three values; the group is the
    // game's own, and says more: a creature of group 2 fighting the hostile
    // ones read "neutral" (computer-use-agent, 2026-10-01). 0 is the
    // player's, 1 the hostile one, 2 friendly, as the game sets them.
    creature["group"] = api::GetGroupId(id);
    creature["combatState"] = api::GetCombatState(id);
    creature["attackTarget"] = Brief(api::GetAttackTarget(id));
    creature["room"] = RoomJson(id);
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

// The abilities a creature acts with, and each one's cooldown: those on its
// quickbar and those its tactics use (no script command lists a creature's
// abilities whole). A cooldown is in seconds of game time, null for an
// ability the creature has no record for.
Json Abilities(ObjectId id)
{
    constexpr std::int32_t kQuickslots = 60;
    constexpr std::int32_t kTactics = 50;
    std::set<std::int32_t> found;
    for (std::int32_t slot = 0; slot < kQuickslots; ++slot)
    {
        if (const auto ability = api::GetQuickslot(id, slot); ability > 0)
        {
            found.insert(ability);
        }
    }
    const auto tactics = std::min(api::GetNumTactics(id), kTactics);
    for (std::int32_t index = 0; index < tactics; ++index)
    {
        if (const auto ability = api::GetTacticCommandParam(id, index);
            ability > 0 && api::HasAbility(id, ability) != 0)
        {
            found.insert(ability);
        }
    }
    Json abilities = Json::array();
    for (const auto ability : found)
    {
        Json entry = AbilityJson(ability);
        // `available` is the scripts' IsAbilityAvailable, which a cooldown
        // does not change: Dual Striking read available with 29.99 of 30 s
        // left (2026-10-01). `ready` is what a tester means: available and
        // not cooling down.
        const bool available = api::IsAbilityAvailable(id, ability) != 0;
        const auto cooldown = dao::abilities::GetCooldown(id, ability);
        entry["available"] = available;
        entry["ready"] = available && (!cooldown || cooldown->remaining <= 0);
        entry["cooldown"] = cooldown ? Json{{"remaining", cooldown->remaining}, {"total", cooldown->total}} : Json();
        abilities.push_back(std::move(entry));
    }
    return abilities;
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

// ---- deaths ----------------------------------------------------------------

// Deaths, watched as well as heard. A creature that is hurt (a damage or
// an attack's impact) or staged is looked at every frame until it is dead,
// gone, or a minute passes with no more harm, and its death is
// eluvian.creature.died with the last one who hurt it. The engine's own
// EVENT_TYPE_DEATH was not heard at all before DAOLib bc081ea; since, it
// comes as eluvian.script.death 1.5 to 2.5 s after this, once the death
// has played out (play, 2026-09-30). So this is the moment of the death,
// and that its end. On the game's thread only, as every caller is.
class DeathWatch
{
  public:
    void Note(ObjectId id, ObjectId harmer)
    {
        if (id == kInvalidObject)
        {
            return;
        }
        Entry &entry = watched_[id];
        if (harmer != kInvalidObject)
        {
            entry.killer = harmer;
        }
        entry.since = std::chrono::steady_clock::now();
    }

    void Tick(Bench &bench)
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = watched_.begin(); it != watched_.end();)
        {
            const ObjectId id = it->first;
            if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
            {
                it = watched_.erase(it); // gone from the world
                continue;
            }
            if (api::IsDead(id) != 0)
            {
                bench.Events().Emit(
                    "eluvian.creature.died",
                    Json{{"id", IdJson(id)}, {"name", Narrow(api::GetName(id))}, {"killer", Brief(it->second.killer)}});
                it = watched_.erase(it);
                continue;
            }
            it = now - it->second.since > std::chrono::minutes(1) ? watched_.erase(it) : std::next(it);
        }
    }

  private:
    struct Entry
    {
        ObjectId killer = kInvalidObject;
        std::chrono::steady_clock::time_point since;
    };
    std::map<ObjectId, Entry> watched_;
};

DeathWatch &Deaths()
{
    static DeathWatch deaths;
    return deaths;
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
               {"waypoints", {{"type", "boolean"}, {"description", "kind=geometry: list the paths' points"}}},
               {"walk",
                {{"type", "boolean"},
                 {"description", "kind=geometry: also search the path the creature would walk there: its length, "
                                 "or what is in the way"}}},
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
        KindSpec{"popup",
                 "The game's own message box, if one is up: `popup` with its text and each button's label and "
                 "what pressing it sends; null with none. Answers in any mode, the main menu too. A box holds "
                 "the game until it is answered, and nothing else says it is there: ask when a load or another "
                 "order was taken and nothing followed. Reading only: no tool presses a button yet."},
        [](const Json &) -> ToolResult { return Json{{"popup", PopupJson()}}; });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"dialogue",
                 "The conversation on the screen (health's mode is dialog while one is): the `line` spoken last, "
                 "with its speaker and text, and the `replies` the player may choose from now, in their order, "
                 "each with its `place`, text and icon. No replies while the other speaks. The same comes as "
                 "events: eluvian.dialogue.line, .replies and .chosen. `cutscene` says whether one is playing. "
                 "The dialogue tool answers."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto *conversation = dao::dialogue::Current();
            Json answer = {{"active", conversation != nullptr}, {"cutscene", dao::dialogue::InCutscene()}};
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
                answer["walk"] = std::move(walk);
            }
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
// PLACEABLE_ACTION_* from 1 (script.ldf), by the names a tester writes.
constexpr std::array<std::string_view, 17> kPlaceableActionNames = {
    "open",      "close",  "unlock",         "destroy",      "use",    "examine",     "disarm",          "flip_cover",
    "use_cover", "dialog", "open_inventory", "trigger_trap", "topple", "leave_cover", "area_transition", "turn_left",
    "turn_right"};

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
    // The command carries one PLACEABLE_ACTION, and the object does nothing
    // for one it does not take: USE left a chest and a door in the Elven
    // Ruins shut, the command ending COMMAND_FAILED, where a click (which
    // chooses the object's own action) opened both (computer-use-agent,
    // then here, 2026-10-01). So the action is the caller's to name.
    const ObjectId target = args.Id("target", "a placeable or other object to use");
    answer["target"] = Brief(target);
    const auto name = args.String("objectAction", "use");
    const auto found = std::ranges::find(kPlaceableActionNames, name);
    if (found == kPlaceableActionNames.end())
    {
        throw ToolError(std::format("order: no `objectAction` \"{}\"; open, close, unlock, use, examine, disarm, "
                                    "dialog, open_inventory, trigger_trap, topple and area_transition are the "
                                    "usual ones",
                                    name));
    }
    answer["objectAction"] = name;
    return dao::orders::UseObject(who, target,
                                  static_cast<dao::script::PlaceableAction>(found - kPlaceableActionNames.begin() + 1));
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
                 "`ability` (an id from the ability table) on `target` or `point`; use `target` (a placeable: a "
                 "door, a chest, a lever). "
                 "`via`: \"command\" (the default, but for use by the controlled creature) gives it through "
                 "the engine's own click functions, so the "
                 "command is player-issued and anything watching the command queue sees one; any creature "
                 "`id` may obey. \"click\" sends explore mode's click message instead (the primary action on the "
                 "target as if the cursor were over it; an ability as the quickbar uses it, with `target` "
                 "selected), so the click's own handling runs too, and whatever hooks it (a mod watching clicks, "
                 "asking for an ability's target); "
                 "it is the controlled creature's, and cannot move. (Seen 2026-09-30: an ability by click works, the "
                 "mod asking for its target when none is given. An attack or use by click is the primary action "
                 "of every party member selected in the interface, and its answer cannot say whether anyone "
                 "obeyed: the events from `cursor` do.) A use goes by click when the controlled creature is to "
                 "do it, since a click chooses what the object takes (a chest and a door wanted open, and did "
                 "nothing for use, their command ending COMMAND_FAILED); by command it does `objectAction`, "
                 "default use. Answers whether the order was taken, and "
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
                                          "mode's click message (the default for use by the controlled "
                                          "creature)"}}},
                        {"objectAction",
                         {{"type", "string"},
                          {"description", "action=use by command: what to do with the object: open, close, "
                                          "unlock, use (default), examine, disarm, dialog, open_inventory, "
                                          "trigger_trap, topple, area_transition, and the rest of "
                                          "PLACEABLE_ACTION_*. It must be the one the object takes as it "
                                          "is, or the command ends COMMAND_FAILED: a locked chest (state "
                                          "1) took unlock, then open_inventory; a door open"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "order");
            const auto action = args.Choice("action", {"attack", "move", "ability", "use"});
            RequireWorld(watch, "order");
            const ObjectId controlled = api::GetMainControlled();
            const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(controlled);
            if (who == kInvalidObject)
            {
                throw ToolError("order: nobody is controlled and no `id` was given");
            }
            const bool useByClick = action == "use" && who == controlled && !args.Has("objectAction");
            const auto via = args.Choice("via", {"command", "click"}, useByClick ? "click" : "command");
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

// What a built-in stage action does: reads its arguments, changes the game,
// fills its answer, and names the creature it changed.
using StageAction = std::function<ObjectId(const Args &args, Json &answer)>;

ObjectId StageEffect(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
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
    api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_TEMPORARY, *effect.get(), id, static_cast<float>(seconds),
                                    creator, ability);
    answer["applied"] = Json{{"type", Named(static_cast<dao::script::EffectType>(*type))},
                             {"seconds", seconds},
                             {"creator", Brief(creator)},
                             {"ability", AbilityJson(ability)}};
    answer["effects"] = Effects(id);
    return id;
}

ObjectId StageRemoveEffects(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const auto type = EffectTypeArg(args).value_or(k::EFFECT_TYPE_INVALID);
    const auto ability = static_cast<std::int32_t>(args.Integer("ability", k::ABILITY_INVALID, -1, 1 << 30));
    const ObjectId creator = args.OptionalId("creator", kCreatureHint).value_or(kInvalidObject);
    api::RemoveEffectsByParameters(id, type, ability, creator);
    answer["effects"] = Effects(id);
    return id;
}

ObjectId StageHealth(const Args &args, Json & /*answer*/)
{
    const ObjectId id = CreatureArg(args);
    const double value = args.Number("value");
    if (value < 0)
    {
        throw ToolError("stage: `value` is the health to set, 0 or more");
    }
    api::SetCreatureProperty(id, k::PROPERTY_DEPLETABLE_HEALTH, static_cast<float>(value), k::PROPERTY_VALUE_CURRENT);
    return id;
}

ObjectId StageCooldown(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const auto ability = static_cast<std::int32_t>(args.Integer("ability", -1, 0, 1 << 30));
    if (ability < 0)
    {
        throw ToolError("stage: action cooldown needs `ability`, an ability's id (inspect kind=creature lists them)");
    }
    const double remaining = args.Number("seconds", 0);
    const auto before = dao::abilities::GetCooldown(id, ability);
    // The whole it is shown against: given, else what it was, else what is left.
    const double total = args.Number("total", before && before->total > 0 ? before->total : remaining);
    if (remaining < 0 || total < 0 || remaining > 3600)
    {
        throw ToolError("stage: `seconds` is the cooldown left, 0 (ready) to 3600");
    }
    if (!dao::abilities::SetCooldown(id, ability, static_cast<float>(remaining),
                                     static_cast<float>(std::max(total, remaining))))
    {
        throw ToolError(
            std::format("stage: {} has no record for ability {}: it does not have it", FormatId(id), ability));
    }
    const auto after = dao::abilities::GetCooldown(id, ability);
    answer["ability"] = AbilityJson(ability);
    answer["cooldown"] = after ? Json{{"remaining", after->remaining}, {"total", after->total}} : Json();
    return id;
}

ObjectId StageKill(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const float health = api::GetCreatureProperty(id, k::PROPERTY_DEPLETABLE_HEALTH, k::PROPERTY_VALUE_CURRENT);
    const auto damage = api::EffectDamage(health + 1000);
    if (damage.get() == nullptr)
    {
        throw ToolError("stage: the engine made no damage effect");
    }
    api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_INSTANT, *damage.get(), id, 0.0F, kInvalidObject, 0);
    Deaths().Note(id, kInvalidObject);
    answer["dealt"] = health + 1000;
    answer["note"] = "the death comes a frame or more later, so `creature` still lives here: wait_for "
                     "eluvian.creature.died from `cursor`";
    return id;
}

// A creature's group decides whom it fights: the game keeps, for each pair
// of groups, whether they are hostile. Moving a spawned creature to group 2
// set it against a room's hostile creatures at once, the party not in it
// (computer-use-agent with `script`, 2026-10-01): a fight out of the
// party's sight, staged.
ObjectId StageGroup(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const auto group = static_cast<std::int32_t>(args.Integer("group", 0, 0, 255));
    if (!args.Has("group"))
    {
        throw ToolError("stage: group needs `group`, a number: 0 is the player's, 1 the hostile one, 2 friendly; "
                        "inspect gives each creature's");
    }
    answer["was"] = api::GetGroupId(id);
    if (args.Has("toward"))
    {
        const auto toward = static_cast<std::int32_t>(args.Integer("toward", 0, 0, 255));
        api::SetGroupHostility(group, toward, args.Bool("hostile", true) ? 1 : 0);
    }
    api::SetGroupId(id, group);
    const ObjectId hero = api::GetHero();
    answer["group"] = group;
    answer["hostileToParty"] = hero != kInvalidObject && api::GetGroupHostility(group, api::GetGroupId(hero)) != 0;
    answer["hostileToGroup1"] = api::GetGroupHostility(group, 1) != 0;
    return id;
}

ObjectId StageSpawn(const Args &args, Json &answer)
{
    const auto name = args.String("template");
    const auto point = VectorOf(args.Raw().value("point", Json()), "stage: `point`");
    const ObjectId around = api::GetMainControlled();
    if (around == kInvalidObject)
    {
        throw ToolError("stage: nobody is controlled, so there is no area to spawn in");
    }
    const auto facing = static_cast<float>(args.Number("facing", 0));
    const auto spawn = [&](const std::string &resource) {
        return api::CreateObject(k::OBJECT_TYPE_CREATURE, dao::Resource{Wide(resource)},
                                 api::Location(api::GetArea(around), point, facing));
    };
    // The scripts name a creature's template with its extension
    // (R"gen00fl_dog.utc"); GetResRef may give it without.
    ObjectId id = spawn(name);
    if (id == kInvalidObject && name.find('.') == std::string::npos)
    {
        id = spawn(name + ".utc");
    }
    if (id == kInvalidObject)
    {
        throw ToolError(std::format("stage: no creature from template \"{}\"; inspect gives each creature's "
                                    "`template` (its tag names the instance, not this)",
                                    name));
    }
    // The game puts a creature at the nearest place it can stand: a point
    // inside a wall gave one 5 m off in the next room, with no word of it
    // (computer-use-agent, 2026-10-01). `moved` says how far from the point
    // asked for it was made.
    const float moved = Distance(point, api::GetPosition(id));
    answer["asked"] = VectorJson(point);
    answer["moved"] = moved;
    // Read in the frame it was made, a spawned creature is not set up yet:
    // health 1 of 1 in the answer, 85 of 85 three seconds on (2026-09-30).
    answer["note"] = moved > 1.0F
                         ? "the point asked for is not a place to stand: the creature was made `moved` metres from "
                           "it, perhaps in another room (see `creature`'s position and room). It is set up over "
                           "the next frames; inspect it again for its health"
                         : "a creature spawned is set up over the next frames; inspect it again for its health";
    return id;
}

void AddStage(Bench &bench, const Watch &watch)
{
    ToolSpec spec{
        "stage",
        "Set up a test's case, where play rarely brings it. `action` chooses what; the built-in ones answer "
        "`creature` as the change leaves it (health, dead, its command) and `cursor`, to wait_for what follows "
        "from. Mods that register with Eluvian may add actions of their own (an arena, a state only they know). "
        "The effects are the engine's bare ones, applied as scripts apply them; a death comes a frame or more "
        "later, as eluvian.creature.died.",
        Json{
            {"type", "object"},
            {"properties",
             {{"id", {{"description", "The creature"}}},
              {"effect", {{"description", "A condition's name or an EFFECT_TYPE_* number"}}},
              {"seconds",
               {{"type", "number"}, {"description", "action=effect: how long, default 6; action=cooldown: time left"}}},
              {"total", {{"type", "number"}, {"description", "action=cooldown: the whole it counts down from"}}},
              {"creator", {{"description", "Whom the effect is from; default none"}}},
              {"ability", {{"type", "integer"}, {"description", "The ability it is from; default none"}}},
              {"value", {{"type", "number"}, {"description", "action=health: the current health"}}},
              {"group", {{"type", "integer"}, {"description", "action=group: the group to move the creature to"}}},
              {"toward", {{"type", "integer"}, {"description", "action=group: another group, to set hostility with"}}},
              {"hostile", {{"type", "boolean"}, {"description", "action=group: with `toward`; default true"}}},
              {"template", {{"type", "string"}, {"description", "action=spawn: the creature's template"}}},
              {"point", {{"description", "action=spawn: {x, y, z} or [x, y, z]"}}},
              {"facing", {{"type", "number"}, {"description", "action=spawn: degrees; default 0"}}}}}},
        Thread::Frame};
    spec.kindProperty = "action";
    bench.Tools().Add(std::move(spec));

    const auto add = [&bench, &watch](std::string name, std::string description, StageAction act) {
        bench.Tools().AddKind("stage", KindSpec{name, std::move(description), Thread::Frame},
                              [&bench, &watch, name, act = std::move(act)](const Json &raw) -> ToolResult {
                                  const Args args(raw, "stage");
                                  RequireWorld(watch, "stage");
                                  Json answer = {{"action", name}, {"cursor", bench.Events().Next()}};
                                  const ObjectId id = act(args, answer);
                                  // The creature as the change leaves it, in every answer (a
                                  // tester reading `creature.dead` after a kill found no field,
                                  // 2026-09-30).
                                  answer["creature"] = Summary(id, api::GetHero(), PartyOf(api::GetHero()));
                                  return answer;
                              });
    };
    add("effect",
        "puts a condition on creature `id`: `effect` (stun, knockdown, paralyze, root, sleep, daze, slip, fear, "
        "confusion, grabbed, or an EFFECT_TYPE_* number) for `seconds` (default 6), as from `creator` and "
        "`ability` if given; answers the effects on it after",
        StageEffect);
    add("remove_effects", "takes effects off creature `id`: by `effect`, `ability`, `creator`, or all",
        StageRemoveEffects);
    add("health", "sets creature `id`'s current health to `value`", StageHealth);
    add("cooldown",
        "sets creature `id`'s cooldown on `ability` to `seconds` left (0 makes it ready), of `total` if given",
        StageCooldown);
    add("kill", "deals creature `id` more damage than it has; it dies a frame or more later", StageKill);
    add("group",
        "moves creature `id` to `group` (0 the player's, 1 the hostile one, 2 friendly: the game keeps which pairs "
        "of groups fight), and with `toward` sets whether `group` and that group are `hostile` (default true) "
        "first; a spawned creature moved to group 2 fights the hostile ones near it, for a fight the party is "
        "not in",
        StageGroup);
    add("spawn",
        "makes a creature from `template` (as inspect gives each creature's `template`) at `point`, facing "
        "`facing` degrees, in the controlled creature's area; `moved` says how far from the point the game "
        "put it (a point inside a wall is not kept); it is set up over the next frames",
        StageSpawn);
}

// ---- screenshot ------------------------------------------------------------

// Where the pictures go, beside the game's logs: <Documents>\BioWare\
// Dragon Age\Screenshots, Eluvian's own in Eluvian, the game's in User.
std::filesystem::path Screenshots()
{
    return dao::LogDirectory().parent_path() / "Screenshots";
}

// The newest .jpg in a folder.
std::optional<std::filesystem::path> NewestJpeg(const std::filesystem::path &folder)
{
    std::error_code error;
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
    {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".jpg")
        {
            continue;
        }
        const auto written = entry.last_write_time(error);
        if (!newest || written > newestTime)
        {
            newest = entry.path();
            newestTime = written;
        }
    }
    return newest;
}

// The game's own picture: the message its screenshot key sends
// (TakeScreenshotMessage, to the mode controller; DAOLib f253b8c), which
// has the renderer write a JPEG, interface and all, to Screenshots\User a
// frame or more later. Sent on the frame thread, waited for here.
std::filesystem::path GameScreenshot(Bench &bench, const Watch &watch)
{
    const auto folder = Screenshots() / "User";
    const auto before = NewestJpeg(folder);
    bench.Frames().RunAndWait(
        [&watch] {
            RequireWorld(watch, "screenshot");
            auto *app = dao::CClientExoApp::Get();
            auto *modes = app != nullptr ? app->GameModes() : nullptr;
            if (modes == nullptr)
            {
                throw ToolError("screenshot: the game's modes are not up yet");
            }
            dao::messages::Send(modes, dao::messages::Make<dao::messages::TakeScreenshotMessage>());
        },
        std::chrono::milliseconds(5000));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uintmax_t lastSize = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto newest = NewestJpeg(folder);
        if (!newest || newest == before)
        {
            continue;
        }
        // Written when its size holds still between two looks.
        std::error_code error;
        const auto size = std::filesystem::file_size(*newest, error);
        if (!error && size > 0 && size == lastSize)
        {
            return *newest;
        }
        lastSize = error ? 0 : size;
    }
    throw ToolError(std::format("screenshot: the game wrote no picture to {} within 10 s; source \"window\" "
                                "draws the window instead",
                                folder.string()));
}

void AddScreenshot(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"screenshot",
                 "A picture of the game as it is now, interface and all; no window over the game gets in the "
                 "way. `source`: window (the default) has Windows draw the game's window from inside, at once, "
                 "even while frames stop; game has the game take its own, as its screenshot key does, a frame "
                 "or more later. Answers the picture as an image, with its size and file (Screenshots\\Eluvian "
                 "or the game's Screenshots\\User); `image: false` answers the file alone, to save tokens; "
                 "`quality` from 0.1 to 1 (window only), 0.8 by default.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"source", {{"type", "string"}, {"enum", {"window", "game"}}}},
                        {"image", {{"type", "boolean"}, {"description", "Answer the picture itself; default true"}}},
                        {"quality", {{"type", "number"}, {"description", "JPEG quality, 0.1 to 1; default 0.8"}}}}}},
                 Thread::Listener, true},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "screenshot");
            const auto source = args.Choice("source", {"window", "game"}, "window");
            const bool image = args.Bool("image", true);
            std::filesystem::path file;
            std::string jpeg;
            Json answer = {{"source", source}};
            if (source == "game")
            {
                file = GameScreenshot(bench, watch);
                std::ifstream in(file, std::ios::binary);
                jpeg.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            else
            {
                const double quality = args.Number("quality", 0.8);
                if (quality < 0.1 || quality > 1)
                {
                    throw ToolError("screenshot: `quality` is from 0.1 to 1");
                }
                const auto picture = CaptureGameWindow(static_cast<float>(quality));
                const auto folder = Screenshots() / "Eluvian";
                std::error_code error;
                std::filesystem::create_directories(folder, error);
                SYSTEMTIME now{};
                GetLocalTime(&now);
                file = folder / std::format("Eluvian{:04}{:02}{:02}{:02}{:02}{:02}{:03}.jpg", now.wYear, now.wMonth,
                                            now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
                std::ofstream(file, std::ios::binary)
                    .write(picture.jpeg.data(), static_cast<std::streamsize>(picture.jpeg.size()));
                answer["width"] = picture.width;
                answer["height"] = picture.height;
                jpeg = picture.jpeg;
            }
            answer["file"] = file.string();
            answer["bytes"] = jpeg.size();
            ToolResult result(std::move(answer));
            if (image)
            {
                result.content.push_back(Json{{"type", "image"}, {"data", Base64(jpeg)}, {"mimeType", "image/jpeg"}});
            }
            return result;
        });
}

// ---- game ------------------------------------------------------------------

dao::saves::Kind KindArg(const Args &args, std::string_view fallback)
{
    const auto kind = args.Choice("kind", {"manual", "auto", "quick"}, fallback);
    return kind == "auto" ? dao::saves::Kind::Auto
                          : (kind == "quick" ? dao::saves::Kind::Quick : dao::saves::Kind::Manual);
}

Json SlotJson(const dao::saves::Slot &slot)
{
    Json answer = {
        {"character", Narrow(slot.character)}, {"kind", dao::saves::Name(slot.kind)}, {"number", slot.number}};
    if (const auto meta = slot.Meta())
    {
        answer["area"] = Narrow(meta->area);
        answer["name"] = Narrow(meta->name);
        answer["secondsPlayed"] = meta->secondsPlayed;
        answer["level"] = meta->level;
    }
    return answer;
}

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
        [&watch](const Json &raw) -> ToolResult {
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
                    const auto to =
                        target ? api::GetPosition(*target) : VectorOf(args.Raw().at("point"), "camera: `point`");
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
        });
}

// Conversations and cutscenes stand between a test and the world it means
// to act in: tonight's first test with no computer use could not leave the
// clearing it loaded into (2026-10-01). DAOLib's dao::dialogue sends what
// the screen's own clicks and keys send (3d6c2ea).
void AddDialogue(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"dialogue",
                 "Answer the conversation on the screen, or skip a cutscene. `action`: choose, the reply at "
                 "`place` among those inspect kind=dialogue lists, as a click on it; continue, as the skip key: "
                 "the line being spoken is cut short and its replies offered, and a reply with no text is gone "
                 "through; skip_cutscene, as Escape in one. A loop that works: read inspect kind=dialogue; with "
                 "no replies, or a first reply with no text, continue; otherwise choose. Each answers that the "
                 "game was asked, and `cursor`: wait_for eluvian.dialogue.chosen, eluvian.dialogue.line or "
                 "eluvian.mode.changed from it. The game ignores an answer in the moment after replies appear: "
                 "if nothing follows in a second, ask again.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"choose", "continue", "skip_cutscene"}}}},
                        {"place",
                         {{"type", "integer"},
                          {"minimum", 0},
                          {"description", "action=choose: which reply, from 0, as inspect kind=dialogue lists"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "dialogue");
            const Args args(raw, "dialogue");
            const auto action = args.Choice("action", {"choose", "continue", "skip_cutscene"});
            Json answer = {{"action", action}, {"asked", true}, {"cursor", bench.Events().Next()}};
            if (action == "skip_cutscene")
            {
                if (!dao::dialogue::SkipCutscene())
                {
                    throw ToolError("dialogue: no cutscene is playing (health's mode is cutscene while one is)");
                }
                return answer;
            }
            if (dao::dialogue::Current() == nullptr)
            {
                throw ToolError(dao::dialogue::InCutscene()
                                    ? "dialogue: a cutscene is playing, not a conversation: action skip_cutscene"
                                    : "dialogue: no conversation is on the screen (health's mode is dialog "
                                      "while one is)");
            }
            if (action == "continue")
            {
                if (!dao::dialogue::Continue())
                {
                    throw ToolError("dialogue: the game took no continue; inspect kind=dialogue says where "
                                    "the conversation is");
                }
                return answer;
            }
            const auto replies = dao::dialogue::Replies();
            if (replies.empty())
            {
                throw ToolError("dialogue: no replies are offered yet, the other is speaking: action continue "
                                "cuts the line short, or wait_for eluvian.dialogue.replies");
            }
            if (!args.Has("place"))
            {
                throw ToolError(std::format("dialogue: choose needs `place`, 0 to {}; inspect kind=dialogue "
                                            "lists the replies",
                                            replies.size() - 1));
            }
            const auto place =
                static_cast<std::size_t>(args.Integer("place", 0, 0, static_cast<std::int64_t>(replies.size()) - 1));
            const bool female = HeroIsFemale();
            if (replies[place].text == 0xFFFFFFFF)
            {
                throw ToolError("dialogue: that reply has no text, which the game goes through on action "
                                "continue, not choose");
            }
            answer["reply"] = LineJson(replies[place], female);
            if (!dao::dialogue::Choose(place))
            {
                throw ToolError("dialogue: the game took no choice; inspect kind=dialogue says where the "
                                "conversation is");
            }
            return answer;
        });
}

void AddGame(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        // No game speed here. Each plugin's copy of DAOLib hooks the frame's
        // timer on its first SetSpeed, so two plugins setting a speed
        // multiply, and each reads back only its own (DAOLib ed55ab8,
        // 2026-09-30). One plugin owns the speed: in a turn-based test the
        // mod, through turnbased_settings' ai_turn_speed.
        ToolSpec{"game",
                 "The game itself. `action`: pause, unpause; saves, the saves of `character` (or every one) with "
                 "each one's area, name, time played and level, as the load screen shows them; load, a save by "
                 "`character` (default the one played, or the menu's), `kind` (manual, auto, quick; default "
                 "manual) and `number`, from the main menu or in the world, as the load screen loads; save, a new "
                 "manual save in the next free slot, with `name` (the default: it writes over nothing), a manual "
                 "save to slot `number` (which writes over what is there), or `kind` quick, which writes over "
                 "the player's quicksave. A load or save answers that the game was asked, and `cursor`: wait_for "
                 "eluvian.game.loaded or eluvian.game.saved from it. The game declines a save in combat or a "
                 "cutscene. (Not the game's speed: each plugin's copy of DAOLib scales the clock by its own, so "
                 "two setting it multiply; a mod that sets speed owns it.)",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"pause", "unpause", "saves", "load", "save"}}}},
                        {"character", {{"type", "string"}, {"description", "A character's save folder, \"Aedan\""}}},
                        {"kind", {{"type", "string"}, {"enum", {"manual", "auto", "quick"}}}},
                        {"number", {{"type", "integer"}, {"description", "The slot's number"}}},
                        {"name", {{"type", "string"}, {"description", "action=save: a manual save's name"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "game");
            const auto action = args.Choice("action", {"pause", "unpause", "saves", "load", "save"});
            auto *app = dao::CClientExoApp::Get();
            if (app == nullptr || watch.Mode() < 0)
            {
                throw ToolError("game: the game is still starting; health says when it runs");
            }
            if (action == "pause" || action == "unpause")
            {
                app->SetPaused(action == "pause");
                return Json{{"paused", app->IsPaused()}};
            }
            if (action == "saves")
            {
                std::vector<std::wstring> characters;
                if (args.Has("character"))
                {
                    characters.push_back(Wide(args.String("character")));
                }
                else
                {
                    characters = dao::saves::Characters();
                }
                Json saves = Json::array();
                for (const auto &character : characters)
                {
                    for (const auto &slot : dao::saves::List(character))
                    {
                        saves.push_back(SlotJson(slot));
                    }
                }
                return Json{{"current", Narrow(dao::saves::Character())}, {"saves", std::move(saves)}};
            }
            RequireWorld(watch, "game");
            const auto cursor = bench.Events().Next();
            if (action == "load")
            {
                const dao::saves::Slot slot{Wide(args.String("character", "")),
                                            static_cast<std::int32_t>(args.Integer("number", 1, 0, 500)),
                                            KindArg(args, "manual")};
                if (!dao::saves::Load(slot))
                {
                    throw ToolError("game: no such save to load: the slot holds none, or the game knows no such "
                                    "character; action saves lists them");
                }
                return Json{{"asked", true},
                            {"save", SlotJson(slot)},
                            {"cursor", cursor},
                            {"note",
                             "wait_for eluvian.game.loaded from `cursor`. If none comes and health's mode does not "
                             "move, the game is showing a message box of its own (a save that needs add-ins "
                             "not registered asks Force Load or Cancel): inspect kind=popup reads it"}};
            }
            // A new manual save by default: a test's save must not write over
            // the player's own, as a quicksave does (computer-use-agent would
            // not call this for that reason, 2026-09-30).
            const auto kind = KindArg(args, "manual");
            const auto number = static_cast<std::int32_t>(args.Integer("number", -1, -1, 500));
            if (!dao::saves::Save(kind, number, Wide(args.String("name", ""))))
            {
                throw ToolError("game: the game began no save: it allows none now (in combat, in a cutscene), or "
                                "is still starting");
            }
            return Json{{"began", true},
                        {"kind", dao::saves::Name(kind)},
                        {"slot", number < 0 ? Json("the game's choice: eluvian.game.saved names it") : Json(number)},
                        {"cursor", cursor},
                        {"note", "wait_for eluvian.game.saved from `cursor`"}};
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
// dozen a second and say little; nor EVENT_TYPE_COMMAND_COMPLETE, the most
// frequent of all in play (2,094 of a session's 12,743 events, 2026-09-30),
// which eluvian.command.ended says better: who, what, and how it ended.
constexpr ScriptEventTopic kScriptEvents[] = {
    {dao::script::EventType::Damaged, "damaged"},
    {dao::script::EventType::Death, "death"},
    {dao::script::EventType::AttackImpact, "attack_impact"},
    {dao::script::EventType::CombatInitiated, "combat_initiated"},
    {dao::script::EventType::AbilityCastStart, "ability_cast_start"},
    {dao::script::EventType::AbilityCastImpact, "ability_cast_impact"},
    {dao::script::EventType::ApplyEffect, "apply_effect"},
    {dao::script::EventType::RemoveEffect, "remove_effect"},
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
    AddScreenshot(bench, watch);
    AddGame(bench, watch);
    AddDialogue(bench, watch);
    AddCamera(bench, watch);
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

namespace
{

// What each of Eluvian's own topics means, in the catalogue a mod's go into
// too (`events describe=true`).
void DescribeTopics(EventCatalog &topics)
{
    const auto describe = [&topics](std::string topic, std::string description, Json fields) {
        topics.Describe(TopicSpec{std::move(topic), std::move(description), std::move(fields), "eluvian"});
    };
    describe("eluvian.mode.changed",
             "The game mode changed: explore, combat, dialog, cutscene, loading, pregame "
             "(the main menu), gui (a menu over the game), and the rest.",
             Json{{"from", "the mode before"}, {"to", "the mode now"}});
    describe("eluvian.paused", "The game was paused or unpaused.", Json{{"paused", "true or false"}});
    describe("eluvian.game.loaded", "A save loaded, or a new game began, and the world is in place.",
             Json{{"newGame", "true for a new game"},
                  {"character", "the save's character folder"},
                  {"kind", "manual, auto or quick"},
                  {"number", "the slot's number"}});
    describe("eluvian.game.saved", "The game wrote a save.",
             Json{{"character", "the save's character folder"},
                  {"kind", "manual, auto or quick"},
                  {"number", "the slot's number"}});
    describe("eluvian.dialogue.line",
             "A line was spoken to the player in a conversation, or by creatures in passing (ambient lines "
             "are conversations too). Some have no text: a line that only routes to the next.",
             Json{{"index", "the line's place in the conversation's file"},
                  {"speaker", "who, as the file writes it: a tag, OWNER or PLAYER"},
                  {"text", "the line, in the player's language"}});
    describe("eluvian.dialogue.replies", "The replies now offered to the player, in their order on the screen.",
             Json{{"replies", "each with its place, index, text and icon"}});
    describe("eluvian.dialogue.chosen", "The player chose a reply, and the conversation goes on from it.",
             Json{{"index", "the reply's place in the file"}, {"text", "the reply"}});
    describe("eluvian.creature.died",
             "A creature died, or a party member fell, the moment it did: seen by watching creatures that were "
             "hurt or staged. The engine's own eluvian.script.death comes 1.5 to 2.5 s later, once the death has "
             "played out.",
             Json{{"id", "the creature"}, {"name", "its name"}, {"killer", "the last who hurt it, if known"}});
    describe("eluvian.command.started",
             "A creature started a command from its queue: an attack, a move, an "
             "ability, a wait, an animation.",
             Json{{"id", "the creature"},
                  {"type", "the command (Attack, MoveToObject, UseAbility, ...)"},
                  {"playerIssued", "whether the player gave it"}});
    describe("eluvian.command.ended", "A creature's command ended, and how.",
             Json{{"id", "the creature"},
                  {"type", "the command"},
                  {"playerIssued", "whether the player gave it"},
                  {"result", "success, a COMMAND_FAILED_*, or cleared"},
                  {"success", "true when it succeeded"}});
    const Json scriptFields = {{"receiver", "the object the event was delivered to (not always the one it is about)"},
                               {"creator", "the event's creator"},
                               {"objects", "its object parameters, as GetEventObject reads them"},
                               {"integers", "its integer parameters"},
                               {"floats", "its float parameters"}};
    for (const auto &[type, name] : kScriptEvents)
    {
        Json fields = scriptFields;
        std::string description =
            std::format("The script event {} (EVENT_TYPE_*), as the scripts receive it.", dao::script::Name(type));
        if (type == dao::script::EventType::AttackImpact)
        {
            fields["attacker"] = "who attacked (delivered to them)";
            fields["target"] = "who was attacked";
            fields["hit"] = "COMBAT_RESULT_*";
            fields["effectId"] = "the effect it made";
        }
        else if (type == dao::script::EventType::Damaged)
        {
            fields["damager"] = "who dealt it";
            fields["amount"] = "how much";
            fields["damageType"] = "DAMAGE_TYPE_*";
            fields["ability"] = "the ability it came from";
        }
        describe(std::format("eluvian.script.{}", name), std::move(description), std::move(fields));
    }
}

} // namespace

void AddEngineEvents(Bench &bench)
{
    DescribeTopics(bench.Topics());

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
    // Conversations, as DAOLib's hooks see them: inside the engine's own
    // work, so each does no more than read the line and put it on the bus.
    dao::dialogue::OnNpcLine([&bench](dao::dialogue::Conversation &, const dao::dialogue::Line &line) {
        bench.Events().Emit("eluvian.dialogue.line", LineJson(line, HeroIsFemale()));
    });
    dao::dialogue::OnReplies([&bench](dao::dialogue::Conversation &, std::span<const dao::dialogue::Line> replies) {
        bench.Events().Emit("eluvian.dialogue.replies", Json{{"replies", RepliesJson(replies, HeroIsFemale())}});
    });
    dao::dialogue::OnReplyChosen([&bench](dao::dialogue::Conversation &, const dao::dialogue::Line &reply) {
        bench.Events().Emit("eluvian.dialogue.chosen", LineJson(reply, HeroIsFemale()));
    });
    dao::saves::OnSaved([&bench](const dao::saves::Slot &slot) {
        bench.Events().Emit("eluvian.game.saved", Json{{"character", Narrow(slot.character)},
                                                       {"kind", dao::saves::Name(slot.kind)},
                                                       {"number", slot.number}});
    });
    for (const auto &[type, name] : kScriptEvents)
    {
        dao::events::OnScriptEvent(type, [&bench, type, topic = std::format("eluvian.script.{}", name)](
                                             ObjectId receiver, const dao::CScriptEvent &event) {
            bench.Events().Emit(topic, ScriptEventPayload(receiver, event));
            if (type == dao::script::EventType::Damaged)
            {
                Deaths().Note(receiver, event.Creator());
            }
            else if (type == dao::script::EventType::AttackImpact)
            {
                Deaths().Note(event.Object(1), event.Object(0));
            }
        });
    }
    dao::OnFrame([&bench] { Deaths().Tick(bench); });
}

} // namespace eluvian::game
