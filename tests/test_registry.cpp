#include "core/ToolRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace eluvian;

namespace
{

ToolHandler Answer(Json value)
{
    return [value = std::move(value)](const Json &) -> ToolResult { return value; };
}

} // namespace

TEST_CASE("a tool resolves to its handler and thread", "[registry]")
{
    ToolRegistry registry;
    REQUIRE(registry.Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true}, Answer(Json{{"ok", true}})));
    const auto call = registry.Resolve("health");
    REQUIRE(call);
    CHECK(call->tool == "health");
    CHECK(call->thread == Thread::Listener);
    CHECK(call->handler(Json::object()).data == Json{{"ok", true}});
    CHECK_FALSE(registry.Resolve("nothing"));
}

TEST_CASE("an unlisted tool is called but not listed, and its name is taken", "[registry]")
{
    ToolRegistry registry;
    registry.Add(ToolSpec{"dialogue", "Answer the conversation.", {}, Thread::Listener}, Answer(1));
    ToolSpec step{"dialogue.step", "One act of it.", {}, Thread::Frame};
    step.listed = false;
    registry.Add(step, Answer(2));
    REQUIRE(registry.Resolve("dialogue.step"));
    CHECK(registry.Resolve("dialogue.step")->handler({}).data == 2);
    const auto listed = registry.Describe();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0]["name"] == "dialogue");
    CHECK(registry.Names() == std::vector<std::string>{"dialogue", "dialogue.step"});
}

TEST_CASE("adding a tool of the same name replaces it and says so", "[registry]")
{
    ToolRegistry registry;
    CHECK(registry.Add(ToolSpec{"t", "first"}, Answer(1)));
    CHECK_FALSE(registry.Add(ToolSpec{"t", "second"}, Answer(2)));
    CHECK(registry.Resolve("t")->handler({}).data == 2);
    CHECK(registry.Describe().size() == 1);
}

TEST_CASE("the description lists each tool with its own schema, sorted, read-only ones marked", "[registry]")
{
    ToolRegistry registry;
    registry.Add(ToolSpec{"order", "Give an order.", Json{{"type", "object"}, {"required", Json::array({"action"})}}},
                 Answer(0));
    registry.Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true}, Answer(0));

    const Json tools = registry.Describe();
    REQUIRE(tools.size() == 2);
    CHECK(tools[0]["name"] == "health"); // sorted
    CHECK(tools[0]["annotations"]["readOnlyHint"] == true);
    CHECK(tools[1]["description"] == "Give an order.");
    CHECK(tools[1]["inputSchema"]["required"] == Json::array({"action"}));
    CHECK_FALSE(tools[1].contains("annotations"));
}

TEST_CASE("a tool needs a handler", "[registry]")
{
    ToolRegistry registry;
    CHECK_THROWS_AS(registry.Add(ToolSpec{"inspect", "Read the game."}, nullptr), std::invalid_argument);
    CHECK_FALSE(registry.Resolve("inspect"));
}

TEST_CASE("every change bumps the revision", "[registry]")
{
    ToolRegistry registry;
    const auto start = registry.Revision();
    registry.Add(ToolSpec{"state", "The game."}, Answer(0));
    registry.Add(ToolSpec{"state", "The game, again."}, Answer(1));
    CHECK(registry.Revision() == start + 2);
}

TEST_CASE("a tool's budget is listed in its _meta, and none where it declares none", "[registry]")
{
    ToolRegistry registry;
    registry.Add(Budgeted(ToolSpec{"travel", "Walk.", {}, Thread::Listener}, "budgetMs", 60000), Answer(1));
    registry.Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true}, Answer(1));
    const Json tools = registry.Describe();
    REQUIRE(tools.size() == 2);
    const Json &health = tools[0].at("name") == "health" ? tools[0] : tools[1];
    const Json &travel = tools[0].at("name") == "travel" ? tools[0] : tools[1];
    CHECK(travel.at("_meta").at("eluvian/budget") == Json{{"argument", "budgetMs"}, {"defaultMs", 60000}});
    CHECK_FALSE(health.contains("_meta"));
}
