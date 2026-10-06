#include "core/Scenario.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
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
        return std::make_optional(answer.at(path));
    }
    catch (const Json::exception &)
    {
        throw ToolError(std::format(R"(scenario: `pointer` "{}" is not a JSON pointer like "/given")", pointer));
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

// One scenario's run: its budget, the cursor taken before the latest tool
// step, the answers of labelled steps, and whether its time ran out. Split
// along its parts from the tool's one handler (#77).
class Run
{
  public:
    Run(Bench &bench, const std::function<Json()> &whyNothing, std::chrono::milliseconds budget)
        : bench_(bench), whyNothing_(whyNothing), budget_(budget), start_(Clock::now()), deadline_(start_ + budget),
          cursor_(bench.Events().Next())
    {
    }

    // The steps in order, stopping at the first failed one unless told to go
    // on, or when the time is gone.
    Json All(const Json &steps, bool continueOnError)
    {
        Json transcript = Json::array();
        bool ok = true;
        for (std::size_t index = 0; index < steps.size(); ++index)
        {
            Json line = Step(steps[index], index);
            const bool failed = !line.value("ok", false);
            ok = ok && !failed;
            transcript.push_back(std::move(line));
            if (timedOut_ || (failed && !continueOnError))
            {
                break;
            }
        }
        return Json{{"ok", ok && !timedOut_},
                    {"timedOut", timedOut_},
                    {"stepsRun", transcript.size()},
                    {"elapsedMs", MsSince(start_)},
                    {"steps", std::move(transcript)}};
    }

  private:
    // What is left of the budget, never less than nothing. Waits and
    // sleeps are cut to it: checked only between steps, a wait begun
    // just inside ten minutes ran five more (a review's finding).
    [[nodiscard]] std::chrono::milliseconds Left() const
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now());
        return std::max(remaining, std::chrono::milliseconds(0));
    }

    // One step, timed, its failure caught: its line of the transcript.
    Json Step(const Json &step, std::size_t index)
    {
        const auto stepStart = Clock::now();
        const std::string kind = step.is_object() ? KindOf(step) : std::string();
        Json line = {{"index", index}, {"kind", kind.empty() ? Json(nullptr) : Json(kind)}};
        Json answer;
        std::string failure;
        try
        {
            if (Left().count() == 0)
            {
                timedOut_ = true;
                throw ToolError(std::format("the scenario's {} ms ran out before this step", budget_.count()));
            }
            failure = Do(kind, step, line, answer);
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
            named_[label->get<std::string>()] = answer;
        }
        line["ok"] = failure.empty();
        if (!failure.empty())
        {
            line["error"] = failure;
        }
        line["ms"] = MsSince(stepStart);
        return line;
    }

    // The step's own work, by its kind: what failed, or nothing.
    std::string Do(const std::string &kind, const Json &step, Json &line, Json &answer)
    {
        if (kind == "tool")
        {
            CallTool(step, line, answer);
            return {};
        }
        if (kind == "waitFor")
        {
            return WaitFor(step, line, answer);
        }
        if (kind == "sleepMs")
        {
            return Sleep(step);
        }
        if (kind == "expect")
        {
            return Expect(step, line);
        }
        throw ToolError("a step is one of tool, waitFor, sleepMs or expect");
    }

    void CallTool(const Json &step, Json &line, Json &answer)
    {
        const auto &tool = step.at("tool");
        if (!tool.is_string() || tool == "scenario")
        {
            throw ToolError("`tool` is a tool's name (not scenario)");
        }
        line["tool"] = tool;
        cursor_ = bench_.Events().Next();
        answer = bench_.Invoke(tool.get<std::string>(), step.value("args", Json::object())).data;
        line["result"] = answer;
        if (Left().count() == 0)
        {
            // A call the game was already running is waited
            // for; it happened, and the run ends here.
            timedOut_ = true;
            line["note"] = "this step ran past the scenario's time; what it did stands";
        }
    }

    std::string WaitFor(const Json &step, Json &line, Json &answer)
    {
        const auto filter = EventFilter::FromArgs(step.at("waitFor"));
        const Args stepArgs(step, "scenario");
        const auto since = static_cast<std::uint64_t>(
            stepArgs.Integer("since", static_cast<std::int64_t>(cursor_), 0, std::numeric_limits<std::int64_t>::max()));
        const auto asked = std::chrono::milliseconds(stepArgs.Integer("timeoutMs", 30000, 0, 300000));
        const auto timeout = std::min(asked, Left());
        if (auto event = bench_.Events().WaitFor(since, filter, timeout))
        {
            answer = event->ToJson();
            line["event"] = answer;
            cursor_ = event->seq + 1; // a second wait looks past this one
            return {};
        }
        if (timeout < asked)
        {
            timedOut_ = true;
            return std::format("the scenario's time ran out {} ms into a wait of {} ms", timeout.count(),
                               asked.count());
        }
        std::string failure = std::format("no such event within {} ms", timeout.count());
        // What the host knows of why: a paused game plays
        // nothing, and a wait in one only times out.
        if (whyNothing_)
        {
            if (const Json why = whyNothing_(); why.is_object() && !why.empty())
            {
                line["why"] = why;
                if (why.contains("note") && why.at("note").is_string())
                {
                    failure += "; " + why.at("note").get<std::string>();
                }
            }
        }
        return failure;
    }

    std::string Sleep(const Json &step)
    {
        const Args stepArgs(step, "scenario");
        const auto asked = std::chrono::milliseconds(stepArgs.Integer("sleepMs", 0, 0, 60000));
        const auto nap = std::min(asked, Left());
        std::this_thread::sleep_for(nap);
        if (nap < asked)
        {
            timedOut_ = true;
            return std::format("the scenario's time ran out {} ms into a sleep of {} ms", nap.count(), asked.count());
        }
        return {};
    }

    std::string Expect(const Json &step, Json &line) const
    {
        const Json &expect = step.at("expect");
        if (!expect.is_object() || !expect.contains("from") || !expect["from"].is_string())
        {
            throw ToolError("`expect` needs `from`, the label of an earlier step");
        }
        const auto from = named_.find(expect["from"].get<std::string>());
        if (from == named_.end())
        {
            throw ToolError(std::format("no earlier step is labelled \"{}\"", expect["from"].get<std::string>()));
        }
        const std::string pointer = expect.value("pointer", "");
        const auto actual = At(from->second, pointer);
        line["actual"] = actual ? *actual : Json(nullptr);
        return Check(expect, actual);
    }

    Bench &bench_;
    const std::function<Json()> &whyNothing_;
    std::chrono::milliseconds budget_;
    Clock::time_point start_;
    Clock::time_point deadline_;
    std::uint64_t cursor_; // before the latest tool step
    bool timedOut_ = false;
    std::map<std::string, Json, std::less<>> named_;
};

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
            const auto budget = std::chrono::milliseconds(args.Integer("budgetMs", 600000, 1, 600000));
            return Run(bench, whyNothing, budget).All(steps, continueOnError);
        });
}

} // namespace eluvian
