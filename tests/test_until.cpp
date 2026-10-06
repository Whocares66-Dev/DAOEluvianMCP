#include "core/Scenario.h"
#include "core/Until.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A bench whose `health` rises by 10 at each call from 40, and whose
// `loading` fails twice (a frame not taken as an area loads) before it
// answers ready.
struct Rising
{
    Bench bench;
    std::atomic<int> health{40};
    std::atomic<int> calls{0};
    std::atomic<int> fails{2};

    Rising()
    {
        AddWaitUntilTool(bench);
        AddScenarioTool(bench);
        bench.Tools().Add(ToolSpec{"health", "Health.", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            ++calls;
            const int now = health.fetch_add(10);
            return Json{{"health", {{"current", now}, {"max", 100}}}};
        });
        bench.Tools().Add(ToolSpec{"loading", "Loading.", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            if (fails.fetch_sub(1) > 0)
            {
                throw ToolError("no frame took the call within 2000 ms");
            }
            return Json{{"mode", "explore"}};
        });
    }

    Json Wait(const Json &args)
    {
        return bench.Invoke("wait_until", args).data;
    }
};

} // namespace

TEST_CASE("wait_until calls a tool until the value it points at meets the expectation", "[until]")
{
    Rising world;
    const auto answer = world.Wait(Json{
        {"tool", "health"}, {"pointer", "/health/current"}, {"atLeast", 70}, {"intervalMs", 50}, {"timeoutMs", 5000}});
    CHECK(answer["held"] == true);
    CHECK(answer["value"] == 70);
    CHECK(answer["tries"] == 4); // 40, 50, 60, 70
    CHECK_FALSE(answer.contains("unmet"));
}

TEST_CASE("wait_until answers the last value and what it lacked when the time runs out", "[until]")
{
    Rising world;
    const auto answer = world.Wait(Json{
        {"tool", "health"}, {"pointer", "/health/current"}, {"equals", 45}, {"intervalMs", 50}, {"timeoutMs", 200}});
    CHECK(answer["held"] == false);
    CHECK(answer["tries"].get<int>() >= 2);
    CHECK(answer["value"].get<int>() >= 50);
    CHECK_THAT(answer["unmet"].get<std::string>(), ContainsSubstring("expected 45"));
}

TEST_CASE("a call that fails is not yet: wait_until asks again", "[until]")
{
    Rising world;
    const auto answer = world.Wait(Json{
        {"tool", "loading"}, {"pointer", "/mode"}, {"equals", "explore"}, {"intervalMs", 50}, {"timeoutMs", 5000}});
    CHECK(answer["held"] == true);
    CHECK(answer["tries"] == 3);
    CHECK_THAT(answer["lastError"].get<std::string>(), ContainsSubstring("no frame"));
}

TEST_CASE("wait_until refuses a tool there is not, and itself", "[until]")
{
    Rising world;
    CHECK_THROWS_AS(world.Wait(Json{{"tool", "nothere"}, {"pointer", "/x"}, {"exists", true}}), UnknownTool);
    CHECK_THROWS_WITH(world.Wait(Json{{"tool", "wait_until"}, {"pointer", "/x"}, {"exists", true}}),
                      ContainsSubstring("not wait_until or scenario"));
    CHECK_THROWS_WITH(world.Wait(Json{{"tool", "health"}, {"pointer", "health"}, {"exists", true}}),
                      ContainsSubstring("is not a JSON pointer"));
    CHECK(world.calls == 0); // a bad pointer refused before any call
}

TEST_CASE("a scenario's until step waits as wait_until does, and fails as its expectation does", "[until]")
{
    Rising world;
    const auto held =
        world.bench
            .Invoke(
                "scenario",
                Json{{"steps",
                      {Json{{"until",
                             {{"tool", "health"}, {"pointer", "/health/current"}, {"atLeast", 60}, {"intervalMs", 50}}},
                            {"timeoutMs", 5000},
                            {"as", "back"}},
                       Json{{"expect", {{"from", "back"}, {"pointer", "/held"}, {"equals", true}}}}}}})
            .data;
    CHECK(held["ok"] == true);
    CHECK(held["steps"][0]["until"]["value"] == 60);
    const auto missed =
        world.bench
            .Invoke(
                "scenario",
                Json{{"steps",
                      {Json{{"until",
                             {{"tool", "health"}, {"pointer", "/health/current"}, {"equals", 0}, {"intervalMs", 50}}},
                            {"timeoutMs", 200}}}}})
            .data;
    CHECK(missed["ok"] == false);
    CHECK_THAT(missed["steps"][0]["error"].get<std::string>(), ContainsSubstring("did not hold within 200 ms"));
}
