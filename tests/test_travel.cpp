#include "core/Travel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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
    double at = 30;            // metres from the goal
    double door = 10;          // where the door stands, from the goal
    bool shut = true;          // the door
    bool opens = true;         // whether a use opens it
    bool named = true;         // whether the search names the door from afar
    bool stuck = false;        // whether a move ends where it began: the walk's end is where none can stand
    double shortBy = 0;        // how far from the goal a walk ends: the goal fills its own place
    double partway = 0;        // where a move stops, if not where it was sent
    bool hidden = false;       // whether the search walks through the shut door until a move is stopped at it
    double reach = 1;          // how far short of the door a search that cannot name it gets
    std::string moveFails;     // the result every move ends with instead of success, if any
    double failsAt = -1;       // where a failing move leaves the walker, from the goal; -1 where it stood
    bool silent = false;       // whether a move's end never comes: a fight takes the command with it
    bool unseen = false;       // whether the walker is left with no command when a move's end never comes
    bool chatter = false;      // whether others' commands end around each order, and the walker's last is cleared
    bool doorLoads = false;    // whether the door's use starts a load, refusing the orders after it
    int clears = 0;            // how many moves the game clears before they go, giving no other
    bool blocksItself = false; // whether the thing in the way is the target itself: a chest in a narrow place
    int pickLevel = 1;         // the door's lock: 10000 is the game's "cannot be picked"
    bool exitAhead = false;    // whether what the search names ahead is an exit beside the target, not a door
    bool inert = false;        // whether what stands in the way is not interactive: the story's to remove
    bool inertStays = false;   // whether it stays when the walker comes up to it (no trigger there removes it)
    bool keyRequired = false;  // whether only the door's key opens it
    bool keyHeld = false;      // whether the party carries that key
    int doorState = 1;         // the door's state: 1 locked, 0 shut and unlocked
    bool unlocksFirst = false; // whether a use with the key carried only unlocks the door, the key taken
    bool silentUse = false;    // whether a use's end is never said either
    std::string mode = "explore";
    double hostileAt = -1; // how far the nearest hostile stands from the walker; -1 none
    bool paused = false;
    std::string fightOn;  // an order after which the mode turns to combat
    std::string loadOn;   // an order after which an area loads, and the frame tools have no frame
    int loadFor = 100;    // health calls the load lasts: an autosave's is short, and the area stays
    std::string loadInto; // the area a load ends in, if another
    std::vector<Json> orders;
    std::string area = "ruins"; // the area's tag
    bool crosses = true;        // whether using the exit takes the party out
    bool exitActive = true;     // whether the exit is active
    bool loads = false;         // whether the crossing loads the other area over a health call
    int loadingFor = 0;         // health calls an area's loading is still said for
    int readsWhileLoading = 0;  // frame reads asked while it loads: each waits out the frame timeout in the game
    bool asks = false;          // whether the game asks first, with a message box
    bool blockerFirst = false;  // whether a text blocker to the same place is nearer than the exit
    std::string exitNoWalk;     // the search's word for a walk to the exit it cannot make, if it cannot
    double strip = -1;          // beyond this distance the search finds none, though straight steps cross
    bool farFails = false;      // whether a move of more than a metre from within the strip fails INVALID_PATH
    bool tooClose = false;      // whether the exit's use fails from closer than 2 m
    Json popup = nullptr;
    bool mapExit = false;              // whether the exit opens the world map instead of leading somewhere
    bool mapUp = false;                // whether the world map is on the screen
    int mapLate = 0;                   // reads of the map before it is up, after its exit
    std::string encounter;             // the area a pin's travel stops in on the way, if any
    std::vector<std::string> messages; // what input sent
    std::vector<Json> screenCalls;     // input's calls on a screen's movie
    bool closedByTravel = false;       // the map movie's mark, as a pin's click sets it
    bool partyKept = false;            // whether the party went with the hero
    bool fadeMap = false;              // whether the map up is the Fade's own
    Json pins = Json::array(
        {Json{{"index", 0}, {"name", "Ostagar"}, {"tag", "wm_ostagar"}, {"area", "ost100ar"}, {"state", "completed"}},
         Json{{"index", 1}, {"name", "Lothering"}, {"tag", "wm_lothering"}, {"area", "lot100ar"}, {"state", "active"}},
         Json{
             {"index", 2}, {"name", "Denerim"}, {"tag", "wm_denerim"}, {"area", "den100ar"}, {"state", "not active"}}});

    World()
    {
        // Every tool here answers on the caller's thread: no frame is pumped.
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            if (mode == "loading" && loadingFor-- <= 0)
            {
                mode = "explore";
            }
            return Json{{"mode", mode}, {"paused", paused}};
        });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (mode == "loading")
            {
                ++readsWhileLoading;
                throw FrameTimeout("no frame took the call within 2000 ms"); // as a frame tool's, while frames stop
            }
            if (args.value("kind", "") == "creatures")
            {
                // Hostiles within the radius asked for, nearest first.
                Json near = Json::array();
                if (hostileAt >= 0 && hostileAt <= args.value("radius", 1e9))
                {
                    near.push_back(Json{{"id", "0x5a1"}, {"name", "Shade"}, {"distance", hostileAt}});
                }
                return Json{{"creatures", near}};
            }
            if (args.value("kind", "") == "creature")
            {
                return Json{{"command", {{"current", unseen ? Json(nullptr) : Json("MoveToLocation")}, {"queued", 0}}}};
            }
            if (args.value("kind", "") == "game")
            {
                return Json{{"controlled", {{"id", "0x82"}}},
                            {"area",
                             {{"tag", area},
                              {"name", area == "forest" ? "Forest Clearing" : (area == "ruins" ? "Ruins" : area)}}},
                            {"popup", popup}};
            }
            if (args.value("kind", "") == "gui")
            {
                return Json{{"popup", popup}};
            }
            if (args.value("kind", "") == "worldmap")
            {
                if (mapExit && !mapUp && mapLate-- == 0)
                {
                    mapUp = true;
                }
                return Json{{"up", mapUp},
                            {"screen", fadeMap ? "FadeMap" : "WorldMap"},
                            {"map", fadeMap ? "fade" : "world"},
                            {"at", area},
                            {"pins", pins}};
            }
            if (args.value("kind", "") == "exits")
            {
                Json exits = Json::array({Json{
                    {"id", "0x1f5"}, {"leadsTo", {{"area", "forest"}, {"waypoint", "wp"}}}, {"active", exitActive}}});
                if (blockerFirst)
                {
                    // Nearer, to the same place, and only a line of text.
                    exits.insert(exits.begin(), Json{{"id", "0x1f9"},
                                                     {"tag", "wmt_rand_enc_text_blocker"},
                                                     {"leadsTo", {{"area", "forest"}, {"waypoint", "wp"}}},
                                                     {"active", true},
                                                     {"blocker", true}});
                }
                return Json{{"exits", exits}};
            }
            Json walk;
            if (!exitNoWalk.empty() && args.value("target", "") == "0x1f5")
            {
                walk = Json{{"found", false}, {"result", exitNoWalk}};
            }
            else if (strip >= 0 && at > strip)
            {
                walk = Json{{"found", false}, {"result", "none"}};
            }
            else if (!shut || at <= door || hidden)
            {
                walk = Json{{"found", true}, {"length", at - shortBy}};
                if (shortBy > 0)
                {
                    walk["waypoints"] = Json::array({Json::array({shortBy, 0, 0})});
                }
            }
            else if (inert && shut && at > door)
            {
                // A thing no use opens, across the way: as far as the search
                // got, short of it.
                walk = Json{{"found", false},
                            {"result", "something is in the way"},
                            {"reached", {door + reach, 0, 0}},
                            {"blocker", {{"id", "0x2a1b4"}, {"name", "Invisible Object"}, {"interactive", false}}}};
            }
            else if (exitAhead)
            {
                // Where the search gives up, the placeable just ahead: here
                // the World Map exit beside the door.
                walk = Json{{"found", false},
                            {"result", "gave up part way"},
                            {"reached", {door + reach, 0, 0}},
                            {"blocker", {{"id", "0x240e9"}, {"name", "World Map"}, {"leadsTo", {{"area", ""}}}}}};
            }
            else if (named || at - door < 1.5)
            {
                walk = Json{{"found", false},
                            {"result", "something is in the way"},
                            {"reached", blocksItself ? Json::array({door + reach, 0, 0}) : Json(nullptr)},
                            {"blocker",
                             {{"id", blocksItself ? args.value("target", "") : std::string("0x1f1")},
                              {"name", "Metal Door"},
                              {"state", doorState},
                              {"lock",
                               {{"pickLevel", pickLevel},
                                {"keyRequired", keyRequired},
                                {"keyTag", "DoesNotExist"},
                                {"keyHeld", keyHeld}}}}}};
            }
            else
            {
                walk = Json{{"found", false}, {"result", "gave up part way"}, {"reached", {door + reach, 0, 0}}};
            }
            return Json{{"from", {{"id", args.value("id", "")}}},
                        {"position", {{"x", at}, {"y", 0}, {"z", 0}}},
                        {"to", {0, 0, 0}},
                        {"distance", at},
                        {"walk", walk}};
        });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (doorLoads && !shut)
            {
                throw ToolError("order: an area is loading; wait_for topic \"eluvian.mode.changed\" and try again");
            }
            orders.push_back(args);
            const auto cursor = bench.Events().Next();
            const std::string action = args.value("action", "");
            if (action == "move" && clears > 0)
            {
                --clears; // the settling after a load: the move cleared, nothing in its place
                bench.Events().Emit("eluvian.command.ended", Json{{"id", args.value("id", "")},
                                                                  {"type", "MoveToLocation"},
                                                                  {"result", "COMMAND_FAILED_COMMAND_CLEARED"}});
                return Json{{"given", true}, {"cursor", cursor}};
            }
            std::string result = "success";
            if (action == "move")
            {
                const double to = args.at("point").at(0).get<double>();
                if (!moveFails.empty())
                {
                    result = moveFails; // and the walker stays where it is, or where it got
                    if (failsAt >= 0)
                    {
                        at = failsAt;
                    }
                }
                else if (shut && to < door && at > door)
                {
                    at = door + 0.5; // stops at the door
                    hidden = false;  // which the search names from now on
                    result = "COMMAND_FAILED_PATH_ACTION_REQUIRED";
                }
                else if (farFails && at > strip && std::abs(at - to) > 1.0)
                {
                    result = "COMMAND_FAILED_INVALID_PATH"; // from within the strip the engine finds none either
                }
                else if (!stuck)
                {
                    at = partway > 0 ? partway : to;
                }
            }
            else if (action == "use" && args.value("target", "") == "0x1f5")
            {
                if (tooClose && at < 2)
                {
                    result = "COMMAND_FAILED";
                }
                else if (asks)
                {
                    popup = Json{{"text", "Leave the area?"}};
                }
                else if (mapExit)
                {
                    mode = "gui"; // the map comes up a moment later, read by its reads
                }
                else if (loads)
                {
                    // The other area loads: its scripts are told it is in
                    // place, the area's and the module's, while the game
                    // still says loading for one more health call.
                    area = "forest";
                    mode = "loading";
                    loadingFor = 1;
                    bench.Events().Emit("eluvian.script.area_loaded", Json{{"receiver", "area"}});
                    bench.Events().Emit("eluvian.script.area_loaded", Json{{"receiver", "module"}});
                }
                else if (crosses)
                {
                    area = "forest";
                    bench.Events().Emit("eluvian.script.area_loaded", Json::object());
                }
            }
            else if (action == "use" && opens)
            {
                if (unlocksFirst && doorState == 1 && keyHeld)
                {
                    doorState = 0; // unlocked, still shut; the key leaves the pack
                    keyHeld = false;
                }
                else
                {
                    shut = false;
                }
            }
            if (action == "move" && inert && !inertStays && at <= door + reach + 0.01)
            {
                shut = false; // walked into the trigger beside it, whose script removes it
            }
            if (action == fightOn)
            {
                mode = "combat";
                hostileAt = 3; // the fight is at hand
            }
            if (action == loadOn)
            {
                mode = "loading";
                loadingFor = loadFor;
                if (!loadInto.empty())
                {
                    area = loadInto;
                }
            }
            const std::string type = action == "move" ? "MoveToLocation" : "UseObject";
            if (chatter)
            {
                // A companion's command of the same kind ends, and the
                // walker's last, which this order cleared.
                bench.Events().Emit("eluvian.command.ended",
                                    Json{{"id", "0x10d"}, {"type", type}, {"result", "COMMAND_FAILED_INVALID_PATH"}});
                bench.Events().Emit(
                    "eluvian.command.ended",
                    Json{{"id", args.value("id", "")}, {"type", type}, {"result", "COMMAND_FAILED_COMMAND_CLEARED"}});
            }
            if ((!silent || action != "move") && !(silentUse && action == "use"))
            {
                bench.Events().Emit("eluvian.command.ended",
                                    Json{{"id", args.value("id", "")}, {"type", type}, {"result", result}});
            }
            return Json{{"given", true}, {"cursor", cursor}};
        });
        bench.Tools().Add(ToolSpec{"input", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (args.contains("screen"))
            {
                // The map movie's own calls: its closed-by-travel mark and
                // the pin's index, as the pin's click sets them.
                screenCalls.push_back(args);
                if (args.value("set", "") == "WorldMap.m_bClosedByTravel")
                {
                    closedByTravel = args.value("value", false);
                    return Json{{"screen", "WorldMap"}, {"set", args.value("set", "")}, {"done", true}};
                }
                return Json{{"screen", "WorldMap"}, {"call", args.value("call", "")}, {"ran", true}};
            }
            const std::string message = args.value("message", "");
            messages.push_back(message);
            // A pin's click: the party goes there, or into an encounter on
            // the way, over a load.
            const std::string click = "GameModeController.AreaTransition " + area + " ";
            if (mapUp && message.starts_with(click))
            {
                const Json &pin = pins.at(std::stoi(message.substr(click.size())));
                if (pin.value("state", "") == "active")
                {
                    // At camp, a map closed not by travel sends the
                    // followers home and the hero goes alone.
                    partyKept = closedByTravel;
                    mapUp = false;
                    area = encounter.empty() ? pin.value("area", "") : encounter;
                    mode = "loading";
                    loadingFor = 1;
                    bench.Events().Emit("eluvian.script.area_loaded", Json::object());
                }
            }
            return Json{{"message", message}, {"sent", true}};
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

TEST_CASE("a walker inside the `within` given is there already", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 4;
    const auto answer = world.Travel(Json{{"within", 5}});
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there");
    CHECK(world.orders.empty());
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

TEST_CASE("a door a move is stopped at is opened, and the walk through it is a new one", "[travel]")
{
    World world;
    world.hidden = true; // the search goes through the door until a move finds it shut
    world.at = 10.6;     // and the move stops at once, hardly nearer
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there"); // not "no nearer", measured against the walk from before the door
    CHECK(answer["opened"].size() == 1);
    REQUIRE(world.orders.size() == 3);
    CHECK(world.orders[0]["action"] == "move");
    CHECK(world.orders[1]["action"] == "use");
    CHECK(world.orders[2]["action"] == "move");
}

TEST_CASE("a move the game refuses ends the journey with the game's word for it", "[travel]")
{
    World world;
    world.shut = false;
    world.moveFails = "COMMAND_FAILED_INVALID_PATH";
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("COMMAND_FAILED_INVALID_PATH"));
    CHECK(world.orders.size() == 1);
}

TEST_CASE("where the search finds none, the engine's own move goes through", "[travel]")
{
    World world; // an open doorway the search takes for no ground, which the engine walks through
    world.shut = false;
    world.strip = 28;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    const auto did = [&answer](const std::string &what) {
        return std::ranges::count_if(
            answer["steps"], [&what](const Json &step) { return step.value("did", std::string()).starts_with(what); });
    };
    CHECK(did("the engine's own move") == 1);
    CHECK(did("straight step") == 0);
}

TEST_CASE("where the search and the engine's own move find none, straight steps cross and the walk goes on", "[travel]")
{
    World world; // a doorway's strip, 2 m deep, that only short steps cross
    world.shut = false;
    world.strip = 28;
    world.farFails = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    const auto steps = std::ranges::count_if(
        answer["steps"], [](const Json &step) { return step.value("did", std::string()) == "straight step"; });
    CHECK(steps == 4); // 30 to 28 in half metres, then the walk
    World stuck;       // the straight step goes nowhere either
    stuck.shut = false;
    stuck.strip = 28;
    stuck.stuck = true;
    const auto stopped = stuck.Travel();
    CHECK(stopped["arrived"] == false);
    CHECK_THAT(stopped["reason"].get<std::string>(), ContainsSubstring("a straight step toward it got no nearer"));
    CHECK(stuck.orders.size() == 2); // the engine's own move, then one straight step
}

TEST_CASE("a move that fails beside its target, where the target stands, is there", "[travel]")
{
    World world;
    world.shut = false;
    world.moveFails = "COMMAND_FAILED_INVALID_PATH";
    world.failsAt = 0.45; // beside a creature: its own place is taken
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there");
    CHECK(answer["distance"] == 0.45);
}

TEST_CASE("a search that gets no nearer from as far as it got says there is no walk", "[travel]")
{
    World world;
    world.named = false;
    world.reach = 3; // too far from the door for the search to name it
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("no walk there: gave up part way"));
    CHECK(world.orders.size() == 1); // as far as the search got, once
}

TEST_CASE("a target that is itself in the way is reached, not opened as a door", "[travel]")
{
    World near; // a chest in a narrow place, the walker at it
    near.blocksItself = true;
    near.door = 1;
    near.at = 3;
    const auto answer = near.Travel();
    CHECK(answer["arrived"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the target itself"));
    CHECK(near.orders.empty());
}

TEST_CASE("a target in its own way far off is walked up to first, as far as the search got", "[travel]")
{
    World world; // a locked door 30 m off, the search's way ending at it
    world.blocksItself = true;
    world.door = 1;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the target itself"));
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["action"] == "move"); // walked up to it, not used
    World short_;                               // the search's way ends 11 m short of it: no nearer
    short_.blocksItself = true;
    const auto stopped = short_.Travel();
    CHECK(stopped["arrived"] == false);
    CHECK_THAT(stopped["reason"].get<std::string>(), ContainsSubstring("11 m off, and the walk gets no nearer"));
    REQUIRE(short_.orders.size() == 2); // as far as the search got, then the engine's own move, once (#67)
    CHECK(short_.orders[1]["point"][0] == 0);
}

TEST_CASE("a target the search names from afar, no nearer, is walked to by the engine's own move", "[travel]")
{
    World world; // an exit two rooms away (#67): the search's way ends where the walker stands
    world.blocksItself = true;
    world.door = 1;
    world.reach = 29; // as far as the search got is where the walk began, 30 m off
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    REQUIRE(world.orders.size() == 2);
    CHECK(world.orders[0]["point"][0] == 30); // as far as the search got: no nearer
    CHECK(world.orders[1]["point"][0] == 0);  // the engine's own move to it
    CHECK_THAT(answer["steps"][1]["did"].get<std::string>(), ContainsSubstring("the engine's own move"));
}

TEST_CASE("the cursor is taken as the journey begins, so its events are read from it", "[travel]")
{
    World world;
    world.shut = false;
    const auto before = world.bench.Events().Next();
    const auto answer = world.Travel();
    CHECK(answer["cursor"] == before); // the move's end, emitted during the journey, is after it
    CHECK_FALSE(world.bench.Events().Since(answer["cursor"].get<std::uint64_t>(), EventFilter{}, 10).events.empty());
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
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("a fight began")); // and who keeps it
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
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("a fight began")); // and who keeps it
    CHECK(answer["distance"] == 8.3); // not the 30 measured before the move
}

