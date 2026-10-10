// The stage tools: a test's set-up, where play rarely brings the case:
// conditions, health, cooldowns, a death, a creature's group, a spawn, an
// item, a cutscene, a reset. One `stage` with an action each shared one
// schema of 21 arguments; a tool each is one concept (the user, 2026-10-09,
// #110). Each answers `creature` as the change leaves it and `cursor`.
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
    throw ToolError(args.Tool() + ": `effect` is one of " + names + ", or an EFFECT_TYPE_* number");
}

ObjectId CreatureArg(const Args &args)
{
    const ObjectId id = args.Id("id", kCreatureHint);
    if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("{}: {} is not a creature; creatures lists them", args.Tool(), FormatId(id)));
    }
    return id;
}

// What a stage tool does: reads its arguments, changes the game,
// fills its answer, and names the creature it changed.
using StageAction = std::function<ObjectId(const Args &args, Json &answer)>;

ObjectId StageEffect(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const auto type = EffectTypeArg(args);
    if (!type)
    {
        throw ToolError(args.Tool() + ": apply needs `effect`, a condition's name or a number");
    }
    const double seconds = args.Number("seconds", 6);
    if (seconds <= 0 || seconds > 600)
    {
        throw ToolError(args.Tool() + ": `seconds` must be more than 0 and at most 600");
    }
    const ObjectId creator = args.OptionalId("creator", kCreatureHint).value_or(kInvalidObject);
    const auto ability = static_cast<std::int32_t>(args.Integer("ability", 0, 0, 1 << 30));
    const auto effect = api::Effect(*type);
    if (effect.get() == nullptr)
    {
        throw ToolError(args.Tool() + ": the engine made no effect of that type");
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

// `ids`: the creatures named, each one checked.
std::vector<ObjectId> NamedCreatures(const Json &ids)
{
    if (!ids.is_array())
    {
        throw ToolError("stage_reset: `ids` is a list of creature ids; leave it out for the party");
    }
    std::vector<ObjectId> who;
    for (const auto &value : ids)
    {
        const auto id = ParseId(value);
        if (!id || api::GetObjectType(*id) != k::OBJECT_TYPE_CREATURE)
        {
            throw ToolError(std::format("stage_reset: {} in `ids` is no creature; creatures lists them", value.dump()));
        }
        who.push_back(*id);
    }
    return who;
}

// Every cooldown of a creature's abilities cleared: how many were running.
int ClearCooldowns(ObjectId id)
{
    int cleared = 0;
    for (const auto kind : {dao::abilities::Kind::Talent, dao::abilities::Kind::Spell, dao::abilities::Kind::Skill,
                            dao::abilities::Kind::Item})
    {
        for (const std::int32_t ability : dao::abilities::Known(id, kind))
        {
            const auto cooldown = dao::abilities::GetCooldown(id, ability);
            if (cooldown && cooldown->remaining > 0 && dao::abilities::SetCooldown(id, ability, 0, cooldown->total))
            {
                ++cleared;
            }
        }
    }
    return cleared;
}

// One creature back to before a fight: raised, its effects gone if
// asked, full, its cooldowns cleared.
Json ResetOne(ObjectId id, bool effects)
{
    Json entry = Brief(id);
    if (api::IsDead(id) != 0)
    {
        if (const auto raise = api::Effect(k::EFFECT_TYPE_RESURRECTION); raise.get() != nullptr)
        {
            api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_INSTANT, *raise.get(), id);
        }
        entry["raised"] = true;
    }
    if (effects)
    {
        api::RemoveEffectsByParameters(id, k::EFFECT_TYPE_INVALID, k::ABILITY_INVALID, kInvalidObject);
    }
    for (const auto property : {k::PROPERTY_DEPLETABLE_HEALTH, k::PROPERTY_DEPLETABLE_MANA_STAMINA})
    {
        api::SetCreatureProperty(id, property, api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_TOTAL),
                                 k::PROPERTY_VALUE_CURRENT);
    }
    entry["cooldownsCleared"] = ClearCooldowns(id);
    entry["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
    entry["manaOrStamina"] = Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA);
    return entry;
}

