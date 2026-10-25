// The reads of who a creature is and what it carries: `character`
// (the sheet: race, class, level, attributes, what it wears), `equipment`
// (each slot's item in full), `inventory` (the backpack) and `item` (one
// item in full). Read through the scripts' own commands and the game's rule
// tables, whose labels name a race, a class, a base item, a material and an
// item's properties (seen in play, 2026-10-01).
#include "game/Shared.h"

#include "core/Requirements.h"

#include "dao/Data.h"

#include <cctype>

namespace eluvian::game
{

namespace
{

struct Named
{
    std::int32_t id;
    std::string_view name;
};

// The equipment slots (INVENTORY_SLOT_*), by the names a tester writes.
constexpr std::array<Named, 19> kSlots = {{{0, "main"},
                                           {1, "offhand"},
                                           {2, "ammo"},
                                           {4, "chest"},
                                           {5, "head"},
                                           {6, "boots"},
                                           {7, "gloves"},
                                           {8, "cloak"},
                                           {9, "ring1"},
                                           {10, "ring2"},
                                           {11, "neck"},
                                           {12, "belt"},
                                           {13, "bite"},
                                           {14, "shale_shoulders"},
                                           {15, "shale_chest"},
                                           {16, "shale_right_arm"},
                                           {17, "shale_left_arm"},
                                           {18, "dog_warpaint"},
                                           {19, "dog_collar"}}};

// An item's stats by their place (GetItemStat), named by the game's own
// table of them (itemstatdefines, table 109: Armor, Damage, Defense,
// Attack, CritChanceModifier, ArmorPenetration, AbilityCostModifier,
// MissileDeflection, OptimumRange, Spellpower, RngdPenalty, AttributeMod;
// lib-agent, 2026-10-01). AbilityCostModifier is what the game's interface
// calls fatigue. The scripts' header names two of them otherwise; the
// table is what the interface reads.
constexpr std::int32_t kTableItemStatDefines = 109;

std::string StatName(std::int32_t index)
{
    std::string label = Narrow(api::GetM2DAString(kTableItemStatDefines, L"Label", index));
    if (label.empty())
    {
        return std::format("stat{}", index);
    }
    if (label == "AbilityCostModifier")
    {
        return "fatigue";
    }
    label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
    return label;
}

std::string_view SlotName(std::int32_t slot)
{
    for (const auto &named : kSlots)
    {
        if (named.id == slot)
        {
            return named.name;
        }
    }
    return {};
}

} // namespace

// A slot's name, as inventory names it: for the equipment events
// (#57), which live in GameTools.
std::string_view EquipSlotName(std::int32_t slot)
{
    return SlotName(slot);
}

// An item: in brief for a list, in full for itself. Shared: the loot panel
// lists its container's items as inventory does.
Json ItemJson(ObjectId item, bool full)
{
    if (item == kInvalidObject)
    {
        return nullptr;
    }
    Json json = Brief(item);
    json["template"] = Narrow(api::GetResRef(item));
    json["baseType"] = Rule(k::TABLE_ITEMS, api::GetBaseItemType(item));
    json["stack"] = api::GetItemStackSize(item);
    json["value"] = api::GetItemValue(item);
    json["plot"] = api::IsPlot(item) != 0;
    if (!full)
    {
        return json;
    }
    json["maxStack"] = api::GetMaxItemStackSize(item);
    json["material"] = Rule(k::TABLE_MATERIAL_TYPES, api::GetItemMaterialType(item));
    const auto slot = api::GetItemEquipSlot(item);
    json["equippedIn"] = SlotName(slot).empty() ? Json(nullptr) : Json(std::string(SlotName(slot)));
    json["droppable"] = api::IsItemDroppable(item) != 0;
    json["indestructible"] = api::IsItemIndestructible(item) != 0;
    Json stats = Json::object();
    for (std::int32_t index = 0; index < 16; ++index)
    {
        if (const float value = api::GetItemStat(item, index); value != 0)
        {
            stats[StatName(index)] = value;
        }
    }
    json["stats"] = std::move(stats);
    Json properties = Json::array();
    const auto list = api::GetItemProperties(item, 1);
    for (std::size_t i = 0; i < list.Size(); ++i)
    {
        const std::int32_t id = list.Int(i);
        properties.push_back(Json{{"id", id},
                                  {"label", Narrow(api::GetM2DAString(k::TABLE_ITEMPRPS, L"Label", id))},
                                  {"power", api::GetItemPropertyPower(item, id, 1)}});
    }
    json["properties"] = std::move(properties);
    // 200000 is the game's "no ability" for an item; anything else is what
    // using the item does (a poultice's heal).
    if (const auto ability = api::GetItemAbilityId(item); ability != 0 && ability != 200000)
    {
        json["ability"] = AbilityJson(ability);
    }
    return json;
}

namespace
{

ObjectId ItemInSlot(ObjectId creature, std::int32_t slot, std::int32_t set)
{
    return api::GetItemInEquipSlot(slot, creature, set);
}

// What a creature wears: each slot that holds something. The first three
// slots (main, offhand, ammo) are the active weapon set's; `otherWeapons`
// is the set not in hand.
Json EquipmentJson(ObjectId creature, bool full)
{
    const auto set = api::GetActiveWeaponSet(creature);
    Json slots = Json::object();
    for (const auto &named : kSlots)
    {
        if (const ObjectId item = ItemInSlot(creature, named.id, named.id <= 2 ? set : -1); item != kInvalidObject)
        {
            slots[std::string(named.name)] = ItemJson(item, full);
        }
    }
    Json other = Json::object();
    for (std::int32_t slot = 0; slot <= 2; ++slot)
    {
        if (const ObjectId item = ItemInSlot(creature, slot, set == 0 ? 1 : 0); item != kInvalidObject)
        {
            other[std::string(SlotName(slot))] = ItemJson(item, full);
        }
    }
    return Json{{"weaponSet", set}, {"slots", std::move(slots)}, {"otherWeapons", std::move(other)}};
}

Json Stat(ObjectId creature, std::int32_t property)
{
    return Json{{"total", api::GetCreatureProperty(creature, property, k::PROPERTY_VALUE_TOTAL)},
                {"base", api::GetCreatureProperty(creature, property, k::PROPERTY_VALUE_BASE)}};
}

ObjectId CreatureOrControlled(const Args &args)
{
    const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
    if (id == kInvalidObject)
    {
        throw ToolError(args.Tool() + ": no creature is controlled and no `id` was given; is a game loaded?");
    }
    // An id gone is told from one that is no creature: a driver asked 43
    // times for a creature of another campaign's, read "not a creature",
    // and went on (eluvian.log, 2026-10-09, #110).
    if (api::IsObjectValid(id) == 0)
    {
        throw ToolError(std::format("{}: there is no object {} now: it may have died and been taken away, or be "
                                    "another area's or another game's; creatures lists those here",
                                    args.Tool(), FormatId(id)));
    }
    if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("{}: {} is not a creature; creatures lists them", args.Tool(), FormatId(id)));
    }
    return id;
}

