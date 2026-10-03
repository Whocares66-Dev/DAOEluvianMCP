#include "core/Travel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A corridor: the walker starts 30 m from the goal with a shut door at 10
// m. The stand-in tools answer as the game's do: geometry with the walk,
// orders that end on the bus, health with the mode.
struct World
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    double at = 30;     // metres from the goal
    double door = 10;   // where the door stands, from the goal
    bool shut = true;   // the door
    bool opens = true;  // whether a use opens it
    bool named = true;  // whether the search names the door from afar
    bool stuck = false; // whether a move ends where it began: the walk's end is where none can stand
    double shortBy = 0; // how far from the goal a walk ends: the goal fills its own place
    double partway = 0; // where a move stops, if not where it was sent
    std::string mode = "explore";
    bool paused = false;
    std::string fightOn; // an order after which the mode turns to combat
    std::vector<Json> orders;
    std::string area = "ruins"; // the area's tag
    bool crosses = true;        // whether using the exit takes the party out
    bool asks = false;          // whether the game asks first, with a message box
    Json popup = nullptr;

    World()
    {
        // Every tool here answers on the caller's thread: no frame is pumped.
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", mode}, {"paused", paused}}; });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (args.value("kind", "") == "game")
            {
                return Json{{"controlled", {{"id", "0x82"}}},
                            {"area", {{"tag", area}, {"name", area == "forest" ? "Forest Clearing" : "Ruins"}}},
                            {"popup", popup}};
            }
            if (args.value("kind", "") == "exits")
            {
                return Json{{"exits", Json::array({Json{{"id", "0x1f5"},
                                                        {"leadsTo", {{"area", "forest"}, {"waypoint", "wp"}}},
                                                        {"active", true}}})}};
            }
            Json walk;
            if (!shut || at <= door)
            {
                walk = Json{{"found", true}, {"length", at - shortBy}};
                if (shortBy > 0)
                {
                    walk["waypoints"] = Json::array({Json::array({shortBy, 0, 0})});
                }
            }
            else if (named || at - door < 1.5)
            {
                walk = Json{{"found", false}, {"result", "something is in the way"}, {"blocker", {{"id", "0x1f1"}}}};
            }
            else
            {
                walk = Json{{"found", false}, {"result", "gave up part way"}, {"reached", {door + 1, 0, 0}}};
            }
            return Json{{"from", {{"id", args.value("id", "")}}}, {"to", {0, 0, 0}}, {"distance", at}, {"walk", walk}};
        });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            orders.push_back(args);
            const auto cursor = bench.Events().Next();
            const std::string action = args.value("action", "");
            std::string result = "success";
            if (action == "move")
            {
                const double to = args.at("point").at(0).get<double>();
                if (shut && to < door && at > door)
                {
                    at = door + 0.5; // stops at the door
                    result = "COMMAND_FAILED_PATH_ACTION_REQUIRED";
                }
                else if (!stuck)
                {
                    at = partway > 0 ? partway : to;
                }
            }
            else if (action == "use" && args.value("target", "") == "0x1f5")
            {
                if (asks)
                {
                    popup = Json{{"text", "Leave the area?"}};
                }
                else if (crosses)
                {
                    area = "forest";
                    bench.Events().Emit("eluvian.script.area_loaded", Json::object());
                }
            }
            else if (action == "use" && opens)
            {
                shut = false;
            }
            if (action == fightOn)
            {
                mode = "combat";
            }
            bench.Events().Emit("eluvian.command.ended",
                                Json{{"id", args.value("id", "")},
                                     {"type", action == "move" ? "MoveToLocation" : "UseObject"},
                                     {"result", result}});
            return Json{{"given", true}, {"cursor", cursor}};
        });
        AddTravelTool(bench);
    }

    Json Travel(Json args = Json::object())
    {
        args.update(Json{{"target", "0x2ec"}, {"budgetMs", 5000}});
        return bench.Invoke("travel", args).data;
    }
};

} // namespace

TEST_CASE("an open way is one move", "[travel]")
{
    World world;
    world.shut = false;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["opened"].empty());
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["action"] == "move");
    CHECK(world.orders[0]["id"] == "0x82"); // the creature controlled, none being named
}

TEST_CASE("a shut door named from afar is used, then the walk made", "[travel]")
{
    World world;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    REQUIRE(answer["opened"].size() == 1);
    CHECK(answer["opened"][0]["id"] == "0x1f1");
    REQUIRE(world.orders.size() == 2);
    CHECK(world.orders[0]["action"] == "use");
    CHECK(world.orders[0]["target"] == "0x1f1");
    CHECK_FALSE(world.orders[0].contains("via")); // the controlled creature's use goes by click
    CHECK(world.orders[1]["action"] == "move");
}

