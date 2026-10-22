// `snapshot`: one frame's state in the schema core/Snapshot.h names, for
// snapshot_diff to compare with another engine's or another moment's (#167).
// Every creature of the area, inactive ones too, since an engine building the
// area from its file makes them all; keyed by tag, template and their order of
// making, which in the game is their ids' order. Reads only.
#include "core/Snapshot.h"
#include "game/Shared.h"

#include <dao/Clock.h>

namespace eluvian::game
{

namespace
{

constexpr std::int32_t kMostFlags = 512; // as plot asks them: 0 up, with gaps

// The world's clock, the one a save writes and a load restores (DAOLib #232,
// seen in play 2026-10-22: a load went back to the save's time). The
// scripts' GetTime counts from the process's start instead, so two runs did
// not line up by it.
Json WorldSeconds()
{
    const auto now = dao::clock::Now();
    return now ? Json(now->seconds) : Json(nullptr);
}

// The same as the save keeps it: the calendar's day and the ms into it.
Json WorldDayMs()
{
    const auto now = dao::clock::Now();
    return now ? Json{{"day", now->day}, {"ms", now->ms}} : Json(nullptr);
}

// The creatures of the hero's area in the order they were made, each with
// its key.
std::vector<std::pair<ObjectId, std::string>> KeyedCreatures(ObjectId area)
{
    std::vector<ObjectId> ids;
    for (const ObjectId id : Objects(api::GetObjectsInArea(area)))
    {
        if (api::GetObjectType(id) == k::OBJECT_TYPE_CREATURE)
        {
            ids.push_back(id);
        }
    }
    std::ranges::sort(ids);
    std::vector<std::pair<std::string, std::string>> kinds;
    kinds.reserve(ids.size());
    for (const ObjectId id : ids)
    {
        kinds.emplace_back(Narrow(api::GetTag(id)), Narrow(api::GetResRef(id)));
    }
    const auto keys = CreatureKeys(kinds);
    std::vector<std::pair<ObjectId, std::string>> keyed;
    keyed.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        keyed.emplace_back(ids[i], keys[i]);
    }
    return keyed;
}

class Keys
{
  public:
    explicit Keys(const std::vector<std::pair<ObjectId, std::string>> &keyed)
    {
        for (const auto &[id, key] : keyed)
        {
            byId_[id] = key;
            byKey_[key] = id;
        }
    }

    // A creature's key; an object no creature of the area, by its id; none, null.
    [[nodiscard]] Json Of(ObjectId id) const
    {
        if (id == kInvalidObject)
        {
            return nullptr;
        }
        const auto found = byId_.find(id);
        return found != byId_.end() ? Json(found->second) : Json(FormatId(id));
    }

    [[nodiscard]] std::optional<ObjectId> Find(const std::string &key) const
    {
        const auto found = byKey_.find(key);
        return found != byKey_.end() ? std::optional{found->second} : std::nullopt;
    }

  private:
    std::map<ObjectId, std::string> byId_;
    std::map<std::string, ObjectId, std::less<>> byKey_;
};

Json EffectsOf(ObjectId id)
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
        const auto ability = effect->AbilityId();
        effects.push_back(Json{{"type", Named(effect->Type())},
                               {"ability", ability > 0 ? Json(ability) : Json(nullptr)},
                               {"remaining", timed ? Json(effect->Remaining()) : Json(nullptr)}});
    }
    return effects;
}

Json CreatureOf(ObjectId id, const std::string &key, const Keys &keys)
{
    const auto *command = CurrentCommand(id);
    return Json{{"key", key},
                {"id", FormatId(id)}, // to act on it here; another engine's differ, so not compared
                {"tag", Narrow(api::GetTag(id))},
                {"template", Narrow(api::GetResRef(id))},
                {"name", Narrow(api::GetName(id))},
                {"position", VectorJson(api::GetPosition(id))},
                {"facingDegrees", api::GetFacing(id)},
                {"health", Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH)},
                {"stamina", Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA)},
                {"dead", api::IsDead(id) != 0},
                {"active", api::GetObjectActive(id) != 0},
                {"group", api::GetGroupId(id)},
                {"combatState", api::GetCombatState(id)},
                {"attackTarget", keys.Of(api::GetAttackTarget(id))},
                {"command", command != nullptr ? Json(static_cast<int>(command->Type())) : Json(nullptr)},
                {"effects", EffectsOf(id)}};
}

// The flags of each plot asked, by name: {plot: {flag: set}}.
Json Plots(const Json &asked)
{
    Json plots = Json::object();
    const ObjectId party = api::GetParty(api::GetHero());
    for (const auto &name : asked)
    {
        if (!name.is_string())
        {
            throw ToolError(R"(snapshot: `plots` lists plots by name, like ["bed000pt_main"])");
        }
        const auto plot = name.get<std::string>();
        const std::wstring guid = PlotGuid(plot, "snapshot");
        Json flags = Json::object();
        for (std::int32_t flag = 0; flag < kMostFlags; ++flag)
        {
            if (const std::wstring flagName = api::GetPlotFlagName(guid, flag); !flagName.empty())
            {
                flags[Narrow(flagName)] = api::GetPartyPlotFlag(party, guid, flag, 0) != 0;
            }
        }
        plots[plot] = std::move(flags);
    }
    return plots;
}