// Back to before a fight, so many fights run in one launch (turn-based-
// agent's balance runs, 2026-10-02): the party, or the creatures named,
// raised if fallen (the rules' resurrection effect, as the scripts' own
// ResurrectCreature applies it), health and mana or stamina full, every
// cooldown cleared, and the dead that are not the party's taken away.
ObjectId StageReset(const Args &args, Json &answer)
{
    const ObjectId hero = api::GetHero();
    const auto party = PartyOf(hero);
    std::vector<ObjectId> who(party.begin(), party.end());
    if (args.Has("ids"))
    {
        who = NamedCreatures(args.Raw().at("ids"));
    }
    const bool effects = args.Bool("effects", false);
    Json reset = Json::array();
    for (const ObjectId id : who)
    {
        reset.push_back(ResetOne(id, effects));
    }
    // The fight's dead only: those seen to die since the last reset, not
    // the bodies the area held before (twelve of the player's earlier
    // fights' went with a reset that took every dead creature, 2026-10-02).
    Json removed = Json::array();
    if (args.Bool("clearDead", true))
    {
        for (const ObjectId id : Deaths().TakeFallen())
        {
            if (api::GetObjectType(id) == k::OBJECT_TYPE_CREATURE && !party.contains(id) && api::IsDead(id) != 0)
            {
                removed.push_back(Brief(id));
                api::DestroyObject(id);
            }
        }
    }
    answer["reset"] = std::move(reset);
    answer["removed"] = std::move(removed);
    return hero;
}

// The slots an item is put in, by the names equipment gives.
struct EquipSlot
{
    std::string_view name;
    std::int32_t id;
};
constexpr std::array<EquipSlot, 12> kEquipSlots = {{{"main", 0},
                                                    {"offhand", 1},
                                                    {"ammo", 2},
                                                    {"chest", 4},
                                                    {"head", 5},
                                                    {"boots", 6},
                                                    {"gloves", 7},
                                                    {"cloak", 8},
                                                    {"ring1", 9},
                                                    {"ring2", 10},
                                                    {"neck", 11},
                                                    {"belt", 12}}};

// An item made from its template on a creature and put on, for a test's
// gear (turn-based-agent, 2026-10-02): the scripts' CreateItemOnObject and
// EquipItem, the slot the item's own unless one is named.
ObjectId StageEquip(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    std::string name = args.String("template");
    if (name.find('.') == std::string::npos)
    {
        name += ".uti";
    }
    const ObjectId item = api::CreateItemOnObject(dao::Resource{Wide(name)}, id);
    if (item == kInvalidObject)
    {
        throw ToolError(std::format("{}: no item from template \"{}\"; resources with type uti "
                                    "and a word finds templates",
                                    args.Tool(), name));
    }
    std::int32_t slot = k::INVENTORY_SLOT_INVALID;
    if (args.Has("slot"))
    {
        const std::string wanted = args.String("slot");
        const auto found = std::ranges::find(kEquipSlots, wanted, &EquipSlot::name);
        if (found == kEquipSlots.end())
        {
            throw ToolError(std::format("{}: no slot \"{}\"; main, offhand, ammo, chest, head, boots, gloves, "
                                        "cloak, ring1, ring2, neck, belt",
                                        args.Tool(), wanted));
        }
        slot = found->id;
    }
    const bool on = args.Bool("equip", true) && api::EquipItem(id, item, slot) != 0;
    answer["item"] =
        Json{{"id", IdJson(item)}, {"name", Narrow(api::GetName(item))}, {"tag", Narrow(api::GetTag(item))}};
    answer["equipped"] = on;
    if (args.Bool("equip", true) && !on)
    {
        answer["note"] = "made, but not put on: the creature cannot use it there (item says its kind); "
                         "it is in the backpack";
    }
    return id;
}

