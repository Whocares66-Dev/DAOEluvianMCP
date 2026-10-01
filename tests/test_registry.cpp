#include "core/ToolRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

ToolHandler Answer(Json value)
{
    return [value](const Json &) -> ToolResult { return value; };
}

ToolSpec Inspect()
{
    return ToolSpec{"inspect", "Read the game.", Json{{"type", "object"}}, Thread::Frame, true};
}

} // namespace

TEST_CASE("a tool resolves to its handler and thread", "[registry]")
{
    ToolRegistry registry;
    REQUIRE(registry.Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true}, Answer(Json{{"ok", true}})));
    const auto call = registry.Resolve("health", Json::object());
    REQUIRE(call);
    CHECK(call->tool == "health");
    CHECK(call->kind.empty());
    CHECK(call->thread == Thread::Listener);
    CHECK(call->handler(Json::object()).data == Json{{"ok", true}});
    CHECK_FALSE(registry.Resolve("nothing", Json::object()));
}

TEST_CASE("adding a tool of the same name replaces it and says so", "[registry]")
{
    ToolRegistry registry;
    CHECK(registry.Add(ToolSpec{"t", "first"}, Answer(1)));
    CHECK_FALSE(registry.Add(ToolSpec{"t", "second"}, Answer(2)));
    CHECK(registry.Resolve("t", {})->handler({}).data == 2);
    CHECK(registry.Describe().size() == 1);
}

TEST_CASE("a base tool dispatches on `kind`, each kind on its own thread", "[registry]")
{
    ToolRegistry registry;
    registry.Add(Inspect());
    REQUIRE(registry.AddKind("inspect", KindSpec{"game", "The game's state.", Thread::Frame}, Answer("game")));
    REQUIRE(registry.AddKind("inspect", KindSpec{"turnbased.state", "The fight.", Thread::Listener}, Answer("fight")));
    const auto call = registry.Resolve("inspect", Json{{"kind", "turnbased.state"}});
    REQUIRE(call);
    CHECK(call->kind == "turnbased.state");
    CHECK(call->thread == Thread::Listener);
    CHECK(call->handler({}).data == "fight");
    CHECK_FALSE(registry.AddKind("inspect", KindSpec{"game", "Again."}, Answer("again")));
}

TEST_CASE("a missing or unknown kind names the kinds there are", "[registry]")
{
    ToolRegistry registry;
    registry.Add(Inspect());
    registry.AddKind("inspect", KindSpec{"game", "The game's state."}, Answer(0));
    registry.AddKind("inspect", KindSpec{"creature", "One creature."}, Answer(0));
    CHECK_THROWS_WITH(registry.Resolve("inspect", Json::object()), ContainsSubstring("creature, game"));
    CHECK_THROWS_WITH(registry.Resolve("inspect", Json{{"kind", "party"}}),
                      ContainsSubstring("no kind \"party\"") && ContainsSubstring("creature, game"));
    CHECK_THROWS_AS(registry.Resolve("inspect", Json{{"kind", 3}}), ToolError);
}

TEST_CASE("a kind needs a base tool without a handler", "[registry]")
{
    ToolRegistry registry;
    registry.Add(ToolSpec{"health", "Alive?"}, Answer(0));
    CHECK_THROWS_AS(registry.AddKind("nothing", KindSpec{"k", "d"}, Answer(0)), std::invalid_argument);
    CHECK_THROWS_AS(registry.AddKind("health", KindSpec{"k", "d"}, Answer(0)), std::invalid_argument);
}

TEST_CASE("the description writes each kind into the schema's enum and the text", "[registry]")
{
    ToolRegistry registry;
    registry.Add(Inspect());
    registry.Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true}, Answer(0));
    registry.Add(ToolSpec{"order", "Give an order."}, Answer(0));
    registry.AddKind("inspect", KindSpec{"turnbased.state", "The fight: whose turn, what is left."}, Answer(0));
    registry.AddKind("inspect", KindSpec{"game", "The game's state."}, Answer(0));

    const Json tools = registry.Describe();
    REQUIRE(tools.size() == 3);
    CHECK(tools[0]["name"] == "health"); // sorted
    CHECK(tools[0]["annotations"]["readOnlyHint"] == true);
    CHECK_FALSE(tools[2].contains("annotations"));

    const Json &inspect = tools[1];
    CHECK(inspect["inputSchema"]["properties"]["kind"]["enum"] == Json::array({"game", "turnbased.state"}));
    CHECK(inspect["inputSchema"]["required"] == Json::array({"kind"}));
    CHECK_THAT(inspect["description"].get<std::string>(),
               ContainsSubstring("- turnbased.state: The fight: whose turn, what is left."));
}

TEST_CASE("every change bumps the revision", "[registry]")
{
    ToolRegistry registry;
    const auto start = registry.Revision();
    registry.Add(Inspect());
    registry.AddKind("inspect", KindSpec{"game", "d"}, Answer(0));
    CHECK(registry.Revision() == start + 2);
}

TEST_CASE("a base tool may choose its kinds by another argument", "[registry]")
{
    ToolRegistry registry;
    ToolSpec stage{"stage", "Set up a test.", Json{{"type", "object"}}};
    stage.kindProperty = "action";
    registry.Add(stage);
    registry.AddKind("stage", KindSpec{"kill", "Kill a creature."}, Answer("killed"));
    registry.AddKind("stage", KindSpec{"mymod.arena", "A mod's fight."}, Answer("arena"));

    const auto call = registry.Resolve("stage", Json{{"action", "mymod.arena"}});
    REQUIRE(call);
    CHECK(call->handler({}).data == "arena");
    CHECK_THROWS_WITH(registry.Resolve("stage", Json{{"kind", "kill"}}), ContainsSubstring("needs `action`"));
    CHECK_THROWS_WITH(registry.Resolve("stage", Json{{"action", "dance"}}),
                      ContainsSubstring("no action \"dance\"") && ContainsSubstring("kill, mymod.arena"));

    const Json tool = registry.Describe()[0];
    CHECK(tool["inputSchema"]["properties"]["action"]["enum"] == Json::array({"kill", "mymod.arena"}));
    CHECK(tool["inputSchema"]["required"] == Json::array({"action"}));
    CHECK_FALSE(tool["inputSchema"]["properties"].contains("kind"));
    CHECK_THAT(tool["description"].get<std::string>(), ContainsSubstring("`action`:\n- kill: Kill a creature."));
}
