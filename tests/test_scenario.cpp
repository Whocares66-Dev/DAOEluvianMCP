#include "core/Scenario.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A bench whose tools answer at once, on the listener: `attack` emits the
// turn's end before it answers, as a refusal in the same frame would.
struct Fixture
{
    Bench bench;
    std::atomic<int> attacks{0};

    Fixture()
    {
        AddEventTools(bench);
        AddScenarioTool(bench);
        bench.Tools().Add(ToolSpec{"attack", "Attack.", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            ++attacks;
            bench.Events().Emit("turnbased.turn.ended", Json{{"id", args.value("id", "0x82")}, {"reason", "done"}});
            return Json{{"given", true}, {"target", args.value("target", "")}};
        });
        bench.Tools().Add(ToolSpec{"health", "Health.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { return Json{{"current", 40}, {"max", 146}}; });
    }

    Json Run(const Json &args)
    {
        return bench.Invoke("scenario", args).data;
    }
};

} // namespace

TEST_CASE("a wait after a tool sees the event the tool set off before it answered", "[scenario]")
{
    Fixture f;
    const auto answer =
        f.Run(Json{{"steps",
                    {{{"tool", "attack"}, {"args", {{"target", "0x7d9"}}}, {"as", "order"}},
                     {{"waitFor", {{"topic", "turnbased.turn.*"}}}, {"timeoutMs", 1000}, {"as", "end"}},
                     {{"expect", {{"from", "order"}, {"pointer", "/given"}, {"equals", true}}}},
                     {{"expect", {{"from", "end"}, {"pointer", "/payload/reason"}, {"equals", "done"}}}}}}});
    CHECK(answer["ok"] == true);
    CHECK(answer["stepsRun"] == 4);
    CHECK(answer["steps"][0]["result"]["target"] == "0x7d9");
    CHECK(answer["steps"][1]["event"]["topic"] == "turnbased.turn.ended");
}

TEST_CASE("a second wait looks past the first one's event", "[scenario]")
{
    Fixture f;
    const auto answer = f.Run(Json{{"steps",
                                    {{{"tool", "attack"}},
                                     {{"waitFor", {{"topic", "turnbased.*"}}}, {"timeoutMs", 1000}},
                                     {{"waitFor", {{"topic", "turnbased.*"}}}, {"timeoutMs", 20}}}}});
    CHECK(answer["ok"] == false);
    CHECK(answer["steps"][2]["ok"] == false);
    CHECK_THAT(answer["steps"][2]["error"].get<std::string>(), ContainsSubstring("no such event within 20 ms"));
}

TEST_CASE("a failed step stops the run, unless told to go on", "[scenario]")
{
    Fixture f;
    const Json steps = {{{"tool", "health"}, {"as", "h"}},
                        {{"expect", {{"from", "h"}, {"pointer", "/current"}, {"atLeast", 100}}}},
                        {{"tool", "attack"}}};
    auto answer = f.Run(Json{{"steps", steps}});
    CHECK(answer["ok"] == false);
    CHECK(answer["stepsRun"] == 2);
    CHECK_THAT(answer["steps"][1]["error"].get<std::string>(), ContainsSubstring("atLeast 100; found 40"));
    CHECK(f.attacks == 0);

    answer = f.Run(Json{{"steps", steps}, {"continueOnError", true}});
    CHECK(answer["stepsRun"] == 3);
    CHECK(f.attacks == 1);
}

TEST_CASE("each expectation compares as it says", "[scenario]")
{
    Fixture f;
    const auto check = [&f](const Json &expect) {
        return f.Run(Json{{"steps", {{{"tool", "health"}, {"as", "h"}}, {{"expect", expect}}}}})["ok"].get<bool>();
    };
    CHECK(check(Json{{"from", "h"}, {"pointer", "/max"}, {"equals", 146}}));
    CHECK_FALSE(check(Json{{"from", "h"}, {"pointer", "/max"}, {"equals", 145}}));
    CHECK(check(Json{{"from", "h"}, {"pointer", "/max"}, {"notEquals", 0}}));
    CHECK(check(Json{{"from", "h"}, {"pointer", "/current"}, {"atMost", 40}}));
    CHECK_FALSE(check(Json{{"from", "h"}, {"pointer", "/current"}, {"atMost", 39}}));
    CHECK(check(Json{{"from", "h"}, {"pointer", "/missing"}, {"exists", false}}));
    CHECK_FALSE(check(Json{{"from", "h"}, {"pointer", "/current"}, {"exists", false}}));
    CHECK(check(Json{{"from", "h"}, {"pointer", ""}, {"exists", true}}));
}

TEST_CASE("mistakes in the steps are failed steps that say what is wrong", "[scenario]")
{
    Fixture f;
    const auto errorOf = [&f](const Json &step) {
        return f.Run(Json{{"steps", {step}}})["steps"][0]["error"].get<std::string>();
    };
    CHECK_THAT(errorOf(Json{{"tool", "nothing"}}), ContainsSubstring("no tool \"nothing\""));
    CHECK_THAT(errorOf(Json{{"tool", "scenario"}}), ContainsSubstring("not scenario"));
    CHECK_THAT(errorOf(Json{{"dance", true}}), ContainsSubstring("one of tool, waitFor, sleepMs or expect"));
    CHECK_THAT(errorOf(Json{{"expect", {{"from", "nobody"}}}}), ContainsSubstring("no earlier step is labelled"));
    CHECK_THAT(errorOf(Json{{"expect", {{"from", "x"}}}}), ContainsSubstring("labelled"));
    CHECK_THROWS_AS(f.Run(Json{{"steps", Json::array()}}), ToolError);
    CHECK_THROWS_AS(f.Run(Json::object()), ToolError);
}

TEST_CASE("a step's label holds only an answer that came", "[scenario]")
{
    Fixture f;
    const auto answer = f.Run(Json{{"steps",
                                    {{{"waitFor", {{"topic", "never"}}}, {"timeoutMs", 0}, {"as", "w"}},
                                     {{"expect", {{"from", "w"}, {"exists", true}}}}}},
                                   {"continueOnError", true}});
    CHECK_THAT(answer["steps"][1]["error"].get<std::string>(), ContainsSubstring("no earlier step is labelled"));
}

TEST_CASE("waits and sleeps are cut to the scenario's budget, and the run says it timed out", "[scenario]")
{
    Fixture f;
    const auto start = std::chrono::steady_clock::now();
    auto answer =
        f.Run(Json{{"budgetMs", 80},
                   {"steps", {{{"waitFor", {{"topic", "never"}}}, {"timeoutMs", 60000}}, {{"tool", "attack"}}}},
                   {"continueOnError", true}});
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(answer["ok"] == false);
    CHECK(answer["timedOut"] == true);
    CHECK(answer["stepsRun"] == 1); // even told to go on: there is no time to go on in
    CHECK_THAT(answer["steps"][0]["error"].get<std::string>(), ContainsSubstring("time ran out"));
    CHECK(f.attacks == 0);

    answer = f.Run(Json{{"budgetMs", 50}, {"steps", {{{"sleepMs", 60000}}}}});
    CHECK(answer["timedOut"] == true);
    CHECK_THAT(answer["steps"][0]["error"].get<std::string>(), ContainsSubstring("into a sleep of 60000 ms"));
}

TEST_CASE("a run within its budget does not time out", "[scenario]")
{
    Fixture f;
    const auto answer = f.Run(Json{{"budgetMs", 5000}, {"steps", {{{"tool", "attack"}}, {{"sleepMs", 5}}}}});
    CHECK(answer["ok"] == true);
    CHECK(answer["timedOut"] == false);
    CHECK_THROWS_AS(f.Run(Json{{"budgetMs", 0}, {"steps", {{{"tool", "attack"}}}}}), ToolError);
}