ObjectId StageHealth(const Args &args, Json & /*answer*/)
{
    const ObjectId id = CreatureArg(args);
    const double value = args.Number("value");
    if (value < 0)
    {
        throw ToolError(args.Tool() + ": `value` is the health to set, 0 or more");
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
        throw ToolError(args.Tool() + ": `ability` is needed, an ability's id (creature lists them)");
    }
    const double remaining = args.Number("seconds", 0);
    const auto before = dao::abilities::GetCooldown(id, ability);
    // The whole it is shown against: given, else what it was, else what is left.
    const double total = args.Number("total", before && before->total > 0 ? before->total : remaining);
    if (remaining < 0 || total < 0 || remaining > 3600)
    {
        throw ToolError(args.Tool() + ": `seconds` is the cooldown left, 0 (ready) to 3600");
    }
    if (!dao::abilities::SetCooldown(id, ability, static_cast<float>(remaining),
                                     static_cast<float>(std::max(total, remaining))))
    {
        throw ToolError(
            std::format("stage_cooldown: {} has no record for ability {}: it does not have it", FormatId(id), ability));
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
        throw ToolError(args.Tool() + ": the engine made no damage effect");
    }
    api::Engine_ApplyEffectOnObject(k::EFFECT_DURATION_TYPE_INSTANT, *damage.get(), id, 0.0F, kInvalidObject, 0);
    Deaths().Note(id, kInvalidObject);
    answer["dealt"] = health + 1000;
    // A death is the game's to play out, and a paused game plays nothing: a
    // wolf killed while a fight's start had the game paused was dead only
    // when it ran again, 14 s on, long after the caller's wait gave up
    // (2026-10-01).
    const auto *app = dao::CClientExoApp::Get();
    const bool paused = app != nullptr && app->IsPaused();
    answer["paused"] = paused;
    answer["note"] = paused ? "the game is paused, and a death does not come while it is: pause paused=false, then "
                              "wait_for eluvian.creature.died from `cursor`"
                            : "the death comes a frame or more later, so `creature` still lives here: wait_for "
                              "eluvian.creature.died from `cursor`";
    return id;
}

// A creature's group decides whom it fights: the game keeps, for each pair
// of groups, whether they are hostile. Moving a spawned creature to group 2
// set it against a room's hostile creatures at once, the party not in it
// (computer-use-agent with `script`, 2026-10-01): a fight out of the
// party's sight, staged.
void PutInGroup(ObjectId id, std::int32_t group, Json &answer)
{
    api::SetGroupId(id, group);
    const ObjectId hero = api::GetHero();
    answer["group"] = api::GetGroupId(id);
    answer["hostileToParty"] = hero != kInvalidObject && api::GetGroupHostility(group, api::GetGroupId(hero)) != 0;
    answer["hostileToGroup1"] = api::GetGroupHostility(group, 1) != 0;
}

ObjectId StageGroup(const Args &args, Json &answer)
{
    const ObjectId id = CreatureArg(args);
    const auto group = static_cast<std::int32_t>(args.Integer("group", 0, 0, 255));
    if (!args.Has("group"))
    {
        throw ToolError(args.Tool() +
                        ": `group` is needed, a number: 0 is the player's, 1 the hostile one, 2 friendly; "
                        "creatures gives each creature's");
    }
    answer["was"] = api::GetGroupId(id);
    if (args.Has("toward"))
    {
        const auto toward = static_cast<std::int32_t>(args.Integer("toward", 0, 0, 255));
        api::SetGroupHostility(group, toward, args.Bool("hostile", true) ? 1 : 0);
    }
    PutInGroup(id, group, answer);
    return id;
}

// A cutscene, started as the scripts start one (LoadCutscene): for a test
// of what a cutscene does to the thing under test, or of skipping one. No
// test met the game's cutscene mode by playing (the Dalish origin's scenes
// went by as conversations, 2026-10-01), so this is how one is had. Loading
// is playing: the mode is cutscene at once (lib-agent's reading of the
// handler; the command's bPlayImmediately is not used). The actors are the
// cutscene's own: none is replaced.
ObjectId StageCutscene(const Args &args, Json &answer)
{
    const ObjectId hero = api::GetHero();
    if (hero == kInvalidObject)
    {
        throw ToolError(args.Tool() + ": no hero, so nobody to play a cutscene for; is a game loaded?");
    }
    // The scripts name a cutscene with its extension: "bed110cs_tainted_mirror"
    // did nothing in play and "bed110cs_tainted_mirror.cut" played (2026-10-01).
    auto name = args.String("resource");
    if (name.find('.') == std::string::npos)
    {
        name += ".cut";
    }
    api::LoadCutscene(dao::Resource{Wide(name)}, hero, 1, dao::script::Array::Empty(dao::core::ArrayTag::String),
                      dao::script::Array::Empty(dao::core::ArrayTag::Object), 0);
    answer["resource"] = name;
    answer["note"] = "the game's mode is cutscene from the next frame if there is such a resource (health says; "
                     "wait_for eluvian.mode.changed from `cursor`); dialogue action=skip_cutscene ends it as "
                     "Escape does. Nothing happens for a name the game has not";
    return hero;
}

