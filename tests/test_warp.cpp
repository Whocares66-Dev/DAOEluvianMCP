#include "core/Warp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The game as warp asks it: the tower's top floor in the archives, a load
// that takes some reads of health, and the area's scripts' mode after.
struct Game
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string mode = "explore";        // before the jump
    std::string arrivedMode = "explore"; // once the area is in
    int loadingReads = 3;                // health reads in loading after the jump; -1, never in
    Json heroAt = {{"x", 5.4}, {"y", -3.5}, {"z", 0.0}};
    std::vector<Json> sent;
    std::string area = "bed100ar_forest_clearing";
    bool jumped = false;
    bool refuses = false; // DoAreaTransition answers 0: the area is not the running module's
    int staleReads = 0;   // state reads, out of the load, that still say the area left

    Game()
    {
        ToolSpec file{"warp.area", "", {}, Thread::Listener};
        file.listed = false;
        bench.Tools().Add(file, [](const Json &args) -> ToolResult {
            if (args.value("area", "") == "str100ar_hut_exterior")
            {
                return Json{
                    {"found", true}, {"tag", "str100ar_hut_exterior"}, {"module", "dao_prc_str"}, {"readable", false}};
            }
            if (args.value("area", "") != "pre440ar_tower_level_4")
            {
                return Json{{"found", false},
                            {"areas", Json::array({"pre440ar_tower_level_3", "pre440ar_tower_level_4",
                                                   "pre100ar_kings_camp", "bed100ar_forest_clearing"})}};
            }
            return Json{
                {"found", true},
                {"tag", "pre440ar_tower_level_4"},
                {"module", "single player"},
                {"waypoints",
                 Json::array(
                     {Json{{"tag", "pre440wp_from_tower3"}, {"position", {{"x", 5.35}, {"y", -3.59}, {"z", 0}}}},
                      Json{{"tag", "pre440wp_archer"}, {"position", {{"x", 9.9}, {"y", 6.5}, {"z", 0}}}},
                      Json{{"tag", "pre440wp_archer"}, {"position", {{"x", 0.4}, {"y", 13.6}, {"z", 0}}}}})}};
        });
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            if (jumped && loadingReads != 0)
            {
                loadingReads -= loadingReads > 0 ? 1 : 0;
                return Json{{"mode", "loading"}, {"cursor", 40}};
            }
            return Json{{"mode", jumped ? arrivedMode : mode}, {"cursor", 40}};
        });
        bench.Tools().Add(ToolSpec{"script", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            sent.push_back(args);
            if (refuses)
            {
                return Json{{"command", args.value("command", "")}, {"value", 0}};
            }
            jumped = true;
            area = args["args"][0].get<std::string>();
            return Json{{"command", args.value("command", "")}, {"value", 1}};
        });
        bench.Tools().Add(ToolSpec{"state", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            if (jumped && staleReads > 0)
            {
                --staleReads;
                return Json{{"area", {{"id", "0xe1"}, {"tag", "bed100ar_forest_clearing"}}}, {"party", Json::array()}};
            }
            return Json{{"area", {{"id", "0x3f9"}, {"tag", area}}},
                        {"party", Json::array({Json{{"id", "0x82"}, {"tag", "player"}}})}};
        });
        bench.Tools().Add(ToolSpec{"creature", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"id", "0x82"}, {"position", heroAt}}; });
        AddWarpTool(bench, std::chrono::milliseconds(1), std::chrono::milliseconds(1));
    }

    // The least budget unless a test sets one: a refusal broken goes on to wait
    // for an area that never comes, and is to fail in seconds, not a minute.
    Json Warp(Json args)
    {
        if (!args.contains("budgetMs"))
        {
            args["budgetMs"] = 5000;
        }
        return bench.Invoke("warp", args).data;
    }
};

} // namespace

TEST_CASE("warp jumps by the game's own transition to the waypoint asked, and waits out the load", "[warp]")
{
    Game game;
    const Json answer = game.Warp({{"area", "PRE440AR_Tower_Level_4"}, {"waypoint", "PRE440WP_FROM_TOWER3"}});
    REQUIRE(game.sent.size() == 1);
    CHECK(game.sent[0]["command"] == "DoAreaTransition");
    // The area's and the waypoint's own tags, whatever the case asked in.
    CHECK(game.sent[0]["args"] == Json::array({"pre440ar_tower_level_4", "pre440wp_from_tower3"}));
    CHECK(answer["arrived"] == true);
    CHECK(answer["area"]["tag"] == "pre440ar_tower_level_4");
    CHECK(answer["mode"] == "explore");
    CHECK(answer["cursor"] == 40);
    CHECK(answer["waypoint"]["tag"] == "pre440wp_from_tower3");
    CHECK(answer["fromWaypoint"].get<double>() < 0.2);
    CHECK_FALSE(answer.contains("note"));
    CHECK(game.loadingReads == 0); // it waited through every read of the load
}

TEST_CASE("warp with no waypoint takes the area's own arrival", "[warp]")
{
    Game game;
    const Json answer = game.Warp({{"area", "pre440ar_tower_level_4"}});
    REQUIRE(game.sent.size() == 1);
    CHECK(game.sent[0]["args"][1].get<std::string>().empty());
    CHECK(answer["arrived"] == true);
    CHECK_FALSE(answer.contains("waypoint"));
}

