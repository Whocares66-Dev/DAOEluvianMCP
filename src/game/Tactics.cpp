// The tactics tool: a follower's tactic slots set as the tactics screen sets
// them, and what the screen offers for each choice (#89). The screen works on
// the data tree under GUI.SelectedChar.Tactics; what it offers comes from the
// engine through FSCommands, answered in the movie's fsc_retval; a pick
// writes the row's fields, then tells the tree DirtyPreset and, for a
// condition, ValidateAction (lib-agent, read in tactics.gfx's ActionScript,
// 2026-10-08). A driver set slots with the scripts' SetTacticEntry and
// numbers copied out of the conditions table (turn-based-agent's survey,
// 2026-10-06).
#include "game/Shared.h"

#include "core/GfxList.h"

#include "dao/Data.h"

#include <cctype>

namespace eluvian::game
{

namespace
{

constexpr std::int32_t kMostSlots = 64;

struct Lists
{
    std::vector<std::int64_t> first;
    std::vector<std::int64_t> second;
};

// An FSCommand of the screen's, with numbers; refused if no screen took it.
Lists AskEngine(std::string_view command, std::initializer_list<double> numbers)
{
    std::vector<dao::gui::Value> args;
    args.reserve(numbers.size());
    for (const double number : numbers)
    {
        args.push_back(dao::gui::Value::Number(number));
    }
    const auto said = ExternalCommand(command, args);
    if (!said)
    {
        throw ToolError(std::format("tactics: no screen shown took {}: the quickbar is hidden while a picker, "
                                    "a conversation or a full screen is up",
                                    command));
    }
    return Lists{ListNumbers(said->first), ListNumbers(said->second)};
}

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A choice named by number or by label or name (any case), among those the
// engine offers; refused naming the offered ones.
std::int64_t Pick(const Args &args, std::string_view key, const std::vector<std::int64_t> &offered, std::int32_t table)
{
    const Json &raw = args.Raw().at(std::string(key));
    Json said = Json::array();
    for (const std::int64_t row : offered)
    {
        const Json rule = Rule(table, static_cast<std::int32_t>(row));
        const bool byNumber = raw.is_number_integer() && raw.get<std::int64_t>() == row;
        const bool byName =
            raw.is_string() && (Lower(rule.value("label", std::string())) == Lower(raw.get<std::string>()) ||
                                Lower(rule.value("name", std::string())) == Lower(raw.get<std::string>()));
        if (byNumber || byName)
        {
            return row;
        }
        said.push_back(rule);
    }
    throw ToolError(
        std::format("tactics: `{}` {} is not among what the screen offers here: {}", key, raw.dump(), said.dump()));
}

// A row's field written as the screen's SetValue writes it, from text: the
// screen reads each back with Number().
void WriteField(std::int32_t row, std::wstring_view field, std::wstring_view value)
{
    const std::wstring path = std::format(L"GUI.SelectedChar.Tactics.TacticsList.{}.{}", row, field);
    auto *attribute = dao::data::Find(path);
    if (attribute == nullptr)
    {
        throw ToolError(std::format("tactics: the tree has no {}; the screen's list is shorter ("
                                    "tactics action=look says how many slots the follower has)",
                                    Narrow(path)));
    }
    attribute->SetText(value);
}

void TellTree(std::wstring_view message)
{
    dao::messages::SendText(std::wstring(L"GUI.SelectedChar.Tactics.") + std::wstring(message));
}

// A slot as the scripts read it back.
Json SlotNow(ObjectId who, std::int32_t slot)
{
    return Json{{"slot", slot},
                {"enabled", api::IsTacticEnabled(who, slot) != 0},
                {"target", Rule(k::TABLE_AI_TACTICS_TARGET_TYPE, api::GetTacticTargetType(who, slot))},
                {"condition", Rule(k::TABLE_TACTICS_CONDITIONS, api::GetTacticCondition(who, slot))},
                {"command", Rule(k::TABLE_COMMAND_TYPES, api::GetTacticCommand(who, slot))},
                {"param", api::GetTacticCommandParam(who, slot)}};
}

// What the screen offers: the targets, and with `target` its base
// conditions, and with `baseCondition` too its conditions, actions,
// abilities and items.
Json Choices(const Args &args, ObjectId who)
{
    const auto targets = AskEngine("ListTacticTargets", {static_cast<double>(who)});
    Json answer = {{"of", Brief(who)}};
    Json list = Json::array();
    for (std::size_t i = 0; i < targets.first.size(); ++i)
    {
        Json entry = {{"type", Rule(k::TABLE_AI_TACTICS_TARGET_TYPE, static_cast<std::int32_t>(targets.first[i]))}};
        if (i < targets.second.size() && targets.second[i] != 0)
        {
            entry["object"] = Brief(static_cast<ObjectId>(targets.second[i]));
        }
        list.push_back(std::move(entry));
    }
    answer["targets"] = std::move(list);
    if (!args.Has("target"))
    {
        return answer;
    }
    const auto target = Pick(args, "target", targets.first, k::TABLE_AI_TACTICS_TARGET_TYPE);
    const auto bases = AskEngine("ListTacticBaseConditions", {static_cast<double>(target)}).first;
    answer["baseConditions"] = Json::array();
    for (const auto base : bases)
    {
        answer["baseConditions"].push_back(Rule(k::TABLE_TACTICS_BASE_CONDITIONS, static_cast<std::int32_t>(base)));
    }
    if (!args.Has("baseCondition"))
    {
        return answer;
    }
    const auto base = Pick(args, "baseCondition", bases, k::TABLE_TACTICS_BASE_CONDITIONS);
    const auto t = static_cast<double>(target);
    const auto b = static_cast<double>(base);
    const auto conditions = AskEngine("ListTacticConditions", {b, t});
    answer["conditions"] = Json::array();
    for (std::size_t i = 0; i < conditions.first.size(); ++i)
    {
        answer["conditions"].push_back(
            Json{{"condition", Rule(k::TABLE_TACTICS_CONDITIONS, static_cast<std::int32_t>(conditions.first[i]))},
                 {"conditionId", i < conditions.second.size() ? conditions.second[i] : 0}});
    }
    answer["actions"] = Json::array();
    for (const auto action : AskEngine("ListTacticActions", {static_cast<double>(who), t, b}).first)
    {
        answer["actions"].push_back(Rule(k::TABLE_COMMAND_TYPES, static_cast<std::int32_t>(action)));
    }
    answer["abilities"] = Json::array();
    for (const auto ability : AskEngine("ListTacticAbilities", {static_cast<double>(who), t, b}).first)
    {
        answer["abilities"].push_back(AbilityJson(static_cast<std::int32_t>(ability)));
    }
    answer["items"] = Json::array();
    for (const auto item : AskEngine("ListTacticItems", {t}).first)
    {
        answer["items"].push_back(Brief(static_cast<ObjectId>(item)));
    }
    return answer;
}

// The condition named, found under the base condition that offers it for
// the target: the base, the condition and its id.
struct Found
{
    std::int64_t base = 0;
    std::int64_t condition = 0;
    std::int64_t id = 0;
};

Found FindCondition(const Args &args, std::int64_t target)
{
    std::vector<std::int64_t> all;
    for (const auto base : AskEngine("ListTacticBaseConditions", {static_cast<double>(target)}).first)
    {
        const auto conditions =
            AskEngine("ListTacticConditions", {static_cast<double>(base), static_cast<double>(target)});
        all.insert(all.end(), conditions.first.begin(), conditions.first.end());
        for (std::size_t i = 0; i < conditions.first.size(); ++i)
        {
            const Json &raw = args.Raw().at("condition");
            const Json rule = Rule(k::TABLE_TACTICS_CONDITIONS, static_cast<std::int32_t>(conditions.first[i]));
            if ((raw.is_number_integer() && raw.get<std::int64_t>() == conditions.first[i]) ||
                (raw.is_string() && (Lower(rule.value("label", std::string())) == Lower(raw.get<std::string>()) ||
                                     Lower(rule.value("name", std::string())) == Lower(raw.get<std::string>()))))
            {
                return Found{base, conditions.first[i], i < conditions.second.size() ? conditions.second[i] : 0};
            }
        }
    }
    Pick(args, "condition", all, k::TABLE_TACTICS_CONDITIONS); // refuses, naming them
    return {};
}

// A slot set as the screen sets one: the condition's fields, DirtyPreset
// and ValidateAction; then the action's, DirtyPreset; then whether it is
// enabled. Read back as the scripts read it, to say whether it took.
Json Set(const Args &args, ObjectId who)
{
    const auto slot = static_cast<std::int32_t>(args.Integer("slot", 1, 1, kMostSlots));
    const std::int32_t row = slot - 1;
    TellTree(L"RefreshTacticsData 1");
    const auto targets = AskEngine("ListTacticTargets", {static_cast<double>(who)});
    const auto target = Pick(args, "target", targets.first, k::TABLE_AI_TACTICS_TARGET_TYPE);
    const auto at = std::ranges::find(targets.first, target) - targets.first.begin();
    const std::int64_t targetObject =
        static_cast<std::size_t>(at) < targets.second.size() ? targets.second[static_cast<std::size_t>(at)] : 0;
    const Found found = FindCondition(args, target);
    WriteField(row, L"Target", std::to_wstring(target));
    WriteField(row, L"TargetID", std::to_wstring(targetObject));
    WriteField(row, L"Condition", std::to_wstring(found.condition));
    WriteField(row, L"ConditionID", std::to_wstring(found.id));
    TellTree(L"DirtyPreset");
    TellTree(std::format(L"ValidateAction {}", row));
    const auto actions = AskEngine(
        "ListTacticActions", {static_cast<double>(who), static_cast<double>(target), static_cast<double>(found.base)});
    const auto command = Pick(args, "command", actions.first, k::TABLE_COMMAND_TYPES);
    const std::int64_t ability = args.Has("ability") ? args.Integer("ability", 0, 0, 1 << 30) : 0;
    const std::wstring item = args.Has("item") ? std::to_wstring(args.Id("item", "an item the follower holds")) : L"";
    WriteField(row, L"Command", std::to_wstring(command));
    WriteField(row, L"CommandParam", std::to_wstring(item.empty() ? ability : 0));
    WriteField(row, L"CommandItem", item);
    TellTree(L"DirtyPreset");
    WriteField(row, L"Enabled", args.Bool("enabled", true) ? L"1" : L"0");
    Json now = SlotNow(who, slot);
    const bool took =
        now["target"]["id"] == target && now["condition"]["id"] == found.condition && now["command"]["id"] == command;
    return Json{{"of", Brief(who)},
                {"slot", std::move(now)},
                {"took", took},
                {"note", took ? "the slot reads back as set"
                              : "the tree's row was set but the slot does not read back so yet: the game may apply "
                                "it when the tactics screen closes (tactics action=look reads it again)"}};
}

// A slot emptied as the screen's row would be: every field none, then
// DirtyPreset. A script that saved a follower's slots could put back a set
// one but not an empty one (turn-based-agent, 2026-10-09). Read back as
// tactics action=look reads an empty slot: no command.
Json Clear(const Args &args, ObjectId who)
{
    const auto slot = static_cast<std::int32_t>(args.Integer("slot", 1, 1, kMostSlots));
    const std::int32_t row = slot - 1;
    TellTree(L"RefreshTacticsData 1");
    for (const wchar_t *field : {L"Target", L"TargetID", L"Condition", L"ConditionID", L"Command", L"CommandParam"})
    {
        WriteField(row, field, L"0");
    }
    WriteField(row, L"CommandItem", L"");
    WriteField(row, L"Enabled", L"0");
    TellTree(L"DirtyPreset");
    const bool cleared = api::GetTacticCommand(who, slot) == 0;
    return Json{{"of", Brief(who)},
                {"slot", slot},
                {"cleared", cleared},
                {"note", cleared ? "the slot reads back empty"
                                 : "the tree's row was emptied but the slot does not read back so: "
                                   "tactics action=look reads it"}};
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
    const ObjectId selected = SelectedCharacter();
    const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(selected);
    const SelectedFor selection(who, selected, "tactics");
    Json answer;
    if (action == "choices")
    {
        answer = Choices(args, who);
    }
    else if (action == "set")
    {
        answer = Set(args, who);
    }
    else if (action == "clear")
    {
        answer = Clear(args, who);
    }
    else
    {
        TellTree(
            std::format(L"LoadPreset {} {}", args.Integer("preset", 0, 0, 255), args.Integer("presetType", 0, 0, 255)));
        Json slots = Json::array();
        for (std::int32_t slot = 1; slot <= api::GetNumTactics(who) && slot <= kMostSlots; ++slot)
        {
            slots.push_back(SlotNow(who, slot));
        }
        answer = Json{{"of", Brief(who)}, {"slots", std::move(slots)}};
    }
    selection.Say(answer);
    return answer;
}

} // namespace

void AddTactics(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"tactics",
                 "A follower's tactics, as the tactics screen sets them (`id`, a party member, default the one the "
                 "interface has selected; another is selected for the call and control given back). "
                 "`action`: choices, what the screen offers: the `targets`, with `target` its "
                 "`baseConditions`, and with `baseCondition` too the `conditions`, `actions`, `abilities` and "
                 "`items` (the engine's own lists for this follower); set, slot `slot` (from 1) to `target`, "
                 "`condition` and `command`, each by number or by label or name among those offered, with "
                 "`ability` (an id) or `item` (an id) for the action, `enabled` (default true), answering the slot "
                 "as the scripts read it back and whether it `took`; clear, slot `slot` emptied (`cleared`), "
                 "to leave a follower as it was found; preset, load preset `preset` "
                 "(`presetType`), answering the slots. tactics action=look reads them.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"look", "choices", "set", "clear", "preset"}}}},
                        {"id", {{"type", "string"}, {"description", "The follower (default: the one selected)"}}},
                        {"slot", {{"type", "integer"}, {"minimum", 1}}},
                        {"target", {{"description", "A target type, by number or label or name"}}},
                        {"baseCondition", {{"description", "choices: a base condition, by number or label or name"}}},
                        {"condition", {{"description", "set: a condition, by number or label or name"}}},
                        {"command", {{"description", "set: an action (command type), by number or label or name"}}},
                        {"ability", {{"type", "integer"}, {"description", "set: the ability the action uses"}}},
                        {"item", {{"type", "string"}, {"description", "set: the item the action uses"}}},
                        {"enabled", {{"type", "boolean"}}},
                        {"preset", {{"type", "integer"}}},
                        {"presetType", {{"type", "integer"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&watch](const Json &raw) -> ToolResult { return ToolTactics(watch, raw); });
}

} // namespace eluvian::game