ObjectId StageSpawn(const Args &args, Json &answer)
{
    const auto name = args.String("template");
    const auto point = VectorOf(args.Raw().value("point", Json()), args.Tool() + ": `point`");
    const ObjectId around = api::GetMainControlled();
    if (around == kInvalidObject)
    {
        throw ToolError(args.Tool() + ": nobody is controlled, so there is no area to spawn in");
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
        // Names that made creatures in play, for a tester guessing at one:
        // "ogre", "hurlock", "genlock" and "bandit" made none (2026-10-02).
        throw ToolError(std::format("{}: no creature from template \"{}\"; a template is the game's creature "
                                    "resource, not a kind's name: wolf, bear_black and skeleton_normal make "
                                    "creatures, and creatures gives each one's `template` in an "
                                    "area where its kind lives (its tag names the instance, not this)",
                                    args.Tool(), name));
    }
    // The game puts a creature at the nearest place it can stand: a point
    // inside a wall gave one 5 m off in the next room, with no word of it
    // (computer-use-agent, 2026-10-01). `moved` says how far from the point
    // asked for it was made.
    const float moved = Distance(point, api::GetPosition(id));
    answer["asked"] = VectorJson(point);
    answer["moved"] = moved;
    // Its group at once: a spawned ogre_a stood neutral until a second
    // call moved it (turn-based-agent's survey, 2026-10-06, #92).
    if (args.Has("group"))
    {
        PutInGroup(id, static_cast<std::int32_t>(args.Integer("group", 0, 0, 255)), answer);
    }
    // Read in the frame it was made, a spawned creature is not set up yet:
    // health 1 of 1 in the answer, 85 of 85 three seconds on (2026-09-30).
    answer["note"] = moved > 1.0F
                         ? "the point asked for is not a place to stand: the creature was made `moved` metres from "
                           "it, perhaps in another room (see `creature`'s position and room). It is set up over "
                           "the next frames; creature reads its health"
                         : "a creature spawned is set up over the next frames; creature reads its health";
    return id;
}

// A condition put on or taken off: one concept, two acts.
ObjectId StageEffects(const Args &args, Json &answer)
{
    return args.Choice("action", {"apply", "remove"}) == "apply" ? StageEffect(args, answer)
                                                                 : StageRemoveEffects(args, answer);
}

// A creature's `id`, as every stage tool on one takes it.
Json IdProperty()
{
    return Json{{"type", "string"}, {"description", "The creature: an id from creatures"}};
}

} // namespace