TEST_CASE("a door whose plot starts a load stops the journey with the refusal, the door kept", "[travel]")
{
    World world;
    world.doorLoads = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("an area is loading"));
    REQUIRE(answer["opened"].size() == 1);
    CHECK(answer["opened"][0]["id"] == "0x1f1");
    CHECK(world.orders.size() == 1); // the use; the move after it was refused
}

TEST_CASE("a move the game clears and gives no other is given again, not waited out", "[travel]")
{
    World world;
    world.shut = false;
    world.clears = 1;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there"); // not "as near as the walk goes": the cleared move was no move
    REQUIRE(world.orders.size() == 2);
    CHECK(answer["steps"][0]["result"] == "COMMAND_FAILED_COMMAND_CLEARED");
}

TEST_CASE("a message box that pauses the game is said, with what it asks", "[travel]")
{
    World world;
    world.paused = true;
    world.popup = Json{{"text", "Gather your party and venture forth?"}};
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Gather your party"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("game action=press"));
    CHECK(world.orders.empty());
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

TEST_CASE("a conversation or a cutscene that has the screen is said, not walked through", "[travel]")
{
    for (const char *mode : {"dialog", "cutscene"})
    {
        World world;
        world.mode = mode;
        const auto answer = world.Travel();
        CHECK(answer["arrived"] == false);
        CHECK(answer["reason"] ==
              std::string("a ") + (world.mode == "dialog" ? "conversation" : "cutscene") + " took the screen");
        CHECK(world.orders.empty());
    }
}

TEST_CASE("an area that begins to load ends the journey, with no distance rather than a stale one", "[travel]")
{
    World world;
    world.shut = false;
    world.loadOn = "move";
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "an area is loading");
    CHECK_FALSE(answer.contains("distance")); // no frame to measure it in; the 30 m from before the move is stale
}

