// `script`: any of the game's script commands by name, with its answer:
// the general way in where no tool fits.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

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

} // namespace

namespace
{

// A command name's longest word, split where a capital follows a small
// letter: "SetImmortal" gives "Immortal".
std::string LongestWord(std::string_view name)
{
    std::string best;
    std::string word;
    for (const char c : name)
    {
        if (std::isupper(static_cast<unsigned char>(c)) != 0 && !word.empty() &&
            std::islower(static_cast<unsigned char>(word.back())) != 0)
        {
            best = word.size() > best.size() ? word : best;
            word.clear();
        }
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            word += c;
        }
        else
        {
            best = word.size() > best.size() ? word : best;
            word.clear();
        }
    }
    return word.size() > best.size() ? word : best;
}

} // namespace

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
            // find's search is `command`, as is any action's name; a caller
            // who tried `name` and `match` was told only that `command` must
            // be given (turn-based-agent's driver, 2026-10-04, #52).
            if (action == "find" && !args.Has("command"))
            {
                throw ToolError("script: action find searches for the part of a name given in `command`: "
                                "{\"action\": \"find\", \"command\": \"Immortal\"}");
            }
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
                // The near names at once: those holding the name's longest
                // word, SetImmortal's "Immortal" (#52).
                const std::string word = LongestWord(name);
                std::string near;
                int shown = 0;
                for (const auto &candidate : dao::script::table::kCommands)
                {
                    if (!word.empty() && Lower(candidate.name).find(Lower(word)) != std::string::npos && shown < 8)
                    {
                        near += (shown++ == 0 ? "" : ", ") + std::string(candidate.name);
                    }
                }
                throw ToolError(std::format(
                    "script: no command \"{}\"{}; {{\"action\": \"find\", \"command\": \"{}\"}} lists those "
                    "whose name holds a part",
                    name, near.empty() ? std::string() : "; near it: " + near, word.empty() ? name : word));
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

} // namespace eluvian::game