TEST_CASE("a door the search cannot name is found by going as far as the search got", "[travel]")
{
    World world;
    world.named = false;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["opened"].size() == 1);
    REQUIRE(world.orders.size() == 3);
    CHECK(world.orders[0]["action"] == "move"); // to where the search stopped
    CHECK(world.orders[1]["action"] == "use");
    CHECK(world.orders[2]["action"] == "move");
}

TEST_CASE("another creature's use goes by command and names what to do", "[travel]")
{
    World world;
    const auto answer = world.Travel(Json{{"id", "0x10d"}});
    CHECK(answer["arrived"] == true);
    CHECK(world.orders[0]["via"] == "command");
    CHECK(world.orders[0]["objectAction"] == "open");
}

TEST_CASE("a door that does not open ends the journey, and it says which", "[travel]")
{
    World world;
    world.opens = false;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open: 0x1f1"));
}

TEST_CASE("no doors allowed, and one in the way", "[travel]")
{
    World world;
    const auto answer = world.Travel(Json{{"maxDoors", 0}});
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("maxDoors"));
    CHECK(world.orders.empty());
}

TEST_CASE("a fight that begins takes the journey from it", "[travel]")
{
    World world;
    world.fightOn = "use"; // opening the door starts one
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "a fight began");
    CHECK(answer["opened"].size() == 1);
    CHECK(world.orders.size() == 1); // no move was ordered into the fight
}

TEST_CASE("a walk a fight stops part way says what is left where it stopped", "[travel]")
{
    World world;
    world.shut = false;
    world.partway = 8.3;
    world.fightOn = "move";
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "a fight began");
    CHECK(answer["distance"] == 8.3); // not the 30 measured before the move
}

TEST_CASE("a paused game is said, not waited out", "[travel]")
{
    World world;
    world.paused = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "the game is paused");
    CHECK(world.orders.empty());
}

TEST_CASE("already there is no order at all", "[travel]")
{
    World world;
    world.at = 1.0;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(world.orders.empty());
}

TEST_CASE("a thing that fills its own place is reached as near as the walk goes", "[travel]")
{
    World world;
    world.shut = false;
    world.shortBy = 2.5; // a door: nothing stands where it does
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "as near as the walk goes");
    CHECK(answer["distance"] == 2.5);
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["point"][0] == 2.5); // the walk's end, not the door's own place
}

TEST_CASE("a thing whose walk's end none can stand on is reached when a move brings it no nearer", "[travel]")
{
    World world;
    world.shut = false;
    world.stuck = true; // a cocoon overhead: each move ends where it began
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("as near as the walk goes"));
    CHECK(world.orders.size() == 1); // not one move after another until the steps run out
}

TEST_CASE("a point a move brings no nearer is not reached", "[travel]")
{
    World world;
    world.shut = false;
    world.stuck = true;
    const auto answer = world.bench.Invoke("travel", Json{{"point", {0, 0, 0}}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("no nearer"));
    CHECK(answer["distance"] == 30);
    CHECK(world.orders.size() == 1);
}

TEST_CASE("a target and a point both, or neither, is refused", "[travel]")
{
    World world;
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json::object()), ToolError);
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json{{"target", "0x1"}, {"point", {0, 0, 0}}}), ToolError);
}

TEST_CASE("an area is reached by the exit that leads there", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 5;
    const auto answer = world.bench.Invoke("travel", Json{{"area", "forest"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    CHECK(answer["area"]["tag"] == "forest");
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("through to Forest Clearing"));
    CHECK(answer["exit"]["id"] == "0x1f5");
    REQUIRE(world.orders.size() == 2);
    CHECK(world.orders[0]["action"] == "move");
    CHECK(world.orders[1]["action"] == "use");
    CHECK(world.orders[1]["target"] == "0x1f5");
}

TEST_CASE("an exit is named by its id too", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1; // already there: no walk, only the use
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["action"] == "use");
}

TEST_CASE("an exit or an area this area has not is refused, saying where its exits lead", "[travel]")
{
    World world;
    CHECK_THROWS_WITH(world.bench.Invoke("travel", Json{{"exit", "0x999"}}),
                      ContainsSubstring("0x999 is not one of this area's exits"));
    CHECK_THROWS_WITH(world.bench.Invoke("travel", Json{{"area", "denerim"}}), ContainsSubstring("lead to forest"));
    CHECK(world.orders.empty());
}

TEST_CASE("only the creature controlled takes the party to another area", "[travel]")
{
    World world;
    CHECK_THROWS_WITH(world.bench.Invoke("travel", Json{{"area", "forest"}, {"id", "0x10d"}}),
                      ContainsSubstring("only the creature controlled"));
}

TEST_CASE("a message box before leaving stops the crossing and says what it asks", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.asks = true;
    const auto answer = world.bench.Invoke("travel", Json{{"area", "forest"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Leave the area?"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("game action=press"));
}

TEST_CASE("an exit that does not take the party out ends when the time does", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.crosses = false;
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 1000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not leave"));
}
