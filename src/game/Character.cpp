// `inspect`'s kinds for who a creature is and what it carries: `character`
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

// A row of one of the game's rule tables: its id, the designers' label, and
// its name in the player's language where the table has one.
Json Rule(std::int32_t table, std::int32_t row)
{
    Json rule = {{"id", row}, {"label", Narrow(api::GetM2DAString(table, L"Label", row))}};
    // The tables do not agree on the column: classes and base items keep
    // the name's string reference in NameStrRef, races in Name.
    auto name = api::GetM2DAInt(table, L"NameStrRef", row);
    if (name <= 0)
    {
        name = api::GetM2DAInt(table, L"Name", row);
    }
    if (name <= 0)
    {
        name = api::GetM2DAInt(table, L"StrRef", row); // the tactics' tables
    }
    if (name > 0)
    {
        if (const auto text = Narrow(dao::tlk::String(static_cast<std::uint32_t>(name))); !text.empty())
        {
            rule["name"] = text;
        }
    }
    return rule;
}

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

// What the level-up screen offers, read as the screen reads it (#5): it asks
// the engine "GameModeController.FindAbilityChains <creature> <type> 2" (2,
// DISPLAYMODE_LEVELUP) for each page, and the engine fills the data tree's
// CharGenCreature.Stats.TalentChains, SpellChains and SkillChains. Each is
// a list of abilities, one a member, `<list>.<index>`, as the screen's
// LoadNextAbility reads them: AbilityTemplate (its ID, Name, ...),
// AbilityInstance (an ability had), DisplayState (DISPLAYSTATE_HAVE 1,
// AVAILABLE 2, UNLOCKABLE 3), Column and Row, where the screen draws it
// (levelup.gfx's SharedLibrary.Ability, read 2026-10-04). The first
// reading took each of those five members for an ability: groups of five
// with no state, some 60 KB (turn-based-agent, 2026-10-04). The points and attributes are
// CharGenCreature.Attributes.<n>: 1 to 6 the attributes (seen in play: Lyna's 12, 22, 16, 14, 23, 12), 34 to 36 the
// points (3, 1, 1 with three attribute points, a skill and a talent banked), 38 the specialisation points.
Json LevelUpOffers()
{
    const auto creature = dao::data::Number(L"CharGenCreature.ID");
    if (!creature)
    {
        throw ToolError("inspect: no level-up screen is up; the levelup tool with `offers: true` opens one and "
                        "reads this");
    }
    const auto number = [](std::wstring_view path) { return dao::data::Number(path).value_or(0); };
    constexpr std::array<std::string_view, 6> kAttributes = {"strength", "dexterity", "willpower",
                                                             "magic",    "cunning",   "constitution"};
    Json attributes = Json::object();
    for (std::size_t i = 0; i < kAttributes.size(); ++i)
    {
        attributes[std::string(kAttributes[i])] = number(std::format(L"CharGenCreature.Attributes.{}.Total", i + 1));
    }
    Json answer = {{"creature", Brief(static_cast<ObjectId>(*creature))},
                   {"points",
                    {{"attributes", number(L"CharGenCreature.Attributes.34.Total")},
                     {"skills", number(L"CharGenCreature.Attributes.35.Total")},
                     {"talentsOrSpells", number(L"CharGenCreature.Attributes.36.Total")},
                     {"specializations", number(L"CharGenCreature.Attributes.38.Total")}}},
                   {"attributes", std::move(attributes)}};
    struct Page
    {
        int type;
        const wchar_t *path;
        const char *name;
    };
    for (const Page page : {Page{1, L"CharGenCreature.Stats.TalentChains", "talents"},
                            Page{2, L"CharGenCreature.Stats.SpellChains", "spells"},
                            Page{3, L"CharGenCreature.Stats.SkillChains", "skills"}})
    {
        dao::messages::SendText(
            std::format(L"GameModeController.FindAbilityChains {} {} 2", static_cast<ObjectId>(*creature), page.type));
        Json abilities = Json::array();
        for (const auto &member : dao::data::Members(page.path))
        {
            // An instance's template is the ability had; a slot with
            // neither is empty and left out.
            auto id = dao::data::Number(member + L".AbilityTemplate.ID");
            if (!id || *id <= 0)
            {
                id = dao::data::Number(member + L".AbilityInstance.AbilityTemplate.ID");
            }
            if (!id || *id <= 0)
            {
                continue;
            }
            const auto ability = static_cast<std::int32_t>(*id);
            std::string name = Narrow(dao::data::Text(member + L".AbilityTemplate.Name").value_or(L""));
            if (name.empty())
            {
                name = AbilityJson(ability).value("name", std::string());
            }
            const auto state = static_cast<int>(number(member + L".DisplayState"));
            abilities.push_back(Json{{"id", ability},
                                     {"name", std::move(name)},
                                     {"state", Label(state, {{1, "has"}, {2, "learnable"}, {3, "not yet"}})}});
        }
        answer[page.name] = std::move(abilities);
    }
    return answer;
}

} // namespace

