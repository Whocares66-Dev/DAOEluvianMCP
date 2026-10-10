#include "core/Loot.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A chest and the tools around it, answering as the game's do: travel the
// walk, order the use (which opens the panel and ends on the bus), inspect
// kind=gui the panel, and press its Take All.
struct Chest
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string panel;        // the container whose loot panel is up
    bool arrives = true;      // whether the walk there arrives
    bool opens = true;        // whether a use opens it
    bool starts = true;       // whether a use starts at all: on empty remains it does not
    bool full = false;        // whether the backpack is full
    bool staysUp = false;     // whether the panel stays up after Take All, though all was taken
    Json popup = nullptr;     // a message box the use brings instead
    std::string opensInstead; // another container, whose panel the use opens
    std::string brings;       // the mode the use brings: a fight, a conversation
    bool chatter = false;     // whether a companion's use ends as the looter's is ordered
    int startsLate = 0;       // gui reads after the order before the use starts: the looter still on its way
    int panelLate = 0;        // gui reads after the use before its panel comes
    bool refuses = false;     // whether the game refuses the use: an area loading
    Json items = Json::array({Json{{"id", "0x500"}, {"name", "Greenstone"}}});
    std::vector<std::string> calls;  // the tools called, in order
    Json walkArgs;                   // what the walk there was given
    std::string used;                // the container the use was ordered on
    std::string mode;                // the mode the use brought, if any
    int startIn = 0;                 // gui reads until the use starts; 0: started, or none ordered
    int panelIn = 0;                 // gui reads until its panel comes; 0: come, or none coming
    bool walkOpens = false;          // whether the walk opens the target itself on the way, stopped by its pause
    std::string controlled = "0x82"; // the creature controlled
    double shortBy = 0;              // a walk that ends this short with no way the last of it, if not 0
    Json around = Json::array();     // the placeables inspect kind=placeables lists, nearest first
    std::set<std::string> shutTight; // those whose use opens no panel (locked, or an _autoloot herb)
    std::string fullAt;              // the one whose Take All finds the backpack full
    std::string fightAt;             // the one whose use brings a fight

    Chest()
    {
        bench.Tools().Add(ToolSpec{"travel", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeTravel(args); });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeOrder(args); });
        AddReads(bench, [this](const Json &args) -> ToolResult { return FakeInspect(args); });
        // control, recorded as order's control was.
        bench.Tools().Add(ToolSpec{"control", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            Json control = args;
            control["action"] = "control";
            return FakeOrder(control);
        });
        // press, recorded as game's press was.
        bench.Tools().Add(ToolSpec{"press", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            Json pressed = args;
            pressed["action"] = "press";
            return FakeGame(pressed);
        });
        AddLootTool(bench);
    }

    // The stand-in's travel.
    ToolResult FakeTravel(const Json &args)
    {
        calls.emplace_back("travel");
        walkArgs = args;
        if (walkOpens)
        {
            panel = args.value("target", "");
            return Json{{"arrived", false},
                        {"reason", "the game is paused"},
                        {"distance", 2.0},
                        {"opened", Json::array({Json{{"id", panel}}})}};
        }
        if (shortBy > 0)
        {
            return Json{{"arrived", false},
                        {"reason", "the walk failed: COMMAND_FAILED_INVALID_PATH"},
                        {"distance", shortBy},
                        {"mode", "explore"},
                        {"opened", Json::array()}};
        }
        return Json{{"arrived", arrives},
                    {"mode", arrives ? "explore" : "combat"},
                    {"reason", arrives ? "there" : "a fight began"},
                    {"distance", 1.0},
                    {"opened", Json::array()}};
    }

    // The stand-in's order.
    ToolResult FakeOrder(const Json &args)
    {
        if (args.value("action", "") == "control")
        {
            const std::string id = args.value("id", "");
            calls.emplace_back("order control " + id);
            const Json before = {{"id", controlled}};
            const bool taken = id != "0xbad"; // one that will not take it
            if (taken)
            {
                controlled = id;
            }
            return Json{
                {"action", "control"}, {"before", before}, {"controlled", {{"id", controlled}}}, {"taken", taken}};
        }
        calls.emplace_back("order " + args.value("action", ""));
        if (refuses)
        {
            throw ToolError("order: an area is loading; wait_for topic \"eluvian.mode.changed\" and try again");
        }
        const auto cursor = bench.Events().Next();
        used = args.value("target", "");
        if (chatter)
        {
            bench.Events().Emit("eluvian.command.ended",
                                Json{{"id", "0x10d"}, {"type", "UseObject"}, {"result", "success"}});
        }
        if (startsLate > 0)
        {
            startIn = startsLate;
        }
        else
        {
            Start();
        }
        return Json{{"given", true}, {"creature", {{"id", "0x82"}}}, {"cursor", cursor}};
    }

    // The stand-in's inspect.
    ToolResult FakeInspect(const Json &args)
    {
        if (args.value("kind", "") == "placeables")
        {
            calls.emplace_back("inspect placeables " + args.value("near", ""));
            return Json{{"placeables", around}};
        }
        if (args.value("kind", "") == "inventory")
        {
            return Json{{"count", items.size()}, {"items", items}};
        }
        // Time passes between reads: a late use starts, a late panel comes.
        bool started = false;
        if (startIn > 0)
        {
            --startIn;
            started = startIn == 0;
        }
        if (started)
        {
            Start();
        }
        else if (panelIn > 0)
        {
            --panelIn;
            if (panelIn == 0)
            {
                panel = Opened();
            }
        }
        std::string shown = mode;
        if (shown.empty())
        {
            shown = panel.empty() ? "explore" : "gui";
        }
        return Json{{"mode", shown},
                    {"popup", calls.empty() || calls.back() != "order use" ? Json(nullptr) : popup},
                    {"loot", panel.empty() ? Json(nullptr) : Panel()}};
    }

    // The stand-in's game.
    ToolResult FakeGame(const Json &args)
    {
        calls.emplace_back("game " + args.value("action", "") + " " + args.value("button", ""));
        if (panel.empty())
        {
            throw ToolError("game: no message box and no loot panel is up");
        }
        if (args.value("button", "") == "Close")
        {
            panel.clear();
            return Json{{"panel", nullptr}};
        }
        const bool isFull = full || panel == fullAt;
        Json answer = {{"container", {{"id", panel}}},
                       {"taken", isFull ? Json::array() : items},
                       {"left", isFull ? items : Json::array()},
                       {"full", isFull}};
        if (!isFull && !staysUp)
        {
            panel.clear();
        }
        answer["panel"] = panel.empty() ? Json(nullptr) : Panel();
        return answer;
    }

    // The use, as the game's: it ends on the bus (on empty remains it never
    // starts), and opens the panel, now or some reads later.
    void Start()
    {
        if (starts)
        {
            bench.Events().Emit("eluvian.command.ended",
                                Json{{"id", "0x82"}, {"type", "UseObject"}, {"result", "success"}});
        }
        const bool opening = opens && !shutTight.contains(used);
        if (opening && panelLate > 0)
        {
            panelIn = panelLate;
        }
        else if (opening)
        {
            panel = Opened();
        }
        mode = !fightAt.empty() && used == fightAt ? "combat" : brings;
    }

    [[nodiscard]] std::string Opened() const
    {
        return opensInstead.empty() ? used : opensInstead;
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

TEST_CASE("the panel stays up a moment before Take All for whoever watches, as long as showMs says", "[loot]")
{
    Chest chest;
    const auto start = std::chrono::steady_clock::now();
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}, {"showMs", 300}});
    CHECK(answer["looted"] == true);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(300));
}