TEST_CASE("the game's autosave on the way is waited out, and the journey goes on", "[travel]")
{
    World world;
    world.loadOn = "use"; // the door's use makes the game autosave
    world.loadFor = 2;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["opened"].size() == 1);
}

TEST_CASE("a load that ends in another area ends the journey, saying which", "[travel]")
{
    World world;
    world.loadOn = "use";
    world.loadFor = 2;
    world.loadInto = "forest";
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("another area loaded: forest"));
}

TEST_CASE("a fight that takes a move's end with it is said, though the end never comes", "[travel]")
{
    World world;
    world.shut = false;
    world.silent = true;
    world.fightOn = "move";
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("a fight began")); // and who keeps it
    REQUIRE(answer["steps"].size() == 1);
    CHECK_THAT(answer["steps"][0]["result"].get<std::string>(), ContainsSubstring("a fight began"));
}

TEST_CASE("others' commands ending, and the walker's own cleared one, are not the end waited for", "[travel]")
{
    World world;
    world.shut = false;
    world.chatter = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there");
    REQUIRE(answer["steps"].size() == 1);
    CHECK(answer["steps"][0]["result"] == "success");
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

TEST_CASE("a pin of the world map, the map up, is travelled to by its name in any case", "[travel]")
{
    World world;
    world.mapUp = true;
    const auto answer = world.bench.Invoke("travel", Json{{"to", "lothering"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "through to Lothering");
    CHECK(answer["area"]["tag"] == "lot100ar");
    REQUIRE(world.messages.size() == 1);
    CHECK(world.messages[0] == "GameModeController.AreaTransition ruins 1"); // where the party is, the pin
    // As the pin's own click: marked closed by travel, the index set, then
    // the transition; the party goes with the hero.
    REQUIRE(world.screenCalls.size() == 2);
    CHECK(world.screenCalls[1]["args"] == Json::array({"WorldMap.LastClickedPinIndex", 1}));
    CHECK(world.partyKept);
    CHECK(world.orders.empty());
}

TEST_CASE("of two pins of one name the active one is taken", "[travel]")
{
    for (const bool activeFirst : {true, false})
    {
        World world;
        world.mapUp = true;
        Json other = {{"index", 3},
                      {"name", "Lothering"},
                      {"tag", "wm_lothering_later"},
                      {"area", "lot900ar"},
                      {"state", "not active"}};
        if (activeFirst)
        {
            world.pins.push_back(other);
        }
        else
        {
            world.pins.insert(world.pins.begin() + 1, other);
            world.pins[1]["index"] = 1;
            world.pins[2]["index"] = 2;
            world.pins[3]["index"] = 3;
        }
        const auto answer = world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"budgetMs", 5000}}).data;
        CHECK(answer["arrived"] == true);
        CHECK(answer["pin"]["tag"] == "wm_lothering");
    }
}

