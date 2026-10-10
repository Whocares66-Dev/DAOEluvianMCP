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
         {{"tool",
           {{"type", "string"}, {"description", "The tool to call, by name (not `wait_until` or `scenario`)."}}},
          {"args", {{"type", "object"}, {"description", "The tool's arguments. Default: none."}}},
          {"pointer",
           {{"type", "string"},
            {"description", "Where to look in the tool's answer, as a JSON pointer like \"/health/current\". "
                            "Default \"\": the whole answer."}}},
          {"equals", {{"description", "Holds when the value equals this."}}},
          {"notEquals", {{"description", "Holds when the value is anything but this."}}},
          {"exists",
           {{"type", "boolean"},
            {"description", "Holds when there is (true) or is not (false) a value at `pointer`."}}},
          {"atLeast", {{"type", "number"}, {"description", "Holds when the value is a number at least this."}}},
          {"atMost", {{"type", "number"}, {"description", "Holds when the value is a number at most this."}}},
          {"intervalMs",
           {{"type", "integer"},
            {"minimum", 50},
            {"maximum", 60000},
            {"description", "How often to call the tool, in milliseconds. Default 250."}}},
          {"timeoutMs",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", kMostWaitMs},
            {"description", "How long to wait, in milliseconds. Default 30000."}}}}},
        {"required", {"tool"}}};
    bench.Tools().Add(
        Budgeted(
            ToolSpec{"wait_until",
                     "Waits until a condition on a tool's answer holds. It calls `tool` with `args` every `intervalMs` "
                     "until the value at `pointer` in the answer meets the expectation (`equals`, `notEquals`, "
                     "`exists`, `atLeast` or `atMost`), or `timeoutMs` runs out. Use it for a state no event marks: a "
                     "creature dead, the party's health back, a mode gone. To wait for an event, use `wait_for`.\n\n"
                     "The answer includes `held`, the last `value`, `tries` and `waitedMs`, and `unmet` or `lastError` "
                     "when the condition did not hold. A failed call (for example while the game loads) counts as not "
                     "yet.",
                     schema, Thread::Listener, true},
            "timeoutMs", 30000),
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "wait_until");
            const auto budget = milliseconds(args.Integer("timeoutMs", 30000, 0, kMostWaitMs));
            return PollUntil(bench, raw, budget);
        });
}

} // namespace eluvian