TEST_CASE("a target the walk itself opened is taken from, its panel's pause no stop", "[loot]")
{
    Chest chest;
    chest.walkOpens = true;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["taken"].size() == 1);
    CHECK(chest.calls == std::vector<std::string>{"travel", "game press Take All"}); // no use after the walk
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

TEST_CASE("the target's own panel up is taken from, with no walk and no use", "[loot]")
{
    Chest chest;
    chest.panel = "0x3ec";
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(chest.calls == std::vector<std::string>{"game press Take All"});
}

TEST_CASE("the walk there has what is left of the loot's budget", "[loot]")
{
    Chest chest;
    chest.Loot(Json{{"target", "0x3ec"}});
    const auto given = chest.walkArgs.value("budgetMs", 0);
    CHECK(given > 0);
    CHECK(given <= 5000);
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
    CHECK_THROWS_WITH(chest.Loot(Json::object()), ContainsSubstring("placeables lists chests"));
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

TEST_CASE("an empty body whose use ends with no panel is said to hold nothing, no lock named", "[loot]")
{
    Chest chest;
    chest.opens = false;
    chest.items = Json::array();
    const auto answer = chest.Loot(Json{{"target", "0x4c65"}});
    CHECK(answer["looted"] == false);
    CHECK(answer["empty"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("nothing there"));
    CHECK_THAT(answer["reason"].get<std::string>(), !ContainsSubstring("unlock"));
}

TEST_CASE("a container that does not open says so once its use has ended", "[loot]")
{
    Chest chest;
    chest.opens = false;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the use ended success")); // its end was seen
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
    chest.chatter = true; // a companion's use, which is not the looter's, ends meanwhile
    const auto answer = chest.Loot(Json{{"target", "0x430"}});
    CHECK(answer["looted"] == false);
    REQUIRE(answer.contains("empty")); // the use was seen to start nothing
    CHECK(answer["empty"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("holds 1 item."));
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("the use started nothing")); // not the companion's
}

TEST_CASE("a use the game refuses ends the looting with the refusal, not an error", "[loot]")
{
    Chest chest;
    chest.refuses = true;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("an area is loading"));
    CHECK(answer["walk"]["arrived"] == true);
}

TEST_CASE("a message box instead of the panel is said, with how to answer it", "[loot]")
{
    Chest chest;
    chest.opens = false;
    chest.popup = Json{{"text", "It is locked."}};
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("It is locked."));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("press"));
}