TEST_CASE("on the Fade's own map a pin is clicked as its pin does, with no mark", "[travel]")
{
    World world;
    world.mapUp = true;
    world.fadeMap = true;
    const auto answer = world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    REQUIRE(world.screenCalls.size() == 1); // the index only: the Fade's map has no closed-by-travel mark
    CHECK(world.screenCalls[0]["screen"] == "FadeMap");
    CHECK(world.screenCalls[0]["call"] == "ExternalCommands.SetValue");
}

TEST_CASE("a pin is named by its tag or its index too", "[travel]")
{
    for (const Json &to : {Json("wm_lothering"), Json(1)})
    {
        World world;
        world.mapUp = true;
        const auto answer = world.bench.Invoke("travel", Json{{"to", to}, {"budgetMs", 5000}}).data;
        CHECK(answer["arrived"] == true);
    }
}

TEST_CASE("the exit to the world map, the map waited for, then the pin", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.mapExit = true;
    world.mapLate = 2; // up on the third read
    const auto answer =
        world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"to", "Lothering"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    CHECK(answer["area"]["tag"] == "lot100ar");
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["action"] == "use");
    CHECK(world.messages.size() == 1);
}

TEST_CASE("an exit with `to` that opens no map keeps its own reason", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.crosses = false;
    const auto answer =
        world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"to", "Lothering"}, {"budgetMs", 1000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not leave"));
    CHECK(world.messages.empty());
}