// A creature's properties by their rows in the game's `properties` table
// (its Stat column, read in play 2026-10-01). script.ldf names some of them
// otherwise and wrongly for this build: its MOVEMENT_SPEED_MODIFIER (20) is
// the table's Flanking_Angle and its MAGIC_CRIT_MODIFIER (22) the table's
// Missile_Shield; the table is what the game reads. The class is row 27
// (the hero read 3, the rogue's row; a warrior companion 1).
constexpr std::int32_t kPropertyClass = 27;
constexpr std::array<Named, 14> kCombat = {{{9, "attack"},
                                            {10, "defense"},
                                            {11, "armor"},
                                            {53, "armorPenetration"},
                                            {13, "spellpower"},
                                            {50, "damageMainHand"},
                                            {49, "damageOffHand"},
                                            {41, "fatigue"},
                                            {21, "meleeCritModifier"},
                                            {23, "rangedCritModifier"},
                                            {31, "attackSpeedModifier"},
                                            {20, "flankingAngle"},
                                            {22, "missileShield"},
                                            {16, "displacement"}}};
// What is left to spend at a level-up.
constexpr std::array<Named, 4> kPoints = {
    {{34, "attributes"}, {35, "skills"}, {36, "talentsOrSpells"}, {38, "specializations"}}};

} // namespace

static void AddEquip(Bench &bench, const Watch &watch);
static void AddGift(Bench &bench, const Watch &watch);

