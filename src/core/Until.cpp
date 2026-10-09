#include "core/Until.h"

#include "core/Args.h"
#include "core/Expect.h"

#include <algorithm>
#include <string>
#include <thread>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr std::int64_t kMostWaitMs = 300000;

} // namespace

Json PollUntil(Bench &bench, const Json &until, milliseconds budget)
{
    if (!until.is_object())
    {
        throw ToolError("wait_until: give {\"tool\", \"args\", \"pointer\", and equals, notEquals, exists, atLeast "
                        "or atMost}");
    }
    const Args args(until, "wait_until");
    const std::string tool = args.String("tool");
    if (tool == "wait_until" || tool == "scenario")
    {
        throw ToolError("wait_until: `tool` is a tool to ask, not wait_until or scenario");
    }
    const Json toolArgs = args.Has("args") ? args.Raw().at("args") : Json::object();
    const std::string pointer = args.String("pointer", "");
    // A pointer that is not one, refused before any call: caught with the
    // calls' failures, it was waited out as not yet (#85).
    (void)expect::At(Json::object(), pointer, "wait_until");
    const auto interval = milliseconds(args.Integer("intervalMs", 250, 50, 60000));
    const auto start = Clock::now();
    const auto deadline = start + budget;
    Json answer = {{"held", false}, {"tries", 0}};
    int tries = 0;
    while (true)
    {
        ++tries;
        try
        {
            const Json result = bench.Invoke(tool, toolArgs).data;
            const auto value = expect::At(result, pointer, "wait_until");
            answer["value"] = value ? *value : Json(nullptr);
            const std::string unmet = expect::Check(until, value);
            if (unmet.empty())
            {
                answer["held"] = true;
                answer.erase("unmet");
                break;
            }
            answer["unmet"] = unmet;
        }
        catch (const UnknownTool &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            // Not yet: a frame tool's call while an area loads, say.
            answer["lastError"] = e.what();
        }
        const auto now = Clock::now();
        if (now >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(std::min(interval, std::chrono::duration_cast<milliseconds>(deadline - now)));
    }
    answer["tries"] = tries;
    answer["waitedMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return answer;
}

void AddWaitUntilTool(Bench &bench)
{
    const Json schema = {
        {"type", "object"},
        {"properties",
         {{"tool", {{"type", "string"}, {"description", "The tool to ask, as it is called"}}},
          {"args", {{"type", "object"}, {"description", "Its arguments"}}},
          {"pointer",
           {{"type", "string"}, {"description", "Where in its answer, a JSON pointer: \"/health/current\""}}},
          {"equals", {{"description", "The value there equals this"}}},
          {"notEquals", {{"description", "The value there is anything but this"}}},
          {"exists", {{"type", "boolean"}, {"description", "There is, or is not, a value there"}}},
          {"atLeast", {{"type", "number"}}},
          {"atMost", {{"type", "number"}}},
          {"intervalMs", {{"type", "integer"}, {"minimum", 50}, {"maximum", 60000}, {"description", "250 by default"}}},
          {"timeoutMs",
           {{"type", "integer"}, {"minimum", 0}, {"maximum", kMostWaitMs}, {"description", "30000 by default"}}}}},
        {"required", {"tool"}}};
    bench.Tools().Add(
        Budgeted(
            ToolSpec{
                "wait_until",
                "Wait until a condition on a tool's answer holds: the tool is called every `intervalMs` until the "
                "value at `pointer` in its answer meets the expectation (equals, notEquals, exists, atLeast, "
                "atMost), or `timeoutMs` runs out. For a state no event marks: a creature dead, the party's health "
                "back, a mode gone. Answers `held`, the last `value`, `tries` and `waitedMs`; a call that failed "
                "(the game loading) counts as not yet. For an event, wait_for.",
                schema, Thread::Listener, true},
            "timeoutMs", 30000),
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "wait_until");
            const auto budget = milliseconds(args.Integer("timeoutMs", 30000, 0, kMostWaitMs));
            return PollUntil(bench, raw, budget);
        });
}

} // namespace eluvian