// The object a variable is asked of: module, area, hero, controlled, a
// creature's key, or an id.
ObjectId VarObject(const std::string &object, const Keys &keys, ObjectId area)
{
    if (object == "module")
    {
        return api::GetModule();
    }
    if (object == "area")
    {
        return area;
    }
    if (object == "hero")
    {
        return api::GetHero();
    }
    if (object == "controlled")
    {
        return api::GetMainControlled();
    }
    if (const auto id = keys.Find(object))
    {
        return *id;
    }
    if (const auto id = ParseId(Json(object)))
    {
        return *id;
    }
    throw ToolError(std::format("snapshot: no object `{}` for a variable; give module, area, hero, controlled, a "
                                "creature's key as this snapshot names it, or an id",
                                object));
}

Json Vars(const Json &asked, const Keys &keys, ObjectId area)
{
    Json vars = Json::array();
    for (const auto &var : asked)
    {
        if (!var.is_object() || !var.contains("name"))
        {
            throw ToolError(R"(snapshot: each of `vars` is {"object": "module", "name": "...", "type": "int"})");
        }
        const auto object = var.value("object", std::string("module"));
        const auto name = var.at("name").get<std::string>();
        const auto type = var.value("type", std::string("int"));
        const ObjectId id = VarObject(object, keys, area);
        Json value;
        if (type == "int")
        {
            value = api::GetLocalInt(id, Wide(name));
        }
        else if (type == "float")
        {
            value = api::GetLocalFloat(id, Wide(name));
        }
        else if (type == "string")
        {
            value = Narrow(api::GetLocalString(id, Wide(name)));
        }
        else if (type == "object")
        {
            value = keys.Of(api::GetLocalObject(id, Wide(name)));
        }
        else
        {
            throw ToolError(
                std::format("snapshot: a variable's `type` is int, float, string or object, not `{}`", type));
        }
        vars.push_back(Json{{"object", object}, {"name", name}, {"type", type}, {"value", std::move(value)}});
    }
    return vars;
}

void Keep(const Json &snapshot, const std::string &path)
{
    std::ofstream file(std::filesystem::path(Wide(path)), std::ios::binary);
    if (!file)
    {
        throw ToolError(std::format("snapshot: `file`: {} cannot be written", path));
    }
    file << snapshot.dump(2) << '\n';
}

ToolResult SnapshotTool(Bench &bench, const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "snapshot");
    const Args args(raw, "snapshot");
    const ObjectId hero = api::GetHero();
    const ObjectId area = api::GetArea(hero);
    const auto keyed = KeyedCreatures(area);
    const Keys keys(keyed);
    const auto *app = dao::CClientExoApp::Get();

    Json snapshot = {{"schema", kSnapshotSchema},
                     {"source", "eluvian"},
                     {"area", Narrow(api::GetTag(area))},
                     {"mode", ModeName(watch.Mode())},
                     {"paused", app != nullptr && app->IsPaused()},
                     {"gameTime", WorldSeconds()},
                     {"worldTime", WorldDayMs()},
                     {"frame", watch.Frames()},
                     {"cursor", bench.Events().Next()}};
    Json creatures = Json::object();
    for (const auto &[id, key] : keyed)
    {
        creatures[key] = CreatureOf(id, key, keys);
    }
    snapshot["creatures"] = std::move(creatures);
    Json party = Json::array();
    for (const ObjectId member : PartyOf(hero))
    {
        party.push_back(keys.Of(member));
    }
    snapshot["party"] = std::move(party);
    snapshot["controlled"] = keys.Of(api::GetMainControlled());
    if (args.Has("plots"))
    {
        snapshot["plots"] = Plots(args.Raw().at("plots"));
    }
    if (args.Has("vars"))
    {
        snapshot["vars"] = Vars(args.Raw().at("vars"), keys, area);
    }
    if (args.Has("file"))
    {
        Keep(snapshot, args.String("file"));
        snapshot["file"] = args.String("file");
    }
    return snapshot;
}

} // namespace

void AddSnapshot(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("snapshot",
             "Takes the game's state in one frame, as a JSON another engine can write too, to compare with "
             "snapshot_diff: the same save and inputs run here and elsewhere, or this game at two moments. It has the "
             "area, mode, paused, `gameTime` (the world clock a save keeps, in seconds; `worldTime` the same as day "
             "and ms), the frame and an events `cursor`; every "
             "creature of the area, inactive ones too, keyed `tag|template#k` (k counts that tag and template in the "
             "order they were made), with position, facingDegrees, health and stamina, dead, active, group, "
             "combatState, attackTarget (a key), the command's type number and effects (type, ability, remaining); "
             "the party and the one controlled, by key. Pass `plots` for their flags by name, `vars` for variables, "
             "and `file` to keep it on disk for snapshot_diff. To line two runs up, pause, act, let the same game "
             "time pass, pause again, then take it.",
             Json{{"plots",
                   {{"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Plots whose flags to include, by resource name (bed000pt_main)."}}},
                  {"vars",
                   {{"type", "array"},
                    {"items", {{"type", "object"}}},
                    {"description", "Variables to include, each with object, name and type: object is module "
                                    "(the default), area, hero, controlled, a creature's key or an id; type is int "
                                    "(the default), float, string or object. A variable the object's scripts do "
                                    "not declare reads as 0 or empty."}}},
                  {"file",
                   {{"type", "string"},
                    {"description", "A path to write the snapshot to as well, for snapshot_diff to read later."}}}},
             {}),
        [&bench, &watch](const Json &raw) { return SnapshotTool(bench, watch, raw); });
}

} // namespace eluvian::game