namespace
{

// The character kind's handler, registered by AddCharacter.
ToolResult KindCharacter(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "character");
    const Args args(raw, "character");
    const ObjectId id = CreatureOrControlled(args);
    Json sheet = Brief(id);
    sheet["template"] = Narrow(api::GetResRef(id));
    sheet["race"] = Rule(k::TABLE_RULES_RACES, api::GetCreatureRacialType(id));
    const auto gender = api::GetCreatureGender(id);
    sheet["gender"] = Label(gender, {{k::GENDER_MALE, "male"}, {k::GENDER_FEMALE, "female"}});
    sheet["class"] =
        Rule(k::TABLE_RULES_CLASSES,
             static_cast<std::int32_t>(api::GetCreatureProperty(id, kPropertyClass, k::PROPERTY_VALUE_TOTAL)));
    sheet["level"] =
        static_cast<std::int32_t>(api::GetCreatureProperty(id, k::PROPERTY_SIMPLE_LEVEL, k::PROPERTY_VALUE_TOTAL));
    sheet["experience"] =
        static_cast<std::int32_t>(api::GetCreatureProperty(id, k::PROPERTY_SIMPLE_EXPERIENCE, k::PROPERTY_VALUE_TOTAL));
    sheet["canLevelUp"] = api::GetCanLevelUp(id) != 0;
    sheet["rank"] = Rule(k::TABLE_CREATURERANKS, api::GetCreatureRank(id));
    // A member of the party, the hero among them (IsFollower is true
    // of the hero too, in play 2026-10-01).
    if (api::IsFollower(id) != 0)
    {
        const auto state = api::GetFollowerState(id);
        sheet["follower"] = Json{{"state", Label(state, {{k::FOLLOWER_STATE_ACTIVE, "active"},
                                                         {k::FOLLOWER_STATE_AVAILABLE, "available"},
                                                         {k::FOLLOWER_STATE_UNAVAILABLE, "unavailable"},
                                                         {k::FOLLOWER_STATE_SUSPENDED, "suspended"},
                                                         {k::FOLLOWER_STATE_LOADING, "loading"},
                                                         {k::FOLLOWER_STATE_LOCKEDACTIVE, "lockedActive"}})},
                                 {"approval", api::GetFollowerApproval(id)},
                                 {"locked", api::IsFollowerLocked(id) != 0},
                                 {"aiEnabled", api::IsPartyAIEnabled(id) != 0}};
    }
    sheet["attributes"] = Json{{"strength", Stat(id, k::PROPERTY_ATTRIBUTE_STRENGTH)},
                               {"dexterity", Stat(id, k::PROPERTY_ATTRIBUTE_DEXTERITY)},
                               {"willpower", Stat(id, k::PROPERTY_ATTRIBUTE_WILLPOWER)},
                               {"magic", Stat(id, k::PROPERTY_ATTRIBUTE_MAGIC)},
                               {"cunning", Stat(id, k::PROPERTY_ATTRIBUTE_INTELLIGENCE)},
                               {"constitution", Stat(id, k::PROPERTY_ATTRIBUTE_CONSTITUTION)}};
    Json combat = Json::object();
    for (const auto &named : kCombat)
    {
        combat[std::string(named.name)] = api::GetCreatureProperty(id, named.id, k::PROPERTY_VALUE_TOTAL);
    }
    sheet["combat"] = std::move(combat);
    Json points = Json::object();
    for (const auto &named : kPoints)
    {
        points[std::string(named.name)] =
            static_cast<std::int32_t>(api::GetCreatureProperty(id, named.id, k::PROPERTY_VALUE_TOTAL));
    }
    sheet["pointsToSpend"] = std::move(points);
    sheet["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
    sheet["manaStamina"] = Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA);
    const auto style = api::GetWeaponStyle(id);
    sheet["weaponStyle"] = Label(style, {{k::WEAPONSTYLE_NONE, "none"},
                                         {k::WEAPONSTYLE_SINGLE, "single"},
                                         {k::WEAPONSTYLE_DUAL, "dual"},
                                         {k::WEAPONSTYLE_TWOHANDED, "two-handed"}});
    sheet["plot"] = api::IsPlot(id) != 0;
    sheet["immortal"] = api::IsImmortal(id) != 0;
    sheet["abilityCount"] = api::GetAbilityCount(id);
    const ObjectId hero = api::GetHero();
    sheet["side"] = Side(id, hero, PartyOf(hero));
    // Every property that is not nothing, by the name the game's own
    // table gives it (the `properties` table's Stat column; lib-agent,
    // 2026-10-01): resistances, regeneration, damage bonuses and the
    // rest, beyond the named ones above.
    Json properties = Json::object();
    for (std::int32_t property = 1; property < 60; ++property) // the table's rows are 0 to 59
    {
        const float total = api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_TOTAL);
        if (total == 0)
        {
            continue;
        }
        std::string name = Narrow(api::GetM2DAString(-1, L"Stat", property, L"properties"));
        if (name.empty())
        {
            name = std::format("property{}", property);
        }
        properties[name] =
            Json{{"total", total}, {"base", api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_BASE)}};
    }
    sheet["properties"] = std::move(properties);
    sheet["equipment"] = EquipmentJson(id, false);
    return sheet;
}

// The items a creature carries that give an ability, by the ability (the
// engine's GetItemAbilityId, as item reads it): each item's id, tag,
// name and stack, and how many in all. A driver joined abilities'
// items to inventory by the item's name to find a poultice it could
// drink, and told health from mana by the tag's prefix (turn-based-agent's
// survey of its driver, 2026-10-06, #87).
struct Giving
{
    Json items = Json::array();
    std::int32_t held = 0;
};

std::map<std::int32_t, Giving> ItemsGiving(ObjectId id)
{
    std::map<std::int32_t, Giving> giving;
    const auto items = api::GetItemsInInventory(id, k::GET_ITEMS_OPTION_ALL);
    for (std::size_t i = 0; i < items.Size(); ++i)
    {
        const ObjectId item = items.Object(i);
        const auto ability = api::GetItemAbilityId(item);
        if (ability == 0 || ability == 200000) // the game's "no ability"
        {
            continue;
        }
        const auto stack = api::GetItemStackSize(item);
        auto &entry = giving[ability];
        entry.items.push_back(Json{{"id", FormatId(item)},
                                   {"tag", Narrow(api::GetTag(item))},
                                   {"name", Narrow(api::GetName(item))},
                                   {"stack", stack}});
        entry.held += stack;
    }
    return giving;
}

// An item's ability's cooldown, from what is running on the creature:
// the record of an item's ability is only in the cooldown manager's
// ticking list, which GetCooldown does not read, where a talent's is in
// both (DAOLib #38, lib-agent, 2026-10-08). A driver wanted to know when a
// held poultice comes back (turn-based-agent, 2026-10-08, #97). Nothing
// when none is running: the item is ready.
std::optional<dao::abilities::Cooldown> ItemCooldown(const std::vector<dao::abilities::RunningCooldown> &running,
                                                     std::int32_t ability)
{
    for (const auto &each : running)
    {
        if (each.ability == ability)
        {
            return each.cooldown;
        }
    }
    return std::nullopt;
}