// A slot's name, as kind=inventory names it: for the equipment events
// (#57), which live in GameTools.
std::string_view EquipSlotName(std::int32_t slot)
{
    return SlotName(slot);
}

// An item: in brief for a list, in full for itself. Shared: the loot panel
// lists its container's items as kind=inventory does.
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
        throw ToolError("inspect: no creature is controlled and no `id` was given; is a game loaded?");
    }
    if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("inspect: {} is not a creature; inspect kind=creatures lists them", FormatId(id)));
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

namespace
{

// The character kind's handler, registered by AddCharacter.
ToolResult KindCharacter(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
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

// The abilities kind's handler, registered by AddCharacter.
ToolResult KindAbilities(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
    const ObjectId id = CreatureOrControlled(args);
    const auto list = [id](dao::abilities::Kind kind) {
        Json abilities = Json::array();
        for (const std::int32_t ability : dao::abilities::Known(id, kind))
        {
            Json entry = AbilityJson(ability);
            const bool available = api::IsAbilityAvailable(id, ability) != 0;
            const auto cooldown = dao::abilities::GetCooldown(id, ability);
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
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
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
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
    const ObjectId id = CreatureOrControlled(args);
    Json answer = EquipmentJson(id, true);
    answer["of"] = Brief(id);
    return answer;
}

// The inventory kind's handler, registered by AddCharacter.
ToolResult KindInventory(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
    const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetHero());
    if (id == kInvalidObject)
    {
        throw ToolError("inspect: no hero and no `id` was given; is a game loaded?");
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

// The levelup kind's handler, registered by AddCharacter.
ToolResult KindLevelup(const Watch &watch, const Json & /*args*/)
{
    RequireWorld(watch, "inspect");
    return LevelUpOffers();
}

// The item kind's handler, registered by AddCharacter.
ToolResult KindItem(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "inspect");
    const Args args(raw, "inspect");
    const ObjectId id = args.Id("id", "an item, as inspect kind=inventory lists them");
    if (api::GetObjectType(id) != k::OBJECT_TYPE_ITEM)
    {
        throw ToolError(std::format("inspect: {} is not an item; inspect kind=inventory and "
                                    "kind=equipment list items",
                                    FormatId(id)));
    }
    return ItemJson(id, true);
}

} // namespace

void AddCharacter(Bench &bench, const Watch &watch)
{
    AddEquip(bench, watch);
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"character",
                 "A creature's sheet (`id`, default the one controlled): race, gender, class, level, experience "
                 "and whether it can level up with the points left to spend, rank, the six attributes (total "
                 "and base), its combat numbers (attack, defense, armor, armor penetration, spellpower, "
                 "weapon damage, fatigue and the rest), health and mana or "
                 "stamina, weapon style and set, whether it is plot or immortal, for a member of the party `follower` "
                 "(its state, "
                 "approval, whether it is locked in and whether its own AI is on), `properties` (every property it has "
                 "that is not nothing, by the game's own "
                 "names: resistances, regeneration, damage), and what it wears, slot by slot in brief. inspect "
                 "kind=equipment gives the items "
                 "in full, kind=inventory the backpack, kind=creature its effects and abilities."},
        [&watch](const Json &raw) -> ToolResult { return KindCharacter(watch, raw); });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"abilities",
                 "Everything a creature knows (`id`, default the one controlled), not only what is on its "
                 "quickbar: its `talents`, `spells`, `skills` and `items`' abilities, each by id and name, with "
                 "whether it is `ready` (the engine's own check, the one the quickbar greys a button by: the "
                 "cost against its mana or stamina, the cooldown, what holds it; `why` says why not, as "
                 "\"Insufficient stamina\"), its `cost` in mana or stamina with its fatigue, and its cooldown. A "
                 "sustained ability's upkeep is checked again as it runs. A skill with ranks is an entry per rank "
                 "it has. "
                 "`count` is the game's own count of them."},
        [&watch](const Json &raw) -> ToolResult { return KindAbilities(watch, raw); });

    // A creature's tactics, by the scripts' own commands. Slots count from 1
    // and an unset one has command 0 (read in play 2026-10-01: the hero's
    // first was Self, condition 101, Use Ability 603; a follower's Enemy,
    // condition 208, Use Ability 803). The tables name each part by row:
    // targets, conditions, command types (4 use ability, 5 and 6 a mode on
    // and off, 7 use item, whose param is the ability), presets, and the
    // behaviours that property 18 (AI_BEHAVIOR) is a row of.
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"tactics",
                 "A creature's tactics (`id`, default the one controlled): what it does by itself in a fight "
                 "when it is not the one controlled. `slots` is how many it has; `tactics` is each in order: "
                 "`enabled`, its `target` (self, ally, enemy ...), its `condition`, its `command` (attack, use "
                 "ability, activate mode, use item ...) and the `ability` or `item` the command is for; a slot "
                 "not set is `empty`. `preset` (null when none is chosen) and `behaviour` are the two choices above "
                 "the slots."},
        [&watch](const Json &raw) -> ToolResult { return KindTactics(watch, raw); });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"equipment",
                 "What a creature wears (`id`, default the one controlled), each slot's item in full: name, tag, "
                 "template, base type and material (by the game's own labels), value, its stats (armor, damage, "
                 "defense, attack, crit chance, armor penetration, fatigue, optimum range and the rest, by "
                 "the game's own names), its properties with their power, and what using it does. `slots` "
                 "are the active weapon set's main, offhand and ammo, then chest, head, boots, gloves, cloak, "
                 "the rings, neck and belt; `otherWeapons` is the set not in hand."},
        [&watch](const Json &raw) -> ToolResult { return KindEquipment(watch, raw); });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"inventory",
                 "The backpack of a creature (`id`, default the hero, whose backpack is the party's), or what "
                 "a container holds without opening it (a chest, a body, a pile of bones: an `id` from inspect "
                 "kind=placeables): each item in brief (name, tag, template, base type, stack, value, plot), "
                 "with `count`; for a creature also the most it `holds` and `money`, the party's purse. `full: "
                 "true` gives each item as inspect kind=item does. Not what is worn: that is inspect "
                 "kind=equipment. A container's treasure may be chosen only as it is opened (a body's remains "
                 "read empty, then held a gem once open), so an empty list before is not the last word: the "
                 "loot tool opens and empties one."},
        [&watch](const Json &raw) -> ToolResult { return KindInventory(watch, raw); });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"levelup",
                 "What the level-up screen offers its creature, while it is up (the levelup tool with `offers: "
                 "true` opens it, reads this and closes it unchanged): the `points` to spend (attributes, skills, "
                 "talents or spells, specialisations), the six `attributes` as the screen shows them, and the "
                 "class's `talents`, `spells` and `skills`, each a list of abilities, `id`, `name` and "
                 "`state`: has, learnable (can be taken now) or not yet (a level, an attribute or the one "
                 "before it in its chain). The levelup tool spends points on named ones."},
        [&watch](const Json &args) -> ToolResult { return KindLevelup(watch, args); });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"item",
                 "One item in full, by its `id` (as inspect kind=inventory and kind=equipment give them): name, "
                 "tag, template, base type, material, stack and the most a stack holds, value, where it is "
                 "worn, its stats, its properties with their power, what using it does, and whether it is "
                 "plot, droppable or indestructible."},
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
// interface has selected (the engine's EquipItemMessage and
// UnequipItemMessage are CCCreature's). Not yet seen in play.
namespace
{

// The equip tool's handler, registered by AddEquip.
// Taken off as the paperdoll does: back to the pack.
Json Unequip(ObjectId item, Json answer)
{
    const auto slot = api::GetItemEquipSlot(item);
    if (SlotName(slot).empty())
    {
        throw ToolError("equip: that item is not worn; inspect kind=equipment says what is");
    }
    if (!dao::messages::SendText(std::format(L"GUI.SelectedChar.UnequipItem {}", item)))
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
    for (const wchar_t *screen : {L"Quickbar", L"Inventory", L"CharacterRecord", L"Store"})
    {
        auto movie = dao::gui::ScreenMovie(screen);
        if (!movie)
        {
            continue;
        }
        const std::array<dao::gui::Value, 2> ids{dao::gui::Value::Number(static_cast<double>(item)),
                                                 dao::gui::Value::Number(static_cast<double>(who))};
        if (!movie->Call("DAExternalCommands.GetItemRequirements", ids))
        {
            continue;
        }
        using Type = dao::gui::Value::Type;
        const auto said = movie->Get("_root.fsc_retval");
        if (said && (said->type == Type::String || said->type == Type::StringW))
        {
            return ItemRequirements(said->text);
        }
    }
    return std::nullopt;
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
                                  "an attribute (strength for heavy armour), or a restriction; the "
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
    const ObjectId item = args.Id("item", "an item, as inspect kind=inventory and kind=equipment list them");
    if (api::GetObjectType(item) != k::OBJECT_TYPE_ITEM)
    {
        throw ToolError(std::format("equip: {} is not an item; inspect kind=inventory lists the pack", FormatId(item)));
    }
    const auto selected = static_cast<ObjectId>(
        dao::data::Number(L"GUI.SelectedChar.ID").value_or(static_cast<double>(api::GetMainControlled())));
    const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(selected);
    if (who != selected)
    {
        // order action=control selects a party member wherever the
        // party is; the binding SelectPartyMemberIndexMessage, named
        // here before, selected nobody at camp (turn-based-agent,
        // 2026-10-03, #31).
        throw ToolError(std::format("equip: the inventory screen equips the character the interface has "
                                    "selected, {} now; select {} first: order {{\"action\": \"control\", "
                                    "\"id\": \"{}\"}}",
                                    FormatId(selected), FormatId(who), FormatId(who)));
    }
    const auto set = api::GetActiveWeaponSet(who);
    Json answer = {{"action", action},
                   {"item", ItemJson(item, false)},
                   {"creature", Brief(who)},
                   {"cursor", bench.Events().Next()}};
    if (action == "unequip")
    {
        return Unequip(item, std::move(answer));
    }
    const auto mask = api::GetM2DAInt(k::TABLE_ITEMS, L"EquippableSlots", api::GetBaseItemType(item));
    const std::int32_t slot = SlotFor(args, mask);
    const auto weaponSet = slot <= 2 ? set : -1;
    const ObjectId before = ItemInSlot(who, slot, weaponSet);
    if (!dao::messages::SendText(std::format(L"GUI.SelectedChar.EquipItem {} {}", item, slot)))
    {
        throw ToolError("equip: the game took no equip");
    }
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
                 "Put on an item of the pack, or take one off, as the inventory screen's paperdoll does, so the "
                 "game's rules hold (requirements, what a slot takes). `action`: equip (default) or unequip; "
                 "`item`, an id from inspect kind=inventory or kind=equipment; `slot` for equip, by name (main, "
                 "offhand, ring1, ring2, ...) or number, default the item's first. It acts on the character the "
                 "interface has selected (`id`, if given, must be that one: order {\"action\": \"control\", "
                 "\"id\": ...} makes another the one selected). Answers what the "
                 "slot held `before` and holds `after`, and whether it took (`equipped`); the item a slot held "
                 "goes back to the pack.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"equip", "unequip"}}}},
                        {"item", {{"type", "string"}, {"description", "The item's id"}}},
                        {"slot", {{"description", "equip: the slot's name or number; default the item's first"}}},
                        {"id", {{"type", "string"}, {"description", "The creature: the one selected"}}}}},
                      {"required", {"item"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult { return ToolEquip(bench, watch, raw); });
}

} // namespace eluvian::game