TEST_CASE("a random encounter on the way is said, not taken for the pin", "[travel]")
{
    World world;
    world.mapUp = true;
    world.encounter = "ran405ar_highway_dog";
    const auto answer = world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK(answer["encounter"] == true);
    CHECK(answer["area"]["tag"] == "ran405ar_highway_dog");
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("random encounter"));
}

TEST_CASE("a pin not active, or none of the name, is refused, saying the active ones", "[travel]")
{
    for (const std::string to : {"Denerim", "Orzammar"})
    {
        World world;
        world.mapUp = true;
        const auto answer = world.bench.Invoke("travel", Json{{"to", to}, {"budgetMs", 5000}}).data;
        CHECK(answer["arrived"] == false);
        CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the active ones: Lothering"));
        CHECK(world.messages.empty());
    }
}

TEST_CASE("a pin with the map not up and no exit says how to open it", "[travel]")
{
    World world;
    const auto answer = world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("no map is up"));
    CHECK(world.messages.empty());
}

TEST_CASE("`to` goes with `exit` only", "[travel]")
{
    World world;
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"target", "0x2ec"}}), ToolError);
    CHECK_THROWS_AS(world.bench.Invoke("travel", Json{{"to", "Lothering"}, {"area", "forest"}}), ToolError);
}

