// The tactics tool: a follower's tactic slots set as the tactics screen sets
// them, and what the screen offers for each choice (#89). Since #188 all of
// it is DAOLib's dao::tactics (#259, 25fbba7, lib-agent-3, 2026-10-25): the
// offers from the engine's own list builders and a slot set as the screen's
// pickers set it, with no screen and no selection. Until then the offers came
// through a shown screen's FSCommands (answered in the movie's fsc_retval),
// and a slot was set by writing its row in the data tree under
// GUI.SelectedChar.Tactics and telling it DirtyPreset and ValidateAction
// (lib-agent, read in tactics.gfx's ActionScript, 2026-10-08), the follower
// selected first (#187).
#include "game/Shared.h"

#include "dao/Tactics.h"

#include <cctype>

namespace eluvian::game
{

namespace
{

namespace tactics = dao::tactics;

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A choice named by number or by label or name (any case), among those the
// engine offers; refused naming the offered ones. Its place among them.
std::size_t Pick(const Args &args, std::string_view key, const std::vector<std::int32_t> &offered, std::int32_t table)
{
    const Json &raw = args.Raw().at(std::string(key));
    Json said = Json::array();
    for (std::size_t i = 0; i < offered.size(); ++i)
    {
        const Json rule = Rule(table, offered[i]);
        const bool byNumber = raw.is_number_integer() && raw.get<std::int64_t>() == offered[i];
        const bool byName =
            raw.is_string() && (Lower(rule.value("label", std::string())) == Lower(raw.get<std::string>()) ||
                                Lower(rule.value("name", std::string())) == Lower(raw.get<std::string>()));
        if (byNumber || byName)
        {
            return i;
        }
        said.push_back(rule);
    }
    throw ToolError(
        std::format("tactics: `{}` {} is not among what the screen offers here: {}", key, raw.dump(), said.dump()));
}

void Refused(tactics::Refusal refusal, std::string_view what)
{
    switch (refusal)
    {
    case tactics::Refusal::None:
        return;
    case tactics::Refusal::NotFollower:
        throw ToolError("tactics: the creature has no tactics; only a follower has them");
    case tactics::Refusal::NoSuchSlot:
        throw ToolError("tactics: no such slot; tactics action=look says how many slots the follower has");
    default:
        throw ToolError(std::format("tactics: the screen offers no such {} here; tactics action=choices lists what "
                                    "it does",
                                    what));
    }
}

// A slot as the screens read it back.
Json SlotNow(ObjectId who, std::int32_t index)
{
    for (const tactics::Slot &slot : tactics::Slots(who))
    {
        if (slot.index == index)
        {
            return Json{{"slot", slot.index},
                        {"enabled", slot.on},
                        {"target", Rule(k::TABLE_AI_TACTICS_TARGET_TYPE, slot.target)},
                        {"condition", Rule(k::TABLE_TACTICS_CONDITIONS, slot.condition)},
                        {"command", Rule(k::TABLE_COMMAND_TYPES, slot.command)},
                        {"param", slot.parameter}};
        }
    }
    return nullptr;
}

std::vector<std::int32_t> TargetTypes(const std::vector<tactics::Target> &targets)
{
    std::vector<std::int32_t> types;
    types.reserve(targets.size());
    for (const tactics::Target &target : targets)
    {
        types.push_back(target.type);
    }
    return types;
}

// What the screen offers: the targets, and with `target` its base
// conditions, and with `baseCondition` too its conditions, actions,
// abilities and items.
Json Choices(const Args &args, ObjectId who)
{
    const auto targets = tactics::Targets(who);
    Json answer = {{"of", Brief(who)}};
    Json list = Json::array();
    for (const tactics::Target &target : targets)
    {
        Json entry = {{"type", Rule(k::TABLE_AI_TACTICS_TARGET_TYPE, target.type)}};
        if (target.object != kInvalidObject)
        {
            entry["object"] = Brief(target.object);
        }
        list.push_back(std::move(entry));
    }
    answer["targets"] = std::move(list);
    if (!args.Has("target"))
    {
        return answer;
    }
    const std::int32_t target =
        targets[Pick(args, "target", TargetTypes(targets), k::TABLE_AI_TACTICS_TARGET_TYPE)].type;
    const auto bases = tactics::BaseConditions(target);
    answer["baseConditions"] = Json::array();
    for (const auto base : bases)
    {
        answer["baseConditions"].push_back(Rule(k::TABLE_TACTICS_BASE_CONDITIONS, base));
    }
    if (!args.Has("baseCondition"))
    {
        return answer;
    }
    const std::int32_t base = bases[Pick(args, "baseCondition", bases, k::TABLE_TACTICS_BASE_CONDITIONS)];
    answer["conditions"] = Json::array();
    for (const tactics::Condition &condition : tactics::Conditions(base, target))
    {
        Json entry = {{"condition", Rule(k::TABLE_TACTICS_CONDITIONS, condition.condition)}};
        if (condition.object != kInvalidObject)
        {
            entry["object"] = Brief(condition.object);
        }
        answer["conditions"].push_back(std::move(entry));
    }
    answer["actions"] = Json::array();
    for (const auto action : tactics::Commands(who, target, base))
    {
        answer["actions"].push_back(Rule(k::TABLE_COMMAND_TYPES, action));
    }
    answer["abilities"] = Json::array();
    for (const auto ability : tactics::Abilities(who, target, base))
    {
        answer["abilities"].push_back(AbilityJson(ability));
    }
    answer["items"] = Json::array();
    for (const ObjectId item : tactics::Items(target))
    {
        answer["items"].push_back(Brief(item));
    }
    return answer;
}

// The condition named, found under the base condition that offers it for
// the target: the base, and the condition with the follower it names.
struct Found
{
    std::int32_t base = 0;
    tactics::Condition condition;
};

Found FindCondition(const Args &args, std::int32_t target)
{
    std::vector<std::int32_t> all;
    for (const auto base : tactics::BaseConditions(target))
    {
        const auto conditions = tactics::Conditions(base, target);
        std::vector<std::int32_t> rows;
        rows.reserve(conditions.size());
        for (const tactics::Condition &condition : conditions)
        {
            rows.push_back(condition.condition);
        }
        const Json &raw = args.Raw().at("condition");
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const Json rule = Rule(k::TABLE_TACTICS_CONDITIONS, rows[i]);
            if ((raw.is_number_integer() && raw.get<std::int64_t>() == rows[i]) ||
                (raw.is_string() && (Lower(rule.value("label", std::string())) == Lower(raw.get<std::string>()) ||
                                     Lower(rule.value("name", std::string())) == Lower(raw.get<std::string>()))))
            {
                return Found{base, conditions[i]};
            }
        }
        all.insert(all.end(), rows.begin(), rows.end());
    }
    Pick(args, "condition", all, k::TABLE_TACTICS_CONDITIONS); // refuses, naming them
    return {};
}

// A slot set as the screen's pickers set it: its target and condition, then
// its command, then whether it is on; read back to say whether it took.
Json Set(const Args &args, ObjectId who)
{
    const auto slot = static_cast<std::int32_t>(args.Integer("slot", 1, 1, 64));
    const auto targets = tactics::Targets(who);
    const tactics::Target target = targets[Pick(args, "target", TargetTypes(targets), k::TABLE_AI_TACTICS_TARGET_TYPE)];
    const Found found = FindCondition(args, target.type);
    Refused(tactics::SetCondition(who, slot, target, found.condition), "target or condition");
    const auto commands = tactics::Commands(who, target.type, found.base);
    const std::int32_t command = commands[Pick(args, "command", commands, k::TABLE_COMMAND_TYPES)];
    const auto parameter = static_cast<std::int32_t>(args.Integer("ability", 0, 0, 1 << 30));
    const ObjectId item = args.Has("item") ? args.Id("item", "an item the follower holds") : kInvalidObject;
    Refused(tactics::SetCommand(who, slot, command, parameter, item), "command, ability or item");
    Refused(tactics::SetOn(who, slot, args.Bool("enabled", true)), "slot");
    Json now = SlotNow(who, slot);
    const bool took = now.is_object() && now["target"]["id"] == target.type &&
                      now["condition"]["id"] == found.condition.condition && now["command"]["id"] == command;
    return Json{{"of", Brief(who)}, {"slot", std::move(now)}, {"took", took}};
}

// A slot emptied as the screen's row is: its action picker's Clear (command
// none), then its condition picker's (target and condition none), which
// leaves the command as it was (lib-agent-3, 2026-10-25: a condition cleared
// first kept Wynne's Heal), and switched off. The command's Clear is refused
// where the slot has no target already, its picker disabled, which leaves
// nothing to clear. A script that saved a follower's slots could put back a
// set one but not an empty one (turn-based-agent, 2026-10-09).
Json Clear(const Args &args, ObjectId who)
{
    const auto slot = static_cast<std::int32_t>(args.Integer("slot", 1, 1, 64));
    if (const auto refusal = tactics::SetCommand(who, slot, 0); refusal != tactics::Refusal::NotOffered)
    {
        Refused(refusal, "slot");
    }
    Refused(tactics::SetCondition(who, slot, tactics::Target{}, tactics::Condition{}), "slot");
    Refused(tactics::SetOn(who, slot, false), "slot");
    const Json now = SlotNow(who, slot);
    return Json{{"of", Brief(who)}, {"slot", slot}, {"cleared", now.is_object() && now["command"]["id"] == 0}};
}

ToolResult ToolTactics(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "tactics");
    const Args args(raw, "tactics");
    const auto action = args.Choice("action", {"look", "choices", "set", "clear", "preset"});
    // The slots read, as tactics action=look read them (#110).
    if (action == "look")
    {
        return ReadTactics(watch, raw);
    }
    const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
    if (action == "choices")
    {
        return Choices(args, who);
    }
    if (action == "set")
    {
        return Set(args, who);
    }
    if (action == "clear")
    {
        return Clear(args, who);
    }
    const auto type = static_cast<std::int32_t>(args.Integer("presetType", 2, 0, 2));
    const auto index = static_cast<std::int32_t>(args.Integer("preset", 0, 0, 255));
    if (!tactics::LoadPreset(who, type, index))
    {
        throw ToolError(std::format("tactics: preset {} of type {} was not loaded: the creature has no tactics, or "
                                    "there is no such preset",
                                    index, type));
    }
    Json slots = Json::array();
    for (const tactics::Slot &slot : tactics::Slots(who))
    {
        slots.push_back(SlotNow(who, slot.index));
    }
    return Json{{"of", Brief(who)}, {"slots", std::move(slots)}};
}

} // namespace