TEST_CASE("warp says what the area's scripts started on arriving", "[warp]")
{
    Game game;
    game.arrivedMode = "cutscene";
    CHECK_THAT(game.Warp({{"area", "pre440ar_tower_level_4"}})["note"].get<std::string>(),
               ContainsSubstring("skip_cutscene"));
    Game fight;
    fight.mode = "combat"; // and it jumps from a fight
    fight.arrivedMode = "combat";
    const Json answer = fight.Warp({{"area", "pre440ar_tower_level_4"}});
    CHECK(answer["mode"] == "combat");
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("fight"));
}

TEST_CASE("warp lists an area's waypoints and jumps nowhere", "[warp]")
{
    Game game;
    const Json answer = game.Warp({{"area", "pre440ar_tower_level_4"}, {"waypoints", true}});
    CHECK(answer["count"] == 3);
    CHECK(answer["module"] == "single player");
    CHECK(answer["waypoints"][0]["tag"] == "pre440wp_from_tower3");
    CHECK(game.sent.empty());
}

TEST_CASE("warp refuses an area the archives lack, naming those near it, and sends nothing", "[warp]")
{
    Game game;
    REQUIRE_THROWS_WITH(game.Warp({{"area", "tower_level"}}),
                        ContainsSubstring("no area \"tower_level\"") &&
                            ContainsSubstring("pre440ar_tower_level_3, pre440ar_tower_level_4"));
    // Nothing has the name in it: those beginning as it does.
    REQUIRE_THROWS_WITH(game.Warp({{"area", "pre440ar_roof"}}),
                        ContainsSubstring("near it: pre440ar_tower_level_3, pre440ar_tower_level_4") &&
                            !ContainsSubstring("kings_camp"));
    REQUIRE_THROWS_WITH(game.Warp({{"area", "nowhere"}}), !ContainsSubstring("near it"));
    CHECK(game.sent.empty());
}

TEST_CASE("warp refuses a waypoint the area lacks, naming its own, and sends nothing", "[warp]")
{
    Game game;
    REQUIRE_THROWS_WITH(game.Warp({{"area", "pre440ar_tower_level_4"}, {"waypoint", "no_such_waypoint"}}),
                        ContainsSubstring("(0, 0, 0)") &&
                            ContainsSubstring("its waypoints: pre440wp_from_tower3, pre440wp_archer."));
    CHECK(game.sent.empty());
}

TEST_CASE("warp jumps only from explore or combat", "[warp]")
{
    Game game;
    game.mode = "dialog";
    REQUIRE_THROWS_WITH(game.Warp({{"area", "pre440ar_tower_level_4"}}), ContainsSubstring("in dialog mode"));
    CHECK(game.sent.empty());
}

TEST_CASE("warp answers not arrived when the load outlasts its budget", "[warp]")
{
    Game game;
    game.loadingReads = -1;
    const Json answer = game.Warp({{"area", "pre440ar_tower_level_4"}, {"budgetMs", 5000}});
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("within budgetMs"));
    CHECK(answer["waitedMs"].get<long long>() >= 5000);
}

TEST_CASE("warp says when the hero stands away from the waypoint asked", "[warp]")
{
    Game game;
    game.heroAt = {{"x", 30.0}, {"y", 0.0}, {"z", 0.0}};
    const Json answer = game.Warp({{"area", "pre440ar_tower_level_4"}, {"waypoint", "pre440wp_from_tower3"}});
    CHECK(answer["fromWaypoint"].get<double>() > 20);
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("m from the waypoint"));
}

TEST_CASE("warp takes an area whose file it cannot read only at the area's own arrival", "[warp]")
{
    Game game;
    REQUIRE_THROWS_WITH(game.Warp({{"area", "str100ar_hut_exterior"}, {"waypoint", "str100wp_start"}}),
                        ContainsSubstring("dao_prc_str's archive, which is enciphered"));
    REQUIRE_THROWS_WITH(game.Warp({{"area", "str100ar_hut_exterior"}, {"waypoints", true}}),
                        ContainsSubstring("waypoints are not known"));
    CHECK(game.sent.empty());
    const Json answer = game.Warp({{"area", "str100ar_hut_exterior"}});
    CHECK(answer["arrived"] == true);
    CHECK(answer["module"] == "dao_prc_str");
    CHECK(game.sent[0]["args"] == Json::array({"str100ar_hut_exterior", ""}));
}

TEST_CASE("warp says the game refused an area the running module does not load", "[warp]")
{
    Game game;
    game.refuses = true;
    REQUIRE_THROWS_WITH(game.Warp({{"area", "str100ar_hut_exterior"}}),
                        ContainsSubstring("answered 0") && ContainsSubstring("the area is dao_prc_str's"));
    CHECK(game.sent.size() == 1);
}

TEST_CASE("warp waits for the area asked, not the one it left", "[warp]")
{
    Game game;
    game.loadingReads = 0; // the load too quick to see, as a jump within one area list was
    game.staleReads = 3;
    const Json answer = game.Warp({{"area", "pre440ar_tower_level_4"}});
    CHECK(answer["arrived"] == true);
    CHECK(answer["area"]["tag"] == "pre440ar_tower_level_4");
    CHECK(game.staleReads == 0);
}