TEST_CASE("an exit's use that fails from too close is tried again from a step back", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.tooClose = true;
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 10000}}).data;
    CHECK(answer["arrived"] == true);
    REQUIRE(world.orders.size() == 3); // the use, the step back, the use again
    CHECK(world.orders[1]["action"] == "move");
    CHECK(world.orders[1]["point"][0] == 5.0); // 4 m further from the exit than the walker stood
    CHECK(world.orders[2]["action"] == "use");
}

TEST_CASE("an exit's use that fails again is said at once, not waited out", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.tooClose = true;
    world.stuck = true; // the step back goes nowhere: the use fails again
    const auto started = std::chrono::steady_clock::now();
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 30000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("ended COMMAND_FAILED"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
}

TEST_CASE("an exit the walk cannot reach is used from where the walk could not start", "[travel]")
{
    for (const std::string word : {"no walkable ground there", "none"})
    {
        World world;
        world.shut = false;
        world.at = 5;
        world.exitNoWalk = word; // the camp's exit; the windmill's door from below
        const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 5000}}).data;
        CHECK(answer["arrived"] == true);
        CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("through to Forest Clearing"));
        CHECK(answer["steps"][0]["result"] == "no walk there: " + word + ": used from here");
        REQUIRE(world.orders.size() == 1);
        CHECK(world.orders[0]["action"] == "use");
    }
}