// The abilities kind's handler, registered by AddCharacter.
ToolResult KindAbilities(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "abilities");
    const Args args(raw, "abilities");
    const ObjectId id = CreatureOrControlled(args);
    const auto giving = ItemsGiving(id);
    const auto running = dao::abilities::Cooldowns(id);
    const auto list = [id, &giving, &running](dao::abilities::Kind kind) {
        Json abilities = Json::array();
        for (const std::int32_t ability : dao::abilities::Known(id, kind))
        {
            Json entry = AbilityJson(ability);
            const auto it = giving.find(ability);
            if (it != giving.end())
            {
                entry["items"] = it->second.items;
                entry["held"] = it->second.held;
            }
            const bool available = api::IsAbilityAvailable(id, ability) != 0;
            const auto cooldown = kind == dao::abilities::Kind::Item || it != giving.end()
                                      ? ItemCooldown(running, ability)
                                      : dao::abilities::GetCooldown(id, ability);
            UseInto(entry, id, ability, available && (!cooldown || cooldown->remaining <= 0));
            if (cooldown && cooldown->remaining > 0)
            {
                entry["cooldown"] = Json{{"remaining", cooldown->remaining}, {"total", cooldown->total}};
            }
            abilities.push_back(std::move(entry));
        }
        return abilities;
    };
    return Json{{"of", Brief(id)},
                {"count", api::GetAbilityCount(id)},
                {"talents", list(dao::abilities::Kind::Talent)},
                {"spells", list(dao::abilities::Kind::Spell)},
                {"skills", list(dao::abilities::Kind::Skill)},
                {"items", list(dao::abilities::Kind::Item)}};
}

// The tactics kind's handler, registered by AddCharacter.
ToolResult KindTactics(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "tactics");
    const Args args(raw, "tactics");
    const ObjectId id = CreatureOrControlled(args);
    constexpr std::int32_t kMostSlots = 64;
    constexpr std::int32_t kCommandUseAbility = 4;
    constexpr std::int32_t kCommandUseItem = 7;
    constexpr std::int32_t kPropertyBehaviour = 18;
    const std::int32_t slots = api::GetNumTactics(id);
    Json tactics = Json::array();
    for (std::int32_t slot = 1; slot <= slots && slot <= kMostSlots; ++slot)
    {
        Json entry = {{"slot", slot}, {"enabled", api::IsTacticEnabled(id, slot) != 0}};
        const std::int32_t command = api::GetTacticCommand(id, slot);
        if (command == 0)
        {
            entry["empty"] = true;
            tactics.push_back(std::move(entry));
            continue;
        }
        entry["target"] = Rule(k::TABLE_AI_TACTICS_TARGET_TYPE, api::GetTacticTargetType(id, slot));
        entry["condition"] = Rule(k::TABLE_TACTICS_CONDITIONS, api::GetTacticCondition(id, slot));
        entry["command"] = Rule(k::TABLE_COMMAND_TYPES, command);
        const std::int32_t param = api::GetTacticCommandParam(id, slot);
        if (command >= kCommandUseAbility && command <= kCommandUseItem && param != 0)
        {
            entry["ability"] = AbilityJson(param);
        }
        else if (param != 0)
        {
            entry["param"] = param;
        }
        if (auto tag = Narrow(api::GetTacticCommandItemTag(id, slot)); !tag.empty())
        {
            entry["item"] = std::move(tag);
        }
        if (const ObjectId object = api::GetTacticTargetObject(id, slot); api::IsObjectValid(object) != 0)
        {
            entry["targetObject"] = Brief(object);
        }
        if (const ObjectId object = api::GetTacticConditionObject(id, slot); api::IsObjectValid(object) != 0)
        {
            entry["conditionObject"] = Brief(object);
        }
        tactics.push_back(std::move(entry));
    }
    const auto behaviour =
        static_cast<std::int32_t>(api::GetCreatureProperty(id, kPropertyBehaviour, k::PROPERTY_VALUE_TOTAL));
    // Preset 0 is the table's INVALID row: none chosen, the slots set by
    // hand or by the creature's template (a follower read 0 in play).
    const std::int32_t preset = api::GetTacticPresetID(id);
    return Json{{"of", Brief(id)},
                {"slots", slots},
                {"preset", preset > 0 ? Rule(k::TABLE_TACTICS_USER_PRESETS, preset) : Json(nullptr)},
                {"behaviour", Rule(k::TABLE_AI_BEHAVIORS, behaviour)},
                {"tactics", std::move(tactics)}};
}

// The equipment kind's handler, registered by AddCharacter.
ToolResult KindEquipment(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "equipment");
    const Args args(raw, "equipment");
    const ObjectId id = CreatureOrControlled(args);
    Json answer = EquipmentJson(id, true);
    answer["of"] = Brief(id);
    return answer;
}

// The inventory kind's handler, registered by AddCharacter.
ToolResult KindInventory(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "inventory");
    const Args args(raw, "inventory");
    const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetHero());
    if (id == kInvalidObject)
    {
        throw ToolError("inventory: no hero and no `id` was given; is a game loaded?");
    }
    const bool full = args.Bool("full", false);
    Json items = Json::array();
    const auto list = api::GetItemsInInventory(id, k::GET_ITEMS_OPTION_BACKPACK);
    for (std::size_t i = 0; i < list.Size(); ++i)
    {
        items.push_back(ItemJson(list.Object(i), full));
    }
    // A container answers the same list (chests, cocoons and piles of
    // bones did in play, 2026-10-01), with no size or purse of its own.
    Json answer = {{"of", Brief(id)}, {"count", items.size()}};
    if (api::GetObjectType(id) == k::OBJECT_TYPE_CREATURE)
    {
        answer["holds"] = api::GetMaxInventorySize(id);
        answer["money"] = api::GetCreatureMoney(id);
    }
    answer["items"] = std::move(items);
    return answer;
}

