#include "core/Ready.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// A tool's answer, or nothing where the call failed: a frame tool's while
// frames stop (an area loading) is not yet, not a failure of the wait.
std::optional<Json> Ask(Bench &bench, const std::string &tool, const Json &args)
{
    try
    {
        return bench.Invoke(tool, args).data;
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

// What the game takes input as now, or nothing while it does not: a
// cutscene, a load, a movie, a spoken line in a conversation.
std::optional<std::string> TakesInput(Bench &bench, const std::string &mode)
{
    if (mode == "explore" || mode == "combat")
    {
        return mode;
    }
    if (mode == "dead")
    {
        return std::string("dead: the party is down (a load, or the game's own menu)");
    }
    if (mode == "pregame")
    {
        return std::string("the main menu");
    }
    if (mode == "dialog")
    {
        const auto talk = Ask(bench, "inspect", Json{{"kind", "dialogue"}});
        if (talk && talk->value("replies", Json::array()).is_array() && !talk->value("replies", Json::array()).empty())
        {
            return std::string("a conversation at a choice");
        }
        return std::nullopt;
    }
    if (mode == "gui")
    {
        const auto gui = Ask(bench, "inspect", Json{{"kind", "gui"}});
        if (!gui)
        {
            return std::nullopt;
        }
        if (gui->contains("popup") && !gui->at("popup").is_null())
        {
            return std::string("a message box");
        }
        if (gui->contains("fullScreen") && gui->at("fullScreen").is_string())
        {
            return "the " + gui->at("fullScreen").get<std::string>() + " screen";
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// What passed while waiting: the modes gone through, saves and loads.
Json Passed(Bench &bench, std::uint64_t since)
{
    EventFilter filter;
    filter.topics = {"eluvian.mode.changed", "eluvian.game.saved", "eluvian.game.loaded"};
    Json passed = Json::array();
    for (const auto &event : bench.Events().Since(since, filter, 200).events)
    {
        if (event.topic == "eluvian.mode.changed")
        {
            passed.push_back(Json{{"mode", event.payload.value("to", std::string())}});
        }
        else
        {
            passed.push_back(Json{{event.topic == "eluvian.game.saved" ? "saved" : "loaded", event.payload}});
        }
    }
    return passed;
}

} // namespace

void AddWaitReadyTool(Bench &bench)
{
    const Json schema = {
        {"type", "object"},
        {"properties",
         {{"timeoutMs",
           {{"type", "integer"}, {"minimum", 0}, {"maximum", 600000}, {"description", "60000 by default"}}},
          {"intervalMs",
           {{"type", "integer"}, {"minimum", 50}, {"maximum", 5000}, {"description", "250 by default"}}}}}};
    bench.Tools().Add(
        ToolSpec{"wait_ready",
                 "Wait until the game takes input again: after an exit, a load, a fight's or a conversation's end, "
                 "a cutscene. Answers when the game is in explore or combat, the party is dead, the main menu is "
                 "up, a conversation is at a choice, a message box is up, or a full-screen interface (a picker, a "
                 "map, a store); `ready`, the `mode`, `what` takes the input, and `passed`, the modes, saves and "
                 "loads gone through on the way. `ready` false when `timeoutMs` (60000 by default) ran out first.",
                 schema, Thread::Listener, true},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "wait_ready");
            const auto timeout = milliseconds(args.Integer("timeoutMs", 60000, 0, 600000));
            const auto interval = milliseconds(args.Integer("intervalMs", 250, 50, 5000));
            const auto since = bench.Events().Next();
            const auto start = Clock::now();
            const auto deadline = start + timeout;
            Json answer = {{"ready", false}};
            while (true)
            {
                const auto health = Ask(bench, "health", Json::object());
                const std::string mode = health ? health->value("mode", std::string()) : std::string();
                answer["mode"] = mode;
                if (const auto what = TakesInput(bench, mode))
                {
                    answer["ready"] = true;
                    answer["what"] = *what;
                    break;
                }
                const auto now = Clock::now();
                if (now >= deadline)
                {
                    break;
                }
                std::this_thread::sleep_for(
                    std::min(interval, std::chrono::duration_cast<milliseconds>(deadline - now)));
            }
            answer["passed"] = Passed(bench, since);
            answer["waitedMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            return answer;
        });
}

} // namespace eluvian