TEST_CASE("a target with no ground under it that is not an exit is no walk there", "[travel]")
{
    World world;
    world.shut = false;
    world.exitNoWalk = "no walkable ground there";
    const auto answer = world.bench.Invoke("travel", Json{{"target", "0x1f5"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("no walkable ground"));
    CHECK(world.orders.empty());
}

TEST_CASE("a text blocker is passed over for an area, and refused by its id", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 5;
    world.blockerFirst = true;
    const auto answer = world.bench.Invoke("travel", Json{{"area", "forest"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == true);
    CHECK(answer["exit"]["id"] == "0x1f5");
    CHECK_THROWS_WITH(world.bench.Invoke("travel", Json{{"exit", "0x1f9"}, {"budgetMs", 5000}}),
                      ContainsSubstring("text blocker"));
}

TEST_CASE("a move that ends unseen, the walker left with no command, is not waited out", "[travel]")
{
    World world;
    world.shut = false;
    world.silent = true; // no end comes
    world.unseen = true; // and nothing is left to do
    const auto started = std::chrono::steady_clock::now();
    const auto answer = world.bench.Invoke("travel", Json{{"target", "0x2ec"}, {"budgetMs", 30000}}).data;
    CHECK(answer["arrived"] == true);
    CHECK(answer["steps"][0]["result"] == "ended unseen");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
}

TEST_CASE("a door whose key the party does not carry is said at once; carried, it is used", "[travel]")
{
    World world;
    world.keyRequired = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("key the party does not carry (DoesNotExist): 0x1f1 Metal Door"));
    CHECK(answer["shutBy"]["id"] == "0x1f1");
    CHECK(world.orders.empty());
    World holding;
    holding.keyRequired = true;
    holding.keyHeld = true;
    CHECK(holding.Travel()["arrived"] == true); // the key opens it: used on the way
}

TEST_CASE("a door locked with its key carried is used twice: the first use only unlocks it", "[travel]")
{
    World world;
    world.keyRequired = true;
    world.keyHeld = true;
    world.unlocksFirst = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    const auto uses = std::ranges::count_if(world.orders, [](const Json &order) { return order["action"] == "use"; });
    CHECK(uses == 2);
    CHECK(answer["opened"].size() == 1); // the one door, once
    World stays;                         // carried, but the door never opens: used twice, then said
    stays.keyRequired = true;
    stays.keyHeld = true;
    stays.opens = false;
    CHECK_THAT(stays.Travel()["reason"].get<std::string>(), ContainsSubstring("did not open"));
    CHECK(std::ranges::count_if(stays.orders, [](const Json &order) { return order["action"] == "use"; }) == 2);
}

TEST_CASE("a door shut but unlocked is used on the way, its key spent or its pick past reach", "[travel]")
{
    World spent; // the key unlocked it and was taken; the door fell shut behind the party
    spent.keyRequired = true;
    spent.doorState = 0;
    CHECK(spent.Travel()["arrived"] == true);
    World story; // the story unlocked it; its pick level stays the game's "cannot be picked"
    story.pickLevel = 10000;
    story.doorState = 0;
    CHECK(story.Travel()["arrived"] == true);
}

TEST_CASE("an exit named ahead is not used as a door: as far as the search got, then said", "[travel]")
{
    World world;
    world.exitAhead = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true); // a thing is used from as near as the walk goes
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("0x240e9 World Map, an exit"));
    CHECK(answer["exitAhead"]["id"] == "0x240e9");
    REQUIRE(world.orders.size() == 1);
    CHECK(world.orders[0]["action"] == "move"); // as far as the search got; nothing used
    World again;
    again.exitAhead = true;
    const auto point = again.bench.Invoke("travel", Json{{"point", {0, 0, 0}}, {"budgetMs", 5000}}).data;
    CHECK(point["arrived"] == false); // a point is not reached
    CHECK_THAT(point["reason"].get<std::string>(), ContainsSubstring("no walk there: 0x240e9"));
}

TEST_CASE("a thing in the way that is not interactive is walked up to, not used", "[travel]")
{
    World world; // a trigger beside it removes it as the walker comes up
    world.inert = true;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
    CHECK(answer["reason"] == "there");
    CHECK(std::ranges::none_of(world.orders, [](const Json &order) { return order["action"] == "use"; }));
    World stays; // nothing removes it: said, naming it, after going as far as the search got
    stays.inert = true;
    stays.inertStays = true;
    const auto point = stays.bench.Invoke("travel", Json{{"point", {0, 0, 0}}, {"budgetMs", 5000}}).data;
    CHECK(point["arrived"] == false);
    CHECK_THAT(point["reason"].get<std::string>(), ContainsSubstring("0x2a1b4 Invisible Object stands in the way"));
    CHECK(point["inertAhead"]["id"] == "0x2a1b4");
    REQUIRE(stays.orders.size() == 1);
    CHECK(stays.orders[0]["action"] == "move");
    World beside; // a thing as the target, behind it: used from as near as the walk goes
    beside.inert = true;
    beside.inertStays = true;
    const auto target = beside.Travel();
    CHECK(target["arrived"] == true);
    CHECK_THAT(target["reason"].get<std::string>(), ContainsSubstring("as near as the walk goes: 0x2a1b4"));
}

TEST_CASE("a door the story keeps shut is said at once, neither used nor walked to", "[travel]")
{
    World world;
    world.pickLevel = 10000;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("cannot be opened: 0x1f1 Metal Door"));
    CHECK(answer["shutBy"]["id"] == "0x1f1");
    CHECK(world.orders.empty());
}