TEST_CASE("another container's panel opening instead is said, and whose", "[loot]")
{
    Chest chest;
    chest.opensInstead = "0x3ed";
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the loot panel of 0x3ed opened, not 0x3ec's"));
}

TEST_CASE("a fight or a conversation the use brings is said", "[loot]")
{
    for (const char *brings : {"combat", "dialog"})
    {
        Chest chest;
        chest.opens = false;
        chest.brings = brings;
        const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
        CHECK(answer["looted"] == false);
        CHECK(answer["reason"] == (chest.brings == "combat" ? "a fight began" : "a conversation took the screen"));
    }
}

TEST_CASE("a use that starts late, the looter still on its way, is waited for", "[loot]")
{
    Chest chest;
    chest.startsLate = 2; // reads, a quarter of a second apart
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    REQUIRE(answer["looted"] == true);
    CHECK(answer["taken"].size() == 1);
}

TEST_CASE("a panel that comes a moment after the use has ended is waited for", "[loot]")
{
    Chest chest;
    chest.panelLate = 2;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    REQUIRE(answer["looted"] == true);
    CHECK(answer["closed"] == true);
}

TEST_CASE("a panel that stays up after Take All is said to", "[loot]")
{
    Chest chest;
    chest.staysUp = true;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["closed"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the panel is still up"));
}

TEST_CASE("a full backpack leaves the rest in the container and closes its panel", "[loot]")
{
    Chest chest;
    chest.full = true;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["full"] == true);
    CHECK(answer["closed"] == true);
    CHECK(answer["left"].size() == 1);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("its panel closed"));
    CHECK(chest.calls.back() == "game press Close");
}

TEST_CASE("another party member loots with control handed to it, and given back after", "[loot]")
{
    Chest chest;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}, {"id", "0x7d9"}});
    CHECK(answer["looted"] == true);
    CHECK(chest.calls == std::vector<std::string>{"order control 0x7d9", "travel", "order use", "game press Take All",
                                                  "order control 0x82"});
    CHECK(answer["controlled"]["id"] == "0x82");
    CHECK(chest.controlled == "0x82");
}

TEST_CASE("control is given back though the looting stops short, or is refused", "[loot]")
{
    Chest stops;
    stops.arrives = false;
    const auto stopped = stops.Loot(Json{{"target", "0x3ec"}, {"id", "0x7d9"}});
    CHECK(stopped["looted"] == false);
    CHECK(stops.controlled == "0x82");

    Chest other; // another's panel up: refused, after control was handed over
    other.panel = "0x3ed";
    CHECK_THROWS_AS(other.Loot(Json{{"target", "0x3ec"}, {"id", "0x7d9"}}), ToolError);
    CHECK(other.controlled == "0x82");
}

TEST_CASE("a party member who will not take control loots nothing", "[loot]")
{
    Chest chest;
    CHECK_THROWS_WITH(chest.Loot(Json{{"target", "0x3ec"}, {"id", "0xbad"}}),
                      ContainsSubstring("would not take control"));
    CHECK(chest.calls == std::vector<std::string>{"order control 0xbad"});
}

TEST_CASE("the one controlled named as the looter is no hand-over", "[loot]")
{
    Chest chest;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}, {"id", "0x82"}});
    CHECK(answer["looted"] == true);
    CHECK_FALSE(answer.contains("controlled"));
    CHECK(chest.calls.back() == "game press Take All");
}

TEST_CASE("a walk that ends a little short of a large container is followed by the use, which walks the rest", "[loot]")
{
    Chest chest;
    chest.shortBy = 1.9; // the high dragon's remains
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == true);
    CHECK(answer["walk"]["arrived"] == false);
    CHECK_THAT(answer["walk"]["note"].get<std::string>(), ContainsSubstring("walks the rest"));
    CHECK(chest.calls == std::vector<std::string>{"travel", "order use", "game press Take All"});
}

