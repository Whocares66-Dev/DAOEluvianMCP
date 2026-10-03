#include "core/Loot.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A chest and the tools around it, answering as the game's do: travel the
// walk, order the use (which opens the panel and ends on the bus), inspect
// kind=gui the panel, and game action=press its Take All.
struct Chest
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string panel;    // the container whose loot panel is up
    bool arrives = true;  // whether the walk there arrives
    bool opens = true;    // whether a use opens it
    bool starts = true;   // whether a use starts at all: on empty remains it does not
    bool full = false;    // whether the backpack is full
    Json popup = nullptr; // a message box the use brings instead
    Json items = Json::array({Json{{"id", "0x500"}, {"name", "Greenstone"}}});
    std::vector<std::string> calls; // the tools called, in order

    Chest()
    {
        bench.Tools().Add(ToolSpec{"travel", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            calls.emplace_back("travel");
            return Json{{"arrived", arrives},
                        {"reason", arrives ? "there" : "a fight began"},
                        {"distance", 1.0},
                        {"opened", Json::array()}};
        });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            calls.emplace_back("order " + args.value("action", ""));
            const auto cursor = bench.Events().Next();
            if (opens)
            {
                panel = args.value("target", "");
            }
            if (starts)
            {
                bench.Events().Emit("eluvian.command.ended",
                                    Json{{"id", "0x82"}, {"type", "UseObject"}, {"result", "success"}});
            }
            return Json{{"given", true}, {"creature", {{"id", "0x82"}}}, {"cursor", cursor}};
        });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (args.value("kind", "") == "inventory")
            {
                return Json{{"count", items.size()}, {"items", items}};
            }
            return Json{{"mode", panel.empty() ? "explore" : "gui"},
                        {"popup", calls.empty() || calls.back() != "order use" ? Json(nullptr) : popup},
                        {"loot", panel.empty() ? Json(nullptr) : Panel()}};
        });
        bench.Tools().Add(ToolSpec{"game", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            calls.emplace_back("game " + args.value("action", "") + " " + args.value("button", ""));
            if (panel.empty())
            {
                throw ToolError("game: no message box and no loot panel is up");
            }
            Json answer = {{"container", {{"id", panel}}},
                           {"taken", full ? Json::array() : items},
                           {"left", full ? items : Json::array()},
                           {"full", full}};
            if (!full)
            {
                panel.clear();
            }
            answer["panel"] = panel.empty() ? Json(nullptr) : Panel();
            return answer;
        });
        AddLootTool(bench);
    }

    [[nodiscard]] Json Panel() const
    {
        return Json{{"container", {{"id", panel}}}, {"items", items}};
    }

    Json Loot(Json args)
    {
        args["budgetMs"] = 5000;
        return bench.Invoke("loot", args).data;
    }
};

} // namespace

TEST_CASE("a container is walked to, opened and emptied, and its panel closes", "[loot]")
{
    Chest chest;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["closed"] == true);
    CHECK(answer["container"]["id"] == "0x3ec");
    REQUIRE(answer["taken"].size() == 1);
    CHECK(answer["taken"][0]["name"] == "Greenstone");
    CHECK(answer["left"].empty());
    CHECK(answer["walk"]["arrived"] == true);
    CHECK(chest.calls == std::vector<std::string>{"travel", "order use", "game press Take All"});
    CHECK(chest.panel.empty());
}

TEST_CASE("a panel up already is taken from, with no walk and no use", "[loot]")
{
    Chest chest;
    chest.panel = "0x3ec";
    const auto answer = chest.Loot(Json::object());
    CHECK(answer["looted"] == true);
    CHECK(answer["taken"].size() == 1);
    CHECK(chest.calls == std::vector<std::string>{"game press Take All"});
}

TEST_CASE("another container's panel up is in the way, and it says whose", "[loot]")
{
    Chest chest;
    chest.panel = "0x3ed";
    CHECK_THROWS_WITH(chest.Loot(Json{{"target", "0x3ec"}}), ContainsSubstring("loot panel of 0x3ed is up"));
    CHECK(chest.calls.empty());
}

TEST_CASE("no target and no panel up is refused, saying where containers are listed", "[loot]")
{
    Chest chest;
    CHECK_THROWS_WITH(chest.Loot(Json::object()), ContainsSubstring("kind=placeables"));
}

TEST_CASE("a walk that stops says why, and nothing is used", "[loot]")
{
    Chest chest;
    chest.arrives = false;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("a fight began"));
    CHECK(chest.calls == std::vector<std::string>{"travel"});
}

TEST_CASE("a container that does not open says so once its use has ended", "[loot]")
{
    Chest chest;
    chest.opens = false;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("unlock"));
}

TEST_CASE("empty remains whose use starts nothing are said to hold nothing", "[loot]")
{
    Chest chest;
    chest.opens = false;
    chest.starts = false;
    chest.items = Json::array();
    const auto answer = chest.Loot(Json{{"target", "0x430"}});
    CHECK(answer["looted"] == false);
    CHECK(answer["empty"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("nothing there"));
}

TEST_CASE("a use that starts nothing on a container that holds items says so", "[loot]")
{
    Chest chest;
    chest.opens = false;
    chest.starts = false;
    const auto answer = chest.Loot(Json{{"target", "0x430"}});
    CHECK(answer["looted"] == false);
    CHECK(answer["empty"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("holds 1 item "));
}

TEST_CASE("a message box instead of the panel is said, with how to answer it", "[loot]")
{
    Chest chest;
    chest.opens = false;
    chest.popup = Json{{"text", "It is locked."}};
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("It is locked."));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("game action=press"));
}

TEST_CASE("a full backpack leaves the rest in the container and its panel up", "[loot]")
{
    Chest chest;
    chest.full = true;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["full"] == true);
    CHECK(answer["closed"] == false);
    CHECK(answer["left"].size() == 1);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("button=Close"));
}