void AddStage(Bench &bench, const Watch &watch)
{
    const auto add = [&bench, &watch](std::string name, std::string description, Json properties,
                                      const std::vector<std::string> &required, StageAction act) {
        ToolSpec spec = Spec(name, std::move(description), std::move(properties), required);
        bench.Tools().Add(
            std::move(spec),
            [&bench, &watch, name = std::move(name), act = std::move(act)](const Json &raw) -> ToolResult {
                const Args args(raw, name);
                RequireWorld(watch, name);
                Json answer = {{"cursor", bench.Events().Next()}};
                const ObjectId id = act(args, answer);
                // The creature as the change leaves it, in every answer (a
                // tester reading `creature.dead` after a kill found no field,
                // 2026-09-30).
                answer["creature"] = Summary(id, api::GetHero(), PartyOf(api::GetHero()));
                return answer;
            });
    };
    add("stage_effect",
        "Put a condition on a creature, or take its effects off, for a test's case. `action`: apply puts "
        "`effect` (stun, knockdown, paralyze, root, sleep, daze, slip, fear, confusion, grabbed, or an "
        "EFFECT_TYPE_* number) on creature `id` for `seconds` (default 6), as from `creator` and `ability` if "
        "given; remove takes off its effects of `effect`, `ability` or `creator`, or all with none given. "
        "Answers the `effects` on it after and `creature` as it is. The effects are the engine's bare ones, "
        "applied as scripts apply them.",
        Json{{"action", {{"type", "string"}, {"enum", {"apply", "remove"}}}},
             {"id", IdProperty()},
             {"effect", {{"description", "A condition's name or an EFFECT_TYPE_* number"}}},
             {"seconds", {{"type", "number"}, {"description", "apply: how long, default 6"}}},
             {"creator", {{"type", "string"}, {"description", "Whom the effect is from; default none"}}},
             {"ability", {{"type", "integer"}, {"description", "The ability it is from; default none"}}}},
        {"action", "id"}, StageEffects);
    add("stage_health",
        "Set creature `id`'s current health to `value`, for a test's case. Answers `creature` as it is after; "
        "stage_kill for a death.",
        Json{{"id", IdProperty()}, {"value", {{"type", "number"}, {"description", "The health, 0 or more"}}}},
        {"id", "value"}, StageHealth);
    add("stage_cooldown",
        "Set creature `id`'s cooldown on `ability` to `seconds` left (0 makes it ready), of `total` if given. "
        "creature lists its abilities.",
        Json{{"id", IdProperty()},
             {"ability", {{"type", "integer"}, {"description", "The ability's id"}}},
             {"seconds", {{"type", "number"}, {"description", "The time left, 0 (ready) to 3600"}}},
             {"total", {{"type", "number"}, {"description", "The whole it counts down from"}}}},
        {"id", "ability", "seconds"}, StageCooldown);
    add("stage_kill",
        "Deal creature `id` more damage than it has. It dies a frame or more later, as eluvian.creature.died: "
        "wait_for that from `cursor`.",
        Json{{"id", IdProperty()}}, {"id"}, StageKill);
    add("stage_group",
        "Move creature `id` to `group` (0 the player's, 1 the hostile one, 2 friendly: the game keeps which pairs "
        "of groups fight), and with `toward` set whether `group` and that group are `hostile` first. A spawned "
        "creature moved to group 2 fights the hostile ones near it, for a fight the party is not in.",
        Json{{"id", IdProperty()},
             {"group", {{"type", "integer"}, {"description", "The group to put it in"}}},
             {"toward", {{"type", "integer"}, {"description", "Another group, to set hostility with"}}},
             {"hostile", {{"type", "boolean"}, {"description", "With `toward`; default true"}}}},
        {"id", "group"}, StageGroup);
    add("stage_spawn",
        "Make a creature from `template` (as creatures gives each one's `template`) at `point`, facing `facing` "
        "degrees, in the controlled creature's area. `moved` says how far from the point the game put it (a point "
        "inside a wall is not kept); it is set up over the next frames. `group` puts it in that group at once, as "
        "stage_group does (`group`, `hostileToParty` say where it stands). Templates: resources with type utc "
        "and a word (ogre_default, hurlock_emissary_off, hurlock_epic_archer).",
        Json{{"template", {{"type", "string"}, {"description", "The creature's template"}}},
             {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
             {"facing", {{"type", "number"}, {"description", "Degrees; default 0"}}},
             {"group", {{"type", "integer"}, {"description", "The group to put it in"}}}},
        {"template", "point"}, StageSpawn);
    add("stage_equip",
        "Make an item from `template` (an item's resource: resources with type uti) on creature `id` and put it "
        "on, in its own slot or `slot`; `equip` false leaves it in the backpack. Answers the `item` (item gives "
        "it in full) and whether it is `equipped`. equip moves what the party already has.",
        Json{{"id", IdProperty()},
             {"template", {{"type", "string"}, {"description", "The item's resource"}}},
             {"slot", {{"type", "string"}, {"description", "main, offhand, chest, head ..."}}},
             {"equip", {{"type", "boolean"}, {"description", "Put it on; default true"}}}},
        {"id", "template"}, StageEquip);
    add("stage_reset",
        "Put things back as before a fight, for another in the same launch: the party (or `ids`) raised if "
        "fallen, health and mana or stamina full, every cooldown cleared, with `effects` every effect taken off "
        "too; and unless `clearDead` is false, the dead that died since the last reset (not the area's older "
        "bodies) taken away, if they are not the party's. Answers each creature as it is after (`reset`) and "
        "what was taken away (`removed`).",
        Json{{"ids", {{"type", "array"}, {"description", "The creatures; default the party"}}},
             {"effects", {{"type", "boolean"}, {"description", "Take every effect off too"}}},
             {"clearDead", {{"type", "boolean"}, {"description", "Take the dead away; default true"}}}},
        {}, StageReset);
    add("stage_cutscene",
        "Play the cutscene `resource` (its name as the scripts give it, \"bed110cs_tainted_mirror.cut\"), as the "
        "scripts' LoadCutscene does. The game's mode is cutscene until it ends or is skipped (dialogue "
        "action=skip_cutscene).",
        Json{{"resource", {{"type", "string"}, {"description", "The cutscene's name"}}}}, {"resource"}, StageCutscene);
}

} // namespace eluvian::game
