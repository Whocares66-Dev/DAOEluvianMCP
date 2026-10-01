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
    double shortBy = 0; // how far from the goal a walk ends: the goal fills its own place
    std::string mode = "explore";
    bool paused = false;
    std::string fightOn; // an order after which the mode turns to combat
    std::vector<Json> orders;

    World()
    {
        // Every tool here answers on the caller's thread: no frame is pumped.
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", mode}, {"paused", paused}}; });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (args.value("kind", "") == "game")
            {
                return Json{{"controlled", {{"id", "0x82"}}}};
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
                else
                {
                    at = to;
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

TEST_CASE("a target and a point both, or neither, is refused", "[travel]")
{
    World world;
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json::object()), ToolError);
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json{{"target", "0x1"}, {"point", {0, 0, 0}}}), ToolError);
}
