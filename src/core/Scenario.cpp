#include "core/Scenario.h"

#include "core/Args.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The value at `pointer` (a JSON pointer, "/given", "/creatures/0/id") in a
// step's answer; nullopt if there is none there.
std::optional<Json> At(const Json &answer, const std::string &pointer)
{
    try
    {
        const Json::json_pointer path(pointer);
        if (!answer.contains(path))
        {
            return std::nullopt;
        }
        return std::optional<Json>(answer.at(path));
    }
    catch (const Json::exception &)
    {
        throw ToolError(std::format("scenario: `pointer` \"{}\" is not a JSON pointer like \"/given\"", pointer));
    }
}

// One expectation over a value: equals, notEquals, exists, atLeast, atMost.
// Returns what failed, or empty.
std::string Check(const Json &expect, const std::optional<Json> &actual)
{
    const auto describe = [&actual] { return actual ? actual->dump() : std::string("nothing"); };
    if (const auto it = expect.find("exists"); it != expect.end())
    {
        if (it->is_boolean() && actual.has_value() != it->get<bool>())
        {
            return std::format("expected it {}to exist; found {}", it->get<bool>() ? "" : "not ", describe());
        }
    }
    if (const auto it = expect.find("equals"); it != expect.end() && (!actual || *actual != *it))
    {
        return std::format("expected {}; found {}", it->dump(), describe());
    }
    if (const auto it = expect.find("notEquals"); it != expect.end() && actual && *actual == *it)
    {
        return std::format("expected anything but {}", it->dump());
    }
    for (const auto *bound : {"atLeast", "atMost"})
    {
        const auto it = expect.find(bound);
        if (it == expect.end())
        {
            continue;
        }
        if (!it->is_number() || !actual || !actual->is_number())
        {
            return std::format("expected a number {} {}; found {}", bound, it->dump(), describe());
        }
        const double have = actual->get<double>();
        const double limit = it->get<double>();
        if (std::string_view(bound) == "atLeast" ? have < limit : have > limit)
        {
            return std::format("expected {} {}; found {}", bound, it->dump(), describe());
        }
    }
    return {};
}

std::string KindOf(const Json &step)
{
    for (const auto *kind : {"tool", "waitFor", "sleepMs", "expect"})
    {
        if (step.contains(kind))
        {
            return kind;
        }
    }
    return {};
}

} // namespace