TEST_CASE("a door whose use ends unseen and stays shut is said not to open, not used for ever", "[travel]")
{
    World world;
    world.opens = false;    // the use does nothing
    world.silentUse = true; // and its end is never said
    world.unseen = true;    // the walker left with no command
    const auto answer = world.bench.Invoke("travel", Json{{"target", "0x2ec"}, {"budgetMs", 30000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open"));
    CHECK(world.orders.size() == 1); // used once, then said, not used again
    CHECK(answer["steps"][0]["result"] == "ended unseen");
}

TEST_CASE("a quiet move whose walker still has it is waited for, not cut short", "[travel]")
{
    World world;
    world.shut = false;
    world.silent = true; // no end comes, but the command stays the walker's
    const auto answer = world.bench.Invoke("travel", Json{{"target", "0x2ec"}, {"budgetMs", 3000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("time ran out"));
}

TEST_CASE("in combat mode with no hostile near, the journey walks on", "[travel]")
{
    World world;
    world.shut = false;
    world.mode = "combat";
    world.hostileAt = 40; // a band far off that has seen the party keeps the mode
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == true);
}

TEST_CASE("in combat mode with a hostile near, the journey stops and names it", "[travel]")
{
    World world;
    world.shut = false;
    world.mode = "combat";
    world.hostileAt = 5;
    const auto answer = world.Travel();
    CHECK(answer["arrived"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Shade 0x5a1 is 5 m off"));
    CHECK(world.orders.empty());
    world.hostileAt = 40;
    CHECK(
        world.bench.Invoke("travel", Json{{"target", "0x2ec"}, {"fightNear", 0}, {"budgetMs", 5000}}).data["arrived"] ==
        false); // 0: any fight stops it, as before
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

TEST_CASE("an exit that is not active is not taken to the area it leads to", "[travel]")
{
    World world;
    world.exitActive = false;
    CHECK_THROWS_WITH(world.bench.Invoke("travel", Json{{"area", "forest"}, {"budgetMs", 5000}}),
                      ContainsSubstring("no active exit here leads to forest"));
    CHECK(world.orders.empty());
}

TEST_CASE("an exit not reached is not used", "[travel]")
{
    World world;
    world.paused = true;
    const auto answer = world.bench.Invoke("travel", Json{{"area", "forest"}, {"budgetMs", 5000}}).data;
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "the game is paused");
    CHECK(world.orders.empty());
    CHECK(world.area == "ruins");
}

TEST_CASE("a crossing waits while the other area loads, and answers once it is in place", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.loads = true; // the game reads nothing while it loads: frame tools fail
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 5000}}).data;
    REQUIRE(answer["arrived"] == true);
    CHECK(answer["area"]["tag"] == "forest");
    CHECK(answer["mode"] == "explore");
    CHECK(world.readsWhileLoading == 0); // health said loading: no read that waits for a frame
}

TEST_CASE("a fight before the party leaves stops the crossing, and says so", "[travel]")
{
    World world;
    world.shut = false;
    world.at = 1;
    world.crosses = false;
    world.fightOn = "use";
    const auto answer = world.bench.Invoke("travel", Json{{"exit", "0x1f5"}, {"budgetMs", 1500}}).data;
    CHECK(answer["arrived"] == false);
    CHECK(answer["reason"] == "a fight began before the party left");
}