// The item kind's handler, registered by AddCharacter.
ToolResult KindItem(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "item");
    const Args args(raw, "item");
    const ObjectId id = args.Id("id", "an item, as inventory lists them");
    if (api::GetObjectType(id) != k::OBJECT_TYPE_ITEM)
    {
        throw ToolError(std::format("item: {} is not an item; inventory and "
                                    "equipment list items",
                                    FormatId(id)));
    }
    return ItemJson(id, true);
}

} // namespace

ToolResult ReadTactics(const Watch &watch, const Json &raw)
{
    return KindTactics(watch, raw);
}

void AddCharacter(Bench &bench, const Watch &watch)
{
    AddEquip(bench, watch);
    AddGift(bench, watch);
    bench.Tools().Add(
        Read("character",
             "Reads a creature's character sheet. It returns:\n"
             "- race, gender, class, level, experience, rank, and `canLevelUp` with the `pointsToSpend`.\n"
             "- the six `attributes`, each with total and base.\n"
             "- `combat` numbers: attack, defense, armor, armor penetration, spellpower, weapon damage, "
             "fatigue and more.\n"
             "- `health` and `manaStamina`, weapon style, and whether it is `plot` or `immortal`.\n"
             "- for a party member, `follower`: its state, approval, whether it is locked in the party, and "
             "whether its own AI is on.\n"
             "- `properties`: every property that is not zero, by the game's own names (resistances, "
             "regeneration, damage bonuses and so on).\n"
             "- `equipment`: what it wears, slot by slot, in brief.\n\n"
             "For more, call `equipment` (worn items in full), `inventory` (the backpack) or `creature` "
             "(effects and abilities).",
             Json{{"id",
                   {{"type", "string"},
                    {"description", "Who to read, by creature id. Default: the creature you control."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindCharacter(watch, raw); });

    bench.Tools().Add(
        Read("abilities",
             "Lists every ability a creature has, not only those on its quickbar: its `talents`, `spells`, "
             "`skills`, and the abilities of its `items`. A skill with ranks has one entry per rank. `count` "
             "is the game's own total.\n\n"
             "Each entry has the ability's id and name, and:\n"
             "- `ready`: whether it can be used now. This is the same check the quickbar uses to gray out a "
             "button (mana or stamina, cooldown, anything stopping it). When false, `why` gives the reason, "
             "such as \"Insufficient stamina\". For a sustained ability that is switched on, the check includes its "
             "ongoing cost.\n"
             "- `cost`: its mana or stamina cost, including fatigue.\n"
             "- `cooldown`: while one runs, the seconds `remaining` of the `total`. This includes an item's "
             "ability, such as a poultice already drunk.\n"
             "- for an item's ability: the `items` that give it (id, tag, name, stack) and how many are "
             "`held`. Use one with `order` action=ability and `item` or `itemTag`.",
             Json{{"id",
                   {{"type", "string"},
                    {"description", "Who to read, by creature id. Default: the creature you control."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindAbilities(watch, raw); });

    // A creature's tactics, by the scripts' own commands. Slots count from 1
    // and an unset one has command 0 (read in play 2026-10-01: the hero's
    // first was Self, condition 101, Use Ability 603; a follower's Enemy,
    // condition 208, Use Ability 803). The tables name each part by row:
    // targets, conditions, command types (4 use ability, 5 and 6 a mode on
    // and off, 7 use item, whose param is the ability), presets, and the
    // behaviours that property 18 (AI_BEHAVIOR) is a row of.
    bench.Tools().Add(
        Read("tactics.read",
             "A creature's tactics (`id`, default the one controlled): what it does by itself in a fight "
             "when it is not the one controlled. `slots` is how many it has; `tactics` is each in order: "
             "`enabled`, its `target` (self, ally, enemy ...), its `condition`, its `command` (attack, use "
             "ability, activate mode, use item ...) and the `ability` or `item` the command is for; a slot "
             "not set is `empty`. `preset` (null when none is chosen) and `behaviour` are the two choices above "
             "the slots.",
             Json{{"id", {{"type", "string"}, {"description", "The creature; default the one controlled"}}}}, {},
             false),
        [&watch](const Json &raw) -> ToolResult { return KindTactics(watch, raw); });

    bench.Tools().Add(
        Read("equipment",
             "Lists what a creature wears, with each item in full, as `item` returns it: name, tag, "
             "template, base type, material, value, stats (armor, damage, defense, attack, crit chance, armor "
             "penetration, fatigue, optimum range and more, by the game's own names), properties with their "
             "power, and what using it does.\n\n"
             "`slots` holds main, offhand and ammo for the weapon set in hand, then chest, head, boots, "
             "gloves, cloak, ring1, ring2, neck and belt. `weaponSet` is the set in hand, and "
             "`otherWeapons` is the other set.",
             Json{{"id",
                   {{"type", "string"},
                    {"description", "Who to read, by creature id. Default: the creature you control."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindEquipment(watch, raw); });

    bench.Tools().Add(
        Read("inventory",
             "Lists the items in a creature's backpack, or in a container without opening it (a chest, a "
             "body, a pile of bones). The hero's backpack is the party's. Worn items are not included; "
             "`equipment` lists those.\n\n"
             "The answer has `count` and `items`, each in brief (name, tag, template, base type, stack, "
             "value, plot). For a creature it also has `holds` (the most the backpack can hold) and "
             "`money` (the party's).\n\n"
             "A container's contents may only be decided when it is opened, so an empty list does not mean "
             "it is empty. The `loot` tool opens a container and takes what is inside.",
             Json{{"id",
                   {{"type", "string"},
                    {"description", "The creature or container, by id (containers come from `placeables`). "
                                    "Default: the hero."}}},
                  {"full",
                   {{"type", "boolean"}, {"description", "Return each item in full, as `item` does. Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindInventory(watch, raw); });

    bench.Tools().Add(
        Read(
            "item",
            "Reads one item in full: name, tag, template, base type, material, `stack` and `maxStack`, value, "
            "the slot it is worn in (`equippedIn`), its stats, its properties with their power, what using "
            "it does (`ability`), and whether it is plot, droppable or indestructible.",
            Json{{"id", {{"type", "string"}, {"description", "The item, by its id from `inventory` or `equipment`."}}}},
            {"id"}),
        [&watch](const Json &raw) -> ToolResult { return KindItem(watch, raw); });
}

// `equip`: put an item of the pack on, or take one off, as the inventory
// screen does, so the game's rules hold. A playthrough driven by MCP alone
// had only `stage equip`, which makes a new item, and the scripts'
// EquipItem, which checks nothing (turn-based-agent, 2026-10-03, #18).
//
// Read by disassembly from inventory.gfx, 2026-10-03: the paperdoll's
// radial menu sends "GUI.SelectedChar.EquipItem <item> <slot>", the slot
// one of the item's EquippableSlots (the base item table's mask: a
// longsword's main or offhand, a ring's either finger), and
// "GUI.SelectedChar.UnequipItem <item>"; both reach the creature the
// interface has selected. The engine's EquipItemMessage and
// UnequipItemMessage are CCCreature's handlers (DAOLib #253), so since #174
// they are sent to the creature itself, typed, and no party member need be
// selected first, which a selection at camp once failed to do (#31).
namespace
{

// The creature's own object, which the equip messages are sent to.
dao::CCCreature &CreatureObject(ObjectId who)
{
    auto *app = dao::CClientExoApp::Get();
    auto *creature = app != nullptr ? app->GetCreatureById(who) : nullptr;
    if (creature == nullptr)
    {
        throw ToolError(std::format("equip: {} is no creature in the area; state lists the party", FormatId(who)));
    }
    return *creature;
}

// Taken off as the paperdoll does: back to the pack.
Json Unequip(ObjectId who, ObjectId item, Json answer)
{
    const auto slot = api::GetItemEquipSlot(item);
    if (SlotName(slot).empty())
    {
        throw ToolError("equip: that item is not worn; equipment says what is");
    }
    auto message = dao::messages::Make<dao::messages::UnequipItemMessage>();
    message.item = item;
    if (!dao::messages::Send(&CreatureObject(who), message))
    {
        throw ToolError("equip: the game took no unequip");
    }
    answer["slot"] = std::string(SlotName(slot));
    answer["unequipped"] = SlotName(api::GetItemEquipSlot(item)).empty();
    return answer;
}

// An item refused a slot it does not go in: the ones it fits named.
[[noreturn]] void NotThere(std::int32_t mask)
{
    std::string fits;
    for (const auto &named : kSlots)
    {
        if (((mask >> named.id) & 1) != 0)
        {
            fits += (fits.empty() ? "" : ", ") + std::string(named.name);
        }
    }
    throw ToolError("equip: that item does not go there; it goes in: " +
                    (fits.empty() ? std::string("nothing (it is not worn)") : fits));
}

// The slot an item goes in: `slot` by number or name, or the item's first;
// refused, naming those it fits, where it goes in none of them.
std::int32_t SlotFor(const Args &args, std::int32_t mask)
{
    std::int32_t slot = -1;
    if (const auto &wanted = args.Raw().value("slot", Json()); wanted.is_number_integer())
    {
        slot = wanted.get<std::int32_t>();
    }
    else if (wanted.is_string())
    {
        for (const auto &named : kSlots)
        {
            if (named.name == wanted.get<std::string>())
            {
                slot = named.id;
            }
        }
    }
    else
    {
        for (std::int32_t bit = 0; bit < 31 && slot < 0; ++bit)
        {
            slot = ((mask >> bit) & 1) != 0 ? bit : -1;
        }
    }
    if (slot < 0 || slot > 30 || ((mask >> slot) & 1) == 0)
    {
        NotThere(mask);
    }
    return slot;
}

// And the lines themselves, as the inventory screen asks for
// them: a shown screen's DAExternalCommands.GetItemRequirements
// raises the engine's handler, which leaves its answer, one
// string, in the movie's fsc_retval before the movie splits
// it (lib-agent read the handler, 2026-10-05). That is the
// root's: the function's register 1 is _root (flags 0x6a),
// and DAExternalCommands.fsc_retval read null in play while
// the call answered the split list (turn-based-agent,
// 2026-10-05; "0;Requires: 36 strength;" at the root). Asked of
// the first screen shown that has DAExternalCommands: the
// quickbar is not shown while a picker, a conversation or a
// full screen is up (turn-based-agent, 2026-10-05).
std::optional<Json> Requirements(ObjectId item, ObjectId who)
{
    const std::array<dao::gui::Value, 2> ids{dao::gui::Value::Number(static_cast<double>(item)),
                                             dao::gui::Value::Number(static_cast<double>(who))};
    const auto said = ExternalCommand("GetItemRequirements", ids);
    if (!said)
    {
        return std::nullopt;
    }
    return ItemRequirements(said->first);
}

// An item's own restrictions are among its properties:
// Warden's Oath, "Restrict: player", was refused for a
// follower with no reason given (turn-based-agent, 2026-10-03,
// #31).
Json Restrictions(ObjectId item)
{
    Json restrictions = Json::array();
    for (const auto &property : ItemJson(item, true).value("properties", Json::array()))
    {
        const std::string label = property.value("label", std::string());
        if (label.find("Restrict") != std::string::npos || label.find("restrict") != std::string::npos)
        {
            restrictions.push_back(label);
        }
    }
    return restrictions;
}

// Why an item did not go on: the game's verdict, its requirement lines and
// the item's restrictions, said in `answer`.
void WhyNotOn(ObjectId item, ObjectId who, Json &answer)
{
    // The game's own verdict on the creature using it
    // (CanUseItem), which its inventory screen's requirement
    // lines follow: Alistair's Helm of the Legion was refused
    // with nothing said (turn-based-agent, 2026-10-05, #31). The
    // lines themselves (DAExternalCommands.GetItemRequirements)
    // come back as objects this GFx cannot hand over.
    const bool canUse = api::CanUseItem(who, item) != 0;
    answer["canUse"] = canUse;
    if (auto requirements = Requirements(item, who))
    {
        answer["requirements"] = std::move(*requirements);
    }
    answer["note"] = canUse ? "the game did not put it on, though it says the creature can use it: the "
                              "creature cannot change equipment now "
                              "(GUI.SelectedChar.CanChangeEquipment), or the slot is held"
                            : "the game says " + Narrow(api::GetName(who)) +
                                  " cannot use it (CanUseItem): a requirement not met, most often "
                                  "an attribute (strength for heavy armor), or a restriction; the "
                                  "inventory screen's tooltip names it";
    if (Json restrictions = Restrictions(item); !restrictions.empty())
    {
        answer["restrictions"] = std::move(restrictions);
    }
}

ToolResult ToolEquip(Bench &bench, const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "equip");
    const Args args(raw, "equip");
    const auto action = args.Choice("action", {"equip", "unequip"}, "equip");
    const ObjectId item = args.Id("item", "an item, as inventory and equipment list them");
    if (api::GetObjectType(item) != k::OBJECT_TYPE_ITEM)
    {
        throw ToolError(std::format("equip: {} is not an item; inventory lists the pack", FormatId(item)));
    }
    const auto selected = static_cast<ObjectId>(
        dao::data::Number(L"GUI.SelectedChar.ID").value_or(static_cast<double>(api::GetMainControlled())));
    const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(selected);
    const auto set = api::GetActiveWeaponSet(who);
    Json answer = {{"action", action},
                   {"item", ItemJson(item, false)},
                   {"creature", Brief(who)},
                   {"cursor", bench.Events().Next()}};
    if (action == "unequip")
    {
        return Unequip(who, item, std::move(answer));
    }
    const auto mask = api::GetM2DAInt(k::TABLE_ITEMS, L"EquippableSlots", api::GetBaseItemType(item));
    const std::int32_t slot = SlotFor(args, mask);
    const auto weaponSet = slot <= 2 ? set : -1;
    const ObjectId before = ItemInSlot(who, slot, weaponSet);
    auto message = dao::messages::Make<dao::messages::EquipItemMessage>();
    message.item = item;
    message.slot = static_cast<std::uint32_t>(slot);
    // Its handler answers true even when the engine refuses the equip
    // (DAOLib #253): `after` says what came of it.
    dao::messages::Send(&CreatureObject(who), message);
    const ObjectId after = ItemInSlot(who, slot, weaponSet);
    answer["slot"] = std::string(SlotName(slot));
    answer["before"] = ItemJson(before, false);
    answer["after"] = ItemJson(after, false);
    answer["equipped"] = after == item;
    if (after != item)
    {
        WhyNotOn(item, who, answer);
    }
    return answer;
}

} // namespace

static void AddEquip(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"equip",
                 "Puts an item from the pack on a party member, or takes one off, the way the inventory screen "
                 "does, so the game's rules apply (requirements, which items fit which slot).\n\n"
                 "`action` is one of:\n"
                 "- equip (default): puts `item` in `slot`. Whatever the slot held goes back to the pack. "
                 "Returns what the slot held `before` and holds `after`, and whether it worked (`equipped`). "
                 "If it did not, the answer says whether the game thinks the creature can use the item "
                 "(`canUse`), with its `requirements` and `restrictions` where known.\n"
                 "- unequip: takes `item` off and puts it in the pack. Returns whether it came off "
                 "(`unequipped`).\n\n"
                 "It acts on `id`, by default the party member selected in the interface, and selects "
                 "nobody: the selection and control stay as they were.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"equip", "unequip"}},
                          {"description", "What to do. Default: equip."}}},
                        {"item",
                         {{"type", "string"}, {"description", "The item, by its id from `inventory` or `equipment`."}}},
                        {"slot",
                         {{"description", "For equip: the slot, by name (main, offhand, ring1, ring2, ...) or "
                                          "number. Default: the first slot the item fits."}}},
                        {"id",
                         {{"type", "string"},
                          {"description", "The party member, by creature id. Default: the one selected in the "
                                          "interface."}}}}},
                      {"required", {"item"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult { return ToolEquip(bench, watch, raw); });
}

namespace
{

// A gift, as the inventory's Give gives it. The Give's message has one
// handler, the full-screen interface mode's (GameModeGUI, 0x00718210), which
// moves nothing itself: it makes the script event EVENT_TYPE_GIFT_ITEM with
// the player's creature as its creator and the item as object 0, and runs it
// on the follower. The rest is the scripts' (rules_core.nss: the approval
// change by Approval_HandleGift, then the module's gift handler moves the
// item, changes the approval, and starts a special gift's conversation).
// Read by lib-agent, 2026-10-05; Zevran had turned on the party with his
// approval at 0, the driver having no way to give one (#81).
constexpr std::int32_t kBaseItemTypeGift = 109; // BASE_ITEM_TYPE_GIFT, 2da_constants_h.nss

bool Holds(ObjectId creature, ObjectId item)
{
    const auto items = api::GetItemsInInventory(creature, k::GET_ITEMS_OPTION_ALL);
    for (std::size_t i = 0; i < items.Size(); ++i)
    {
        if (items.Object(i) == item)
        {
            return true;
        }
    }
    return false;
}

ToolResult ToolGift(Bench &bench, const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "gift");
    const Args args(raw, "gift");
    const ObjectId to = args.Id("to", "a follower, as state lists the party");
    const ObjectId item = args.Id("item", "a gift in the pack, as inventory lists it");
    const ObjectId hero = api::GetHero();
    if (to == hero || !PartyOf(hero).contains(to))
    {
        throw ToolError(std::format("gift: {} is not a follower in the party; state lists it", FormatId(to)));
    }
    if (api::GetObjectType(item) != k::OBJECT_TYPE_ITEM || !Holds(hero, item))
    {
        throw ToolError(std::format("gift: {} is not an item in the pack; inventory lists it", FormatId(item)));
    }
    if (api::GetBaseItemType(item) != kBaseItemTypeGift)
    {
        throw ToolError(std::format("gift: {} is not a gift (its base type is {}); the inventory's Give takes "
                                    "items of base type Gift",
                                    FormatId(item), Rule(k::TABLE_ITEMS, api::GetBaseItemType(item)).dump()));
    }
    const auto cursor = bench.Events().Next();
    const auto before = api::GetFollowerApproval(to);
    auto event = api::Event(k::EVENT_TYPE_GIFT_ITEM);
    if (event)
    {
        event = api::SetEventCreator(*event.get(), hero);
    }
    if (event)
    {
        event = api::SetEventObject(*event.get(), 0, item);
    }
    if (!event)
    {
        throw ToolError("gift: the game made no gift event");
    }
    api::SignalEvent(to, *event.get(), 1);
    // Handled a frame or more later, whatever the flag says: the gift
    // tool (core/Gift.cpp) reads gift.state until it is (#81).
    return Json{{"to", Brief(to)}, {"item", ItemJson(item, false)}, {"approvalBefore", before}, {"cursor", cursor}};
}

// What the gift has done so far: the follower's approval, and whether the
// item is still in the pack.
ToolResult ToolGiftState(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "gift");
    const Args args(raw, "gift");
    const ObjectId to = args.Id("to", "a follower");
    const ObjectId item = args.Id("item", "an item");
    return Json{{"approval", api::GetFollowerApproval(to)}, {"held", Holds(api::GetHero(), item)}};
}

} // namespace

static void AddGift(Bench &bench, const Watch &watch)
{
    // The gift tool's steps, unlisted: the tool itself is core/Gift.cpp's.
    const Json which = {{"type", "object"},
                        {"properties", {{"to", {{"type", "string"}}}, {"item", {{"type", "string"}}}}},
                        {"required", {"to", "item"}}};
    ToolSpec give{"gift.give", "The gift's event signalled: the `gift` tool's first step.", which, Thread::Frame};
    give.listed = false;
    bench.Tools().Add(std::move(give),
                      [&bench, &watch](const Json &raw) -> ToolResult { return ToolGift(bench, watch, raw); });
    ToolSpec state{"gift.state", "The follower's approval and whether the item is held: the `gift` tool's read.", which,
                   Thread::Frame};
    state.listed = false;
    bench.Tools().Add(std::move(state), [&watch](const Json &raw) -> ToolResult { return ToolGiftState(watch, raw); });
}

} // namespace eluvian::game
