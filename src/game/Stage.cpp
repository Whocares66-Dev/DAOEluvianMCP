// `stage`: a test's set-up, where play rarely brings the case: conditions,
// health, cooldowns, a death, a creature's group, a spawn. Each is a kind
// of the tool, as a plugin's own actions are.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

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

} // namespace

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

} // namespace eluvian::game