void AddScenarioTool(Bench &bench)
{
    const Json schema = {
        {"type", "object"},
        {"properties",
         {{"steps",
           {{"type", "array"},
            {"description",
             "Each one of: {\"tool\": name, \"args\": {...}, \"as\": label}; {\"waitFor\": {\"topic\": ..., "
             "\"match\": {...}}, \"timeoutMs\": n, \"since\": cursor, \"as\": label}; {\"sleepMs\": n}; "
             "{\"expect\": {\"from\": label, \"pointer\": \"/given\", \"equals\" | \"notEquals\" | \"exists\" | "
             "\"atLeast\" | \"atMost\": value}}"}}},
          {"continueOnError", {{"type", "boolean"}, {"description", "Run on past a failed step; default false"}}}}},
        {"required", {"steps"}}};
    bench.Tools().Add(
        ToolSpec{"scenario",
                 "Run a list of steps in one call and get a transcript: call tools, wait for events, pause, check "
                 "answers. A test's whole checklist step at once: order an attack, wait for the turn to end, check "
                 "the target's health. A wait starts from the cursor taken just before the latest tool step, so "
                 "what the tool set off is not missed. Stops at the first failed step unless continueOnError. "
                 "At most 100 steps and ten minutes.",
                 schema, Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "scenario");
            const Json steps = args.Raw().value("steps", Json());
            if (!steps.is_array() || steps.empty() || steps.size() > 100)
            {
                throw ToolError("scenario: `steps` is a list of 1 to 100 steps; see the schema for their kinds");
            }
            const bool continueOnError = args.Bool("continueOnError", false);
            const auto start = Clock::now();
            const auto deadline = start + std::chrono::minutes(10);
            std::map<std::string, Json, std::less<>> named;
            std::uint64_t cursor = bench.Events().Next(); // before the latest tool step
            Json transcript = Json::array();
            bool ok = true;

            for (std::size_t index = 0; index < steps.size(); ++index)
            {
                const Json &step = steps[index];
                const auto stepStart = Clock::now();
                const std::string kind = step.is_object() ? KindOf(step) : std::string();
                Json line = {{"index", index}, {"kind", kind.empty() ? Json(nullptr) : Json(kind)}};
                Json answer;
                std::string failure;
                try
                {
                    if (Clock::now() > deadline)
                    {
                        throw ToolError("the scenario ran past its ten minutes");
                    }
                    if (kind == "tool")
                    {
                        const auto &tool = step.at("tool");
                        if (!tool.is_string() || tool == "scenario")
                        {
                            throw ToolError("`tool` is a tool's name (not scenario)");
                        }
                        line["tool"] = tool;
                        cursor = bench.Events().Next();
                        answer = bench.Invoke(tool.get<std::string>(), step.value("args", Json::object())).data;
                        line["result"] = answer;
                    }
                    else if (kind == "waitFor")
                    {
                        const auto filter = EventFilter::FromArgs(step.at("waitFor"));
                        const Args stepArgs(step, "scenario");
                        const auto since = static_cast<std::uint64_t>(stepArgs.Integer(
                            "since", static_cast<std::int64_t>(cursor), 0, std::numeric_limits<std::int64_t>::max()));
                        const auto timeout = std::chrono::milliseconds(stepArgs.Integer("timeoutMs", 30000, 0, 300000));
                        if (auto event = bench.Events().WaitFor(since, filter, timeout))
                        {
                            answer = event->ToJson();
                            line["event"] = answer;
                            cursor = event->seq + 1; // a second wait looks past this one
                        }
                        else
                        {
                            failure = std::format("no such event within {} ms", timeout.count());
                        }
                    }
                    else if (kind == "sleepMs")
                    {
                        const Args stepArgs(step, "scenario");
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(stepArgs.Integer("sleepMs", 0, 0, 60000)));
                    }
                    else if (kind == "expect")
                    {
                        const Json &expect = step.at("expect");
                        if (!expect.is_object() || !expect.contains("from") || !expect["from"].is_string())
                        {
                            throw ToolError("`expect` needs `from`, the label of an earlier step");
                        }
                        const auto from = named.find(expect["from"].get<std::string>());
                        if (from == named.end())
                        {
                            throw ToolError(
                                std::format("no earlier step is labelled \"{}\"", expect["from"].get<std::string>()));
                        }
                        const std::string pointer = expect.value("pointer", "");
                        const auto actual = At(from->second, pointer);
                        line["actual"] = actual ? *actual : Json(nullptr);
                        failure = Check(expect, actual);
                    }
                    else
                    {
                        throw ToolError("a step is one of tool, waitFor, sleepMs or expect");
                    }
                }
                catch (const UnknownTool &e)
                {
                    failure = e.what();
                }
                catch (const ToolError &e)
                {
                    failure = e.what();
                }
                catch (const FrameTimeout &e)
                {
                    failure = std::string(e.what()) + "; the game's frame is not running";
                }
                catch (const std::exception &e)
                {
                    failure = std::string("failed inside the bench: ") + e.what();
                }
                if (const auto label = step.is_object() ? step.find("as") : step.end();
                    label != step.end() && label->is_string() && failure.empty())
                {
                    named[label->get<std::string>()] = answer;
                }
                line["ok"] = failure.empty();
                if (!failure.empty())
                {
                    line["error"] = failure;
                    ok = false;
                }
                line["ms"] = MsSince(stepStart);
                transcript.push_back(std::move(line));
                if (!failure.empty() && !continueOnError)
                {
                    break;
                }
            }
            return Json{{"ok", ok},
                        {"stepsRun", transcript.size()},
                        {"elapsedMs", MsSince(start)},
                        {"steps", std::move(transcript)}};
        });
}

} // namespace eluvian