void AddTactics(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "tactics",
            "Reads and sets a party member's tactics (what they do on their own in a fight), the way the "
            "tactics screen does, with no screen open and nobody selected.\n\n"
            "`action` is one of:\n"
            "- look: returns `slots` (how many there are), `ai` (whether the follower's tactics run), `preset` "
            "(null when none is chosen), `behaviour`, and `tactics`: each slot in order with `enabled`, `target`, "
            "`condition`, `command` and the `ability` or `item` it uses. An unset slot is `empty`.\n"
            "- choices: returns what the screen offers this party member: the `targets`; with `target`, "
            "its `baseConditions`; with `baseCondition` as well, the `conditions`, `actions`, `abilities` "
            "and `items`.\n"
            "- set: sets slot `slot` to `target`, `condition` and `command`, each by number, label or name "
            "from what choices offers, with `ability` or `item` for the command and `enabled`. Returns the "
            "slot as the game reads it back and whether the change `took`; one the screen would not offer is "
            "refused, saying which part.\n"
            "- clear: empties slot `slot` and returns whether it is `cleared`. Use it to put a party "
            "member's tactics back as you found them.\n"
            "- preset: loads preset `preset` of type `presetType` (0 the AI off, 1 a custom slot, 2 one of the "
            "game's, the default) and returns the slots.",
            Json{
                {"type", "object"},
                {"properties",
                 {{"action",
                   {{"type", "string"},
                    {"enum", {"look", "choices", "set", "clear", "preset"}},
                    {"description", "What to do."}}},
                  {"id",
                   {{"type", "string"},
                    {"description", "The party member, by creature id. Default: the creature you control."}}},
                  {"slot",
                   {{"type", "integer"},
                    {"minimum", 1},
                    {"description", "For set and clear: the slot, counting from 1. Default 1."}}},
                  {"target", {{"description", "For choices and set: a target type, by number, label or name."}}},
                  {"baseCondition", {{"description", "For choices: a base condition, by number, label or name."}}},
                  {"condition", {{"description", "For set: a condition, by number, label or name."}}},
                  {"command", {{"description", "For set: the action to take, by number, label or name."}}},
                  {"ability",
                   {{"type", "integer"},
                    {"description", "For set: the id of the ability the action uses, or the slot a jump goes to."}}},
                  {"item", {{"type", "string"}, {"description", "For set: the id of the item the action uses."}}},
                  {"enabled", {{"type", "boolean"}, {"description", "For set: whether the slot is on. Default true."}}},
                  {"preset",
                   {{"type", "integer"},
                    {"description", "For preset: the custom slot or the place among the game's presets, from 0. "
                                    "Default 0."}}},
                  {"presetType",
                   {{"type", "integer"},
                    {"description", "For preset: 0 the AI off, 1 a custom slot, 2 one of the game's. Default 2."}}}}},
                {"required", {"action"}}},
            Thread::Frame},
        [&watch](const Json &raw) -> ToolResult { return ToolTactics(watch, raw); });
}

} // namespace eluvian::game