TEST_CASE("a walk that ends far short is said, and nothing is used", "[loot]")
{
    Chest chest;
    chest.shortBy = 12;
    const auto answer = chest.Loot(Json{{"target", "0x3ec"}});
    CHECK(answer["looted"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("INVALID_PATH"));
    CHECK(chest.calls == std::vector<std::string>{"travel"});
}

namespace
{

// A placeable as inspect kind=placeables lists it, with its action.
Json Placeable(const std::string &id, int action, const std::string &actionName, const std::string &tag = "chest")
{
    return Json{
        {"id", id}, {"name", id}, {"tag", tag}, {"interactive", true}, {"action", action}, {"actionName", actionName}};
}

} // namespace

TEST_CASE("loot with a radius takes each placeable whose use is a loot, nearest first, and passes the rest", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e1", 11, "loot"), Placeable("0x3e2", 15, "area transition", "to_market"),
                                Placeable("0x3e3", 5, "use", "lever"), Placeable("0x3e4", 11, "loot")});
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["looted"] == true);
    CHECK(answer["count"] == 2);
    CHECK(answer["stopped"].is_null());
    CHECK(answer["reason"] == "2 of 2 containers looted");
    REQUIRE(answer["containers"].size() == 2);
    CHECK(answer["containers"][0]["target"]["id"] == "0x3e1");
    CHECK(answer["containers"][1]["target"]["id"] == "0x3e4");
    REQUIRE(answer["skipped"].size() == 2);
    CHECK(answer["skipped"][0]["action"] == "area transition");
    CHECK(answer["skipped"][1]["action"] == "use");
    CHECK(chest.calls == std::vector<std::string>{"inspect placeables ", "travel", "order use", "game press Take All",
                                                  "travel", "order use", "game press Take All"});
}

TEST_CASE("a sweep passes a locked one and an interactive-less one, and goes on", "[loot]")
{
    Chest chest;
    Json quiet = Placeable("0x3e0", 11, "loot");
    quiet["interactive"] = false;
    chest.around = Json::array({quiet, Placeable("0x3e1", 3, "unlock"), Placeable("0x3e4", 11, "loot")});
    chest.shutTight = {"0x3e1"};
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["count"] == 1);
    CHECK(answer["stopped"].is_null());
    REQUIRE(answer["containers"].size() == 2); // the locked one was tried: the looter may pick it
    CHECK(answer["containers"][0]["looted"] == false);
    CHECK(answer["containers"][1]["looted"] == true);
    REQUIRE(answer["skipped"].size() == 1);
    CHECK(answer["skipped"][0]["id"] == "0x3e0");
}

TEST_CASE("a sweep stops at a full backpack", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e1", 11, "loot"), Placeable("0x3e4", 11, "loot")});
    chest.fullAt = "0x3e1";
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["stopped"] == "the backpack is full");
    REQUIRE(answer["containers"].size() == 1); // the next not walked to
    CHECK(answer["containers"][0]["full"] == true);
    CHECK(std::count(chest.calls.begin(), chest.calls.end(), "travel") == 1);
}

TEST_CASE("a sweep stops at a fight a use brings, the next not walked to", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e1", 11, "loot"), Placeable("0x3e4", 11, "loot")});
    chest.fightAt = "0x3e1";
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["stopped"] == "a fight began");
    CHECK(answer["containers"].size() == 1);
    CHECK(std::count(chest.calls.begin(), chest.calls.end(), "travel") == 1);
}

TEST_CASE("a sweep picks an _autoloot herb, whose use gives its items with no panel", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e5", 11, "loot", "plt_autoloot_elfroot")});
    chest.shutTight = {"0x3e5"};
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["count"] == 1);
    REQUIRE(answer["containers"].size() == 1);
    CHECK(answer["containers"][0]["picked"] == true);
    CHECK(answer["containers"][0]["looted"] == true);
}

TEST_CASE("a container whose use opens no panel, not an _autoloot one, is not taken for picked", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e5", 11, "loot")});
    chest.shutTight = {"0x3e5"};
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}});
    CHECK(answer["count"] == 0);
    CHECK_FALSE(answer["containers"][0].contains("picked"));
}

TEST_CASE("a sweep for a party member lists around it and hands control over once", "[loot]")
{
    Chest chest;
    chest.around = Json::array({Placeable("0x3e1", 11, "loot"), Placeable("0x3e4", 11, "loot")});
    const auto answer = chest.Loot(Json{{"radius", 30}, {"showMs", 0}, {"id", "0x7d9"}});
    CHECK(answer["count"] == 2);
    CHECK(chest.calls.front() == "order control 0x7d9");
    CHECK(chest.calls[1] == "inspect placeables 0x7d9");
    CHECK(chest.calls.back() == "order control 0x82");
    CHECK(std::count(chest.calls.begin(), chest.calls.end(), "order control 0x7d9") == 1);
}

TEST_CASE("radius and target together are refused", "[loot]")
{
    Chest chest;
    CHECK_THROWS_WITH(chest.Loot(Json{{"radius", 30}, {"target", "0x3ec"}}), ContainsSubstring("give one"));
    CHECK(chest.calls.empty());
}
