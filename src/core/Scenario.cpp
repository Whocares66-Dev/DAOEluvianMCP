#include "core/Scenario.h"

#include "core/Args.h"

#include <algorithm>
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

void AddScenarioTool(Bench &bench, std::function<Json()> whyNothing)
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
          {"continueOnError", {{"type", "boolean"}, {"description", "Run on past a failed step; default false"}}},
          {"budgetMs",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 600000},
            {"description", "The time the whole run may take; ten minutes by default and at most"}}}}},
        {"required", {"steps"}}};
    bench.Tools().Add(
        ToolSpec{"scenario",
                 "Run a list of steps in one call and get a transcript: call tools, wait for events, pause, check "
                 "answers. A test's whole checklist step at once: order an attack, wait for the turn to end, check "
                 "the target's health. A wait starts from the cursor taken just before the latest tool step, so "
                 "what the tool set off is not missed. Stops at the first failed step unless continueOnError. "
                 "At most 100 steps, within `budgetMs` (ten minutes by default and at most): waits and sleeps are "
                 "cut to the time left, and the run stops with `timedOut` when it is gone. A tool step already "
                 "running in the game is waited for to its end, since what it does cannot be taken back.",
                 schema, Thread::Listener},
        [&bench, whyNothing = std::move(whyNothing)](const Json &raw) -> ToolResult {
            const Args args(raw, "scenario");
            const Json steps = args.Raw().value("steps", Json());
            if (!steps.is_array() || steps.empty() || steps.size() > 100)
            {
                throw ToolError("scenario: `steps` is a list of 1 to 100 steps; see the schema for their kinds");
            }
            const bool continueOnError = args.Bool("continueOnError", false);
            const auto start = Clock::now();
            const auto budget = std::chrono::milliseconds(args.Integer("budgetMs", 600000, 1, 600000));
            const auto deadline = start + budget;
            // What is left of the budget, never less than nothing. Waits and
            // sleeps are cut to it: checked only between steps, a wait begun
            // just inside ten minutes ran five more (a review's finding).
            const auto left = [deadline] {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                return std::max(remaining, std::chrono::milliseconds(0));
            };
            bool timedOut = false;
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
                    if (left().count() == 0)
                    {
                        timedOut = true;
                        throw ToolError(std::format("the scenario's {} ms ran out before this step", budget.count()));
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
                        if (left().count() == 0)
                        {
                            // A call the game was already running is waited
                            // for; it happened, and the run ends here.
                            timedOut = true;
                            line["note"] = "this step ran past the scenario's time; what it did stands";
                        }
                    }
                    else if (kind == "waitFor")
                    {
                        const auto filter = EventFilter::FromArgs(step.at("waitFor"));
                        const Args stepArgs(step, "scenario");
                        const auto since = static_cast<std::uint64_t>(stepArgs.Integer(
                            "since", static_cast<std::int64_t>(cursor), 0, std::numeric_limits<std::int64_t>::max()));
                        const auto asked = std::chrono::milliseconds(stepArgs.Integer("timeoutMs", 30000, 0, 300000));
                        const auto timeout = std::min(asked, left());
                        if (auto event = bench.Events().WaitFor(since, filter, timeout))
                        {
                            answer = event->ToJson();
                            line["event"] = answer;
                            cursor = event->seq + 1; // a second wait looks past this one
                        }
                        else if (timeout < asked)
                        {
                            timedOut = true;
                            failure = std::format("the scenario's time ran out {} ms into a wait of {} ms",
                                                  timeout.count(), asked.count());
                        }
                        else
                        {
                            failure = std::format("no such event within {} ms", timeout.count());
                            // What the host knows of why: a paused game plays
                            // nothing, and a wait in one only times out.
                            if (whyNothing)
                            {
                                if (const Json why = whyNothing(); why.is_object() && !why.empty())
                                {
                                    line["why"] = why;
                                    if (why.contains("note") && why.at("note").is_string())
                                    {
                                        failure += "; " + why.at("note").get<std::string>();
                                    }
                                }
                            }
                        }
                    }
                    else if (kind == "sleepMs")
                    {
                        const Args stepArgs(step, "scenario");
                        const auto asked = std::chrono::milliseconds(stepArgs.Integer("sleepMs", 0, 0, 60000));
                        const auto nap = std::min(asked, left());
                        std::this_thread::sleep_for(nap);
                        if (nap < asked)
                        {
                            timedOut = true;
                            failure = std::format("the scenario's time ran out {} ms into a sleep of {} ms",
                                                  nap.count(), asked.count());
                        }
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
                if (timedOut || (!failure.empty() && !continueOnError))
                {
                    break;
                }
            }
            return Json{{"ok", ok && !timedOut},
                        {"timedOut", timedOut},
                        {"stepsRun", transcript.size()},
                        {"elapsedMs", MsSince(start)},
                        {"steps", std::move(transcript)}};
        });
}

} // namespace eluvian
