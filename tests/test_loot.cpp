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
    std::string panel;        // the container whose loot panel is up
    bool arrives = true;      // whether the walk there arrives
    bool opens = true;        // whether a use opens it
    bool starts = true;       // whether a use starts at all: on empty remains it does not
    bool full = false;        // whether the backpack is full
    Json popup = nullptr;     // a message box the use brings instead
    std::string opensInstead; // another container, whose panel the use opens
    std::string brings;       // the mode the use brings: a fight, a conversation
    bool chatter = false;     // whether a companion's use ends as the looter's is ordered
    int startsLate = 0;       // gui reads after the order before the use starts: the looter still on its way
    int panelLate = 0;        // gui reads after the use before its panel comes
    bool refuses = false;     // whether the game refuses the use: an area loading
    Json items = Json::array({Json{{"id", "0x500"}, {"name", "Greenstone"}}});
    std::vector<std::string> calls; // the tools called, in order
    Json walkArgs;                  // what the walk there was given
    std::string used;               // the container the use was ordered on
    std::string mode;               // the mode the use brought, if any
    int startIn = 0;                // gui reads until the use starts; 0: started, or none ordered
    int panelIn = 0;                // gui reads until its panel comes; 0: come, or none coming
    bool walkOpens = false;         // whether the walk opens the target itself on the way, stopped by its pause

    Chest()
    {
        bench.Tools().Add(ToolSpec{"travel", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
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
            return Json{{"arrived", arrives},
                        {"reason", arrives ? "there" : "a fight began"},
                        {"distance", 1.0},
                        {"opened", Json::array()}};
        });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
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
        });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            if (args.value("kind", "") == "inventory")
            {
                return Json{{"count", items.size()}, {"items", items}};
            }
            // Time passes between reads: a late use starts, a late panel comes.
            if (startIn > 0 && --startIn == 0)
            {
                Start();
            }
            else if (panelIn > 0 && --panelIn == 0)
            {
                panel = Opened();
            }
            return Json{{"mode", !mode.empty()   ? mode
                                 : panel.empty() ? "explore"
                                                 : "gui"},
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

    // The use, as the game's: it ends on the bus (on empty remains it never
    // starts), and opens the panel, now or some reads later.
    void Start()
    {
        if (starts)
        {
            bench.Events().Emit("eluvian.command.ended",
                                Json{{"id", "0x82"}, {"type", "UseObject"}, {"result", "success"}});
        }
        if (opens && panelLate > 0)
        {
            panelIn = panelLate;
        }
        else if (opens)
        {
            panel = Opened();
        }
        mode = brings;
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
    chest.chatter = true; // a companion's use, which is not the looter's, ends meanwhile
    const auto answer = chest.Loot(Json{{"target", "0x430"}});
    CHECK(answer["looted"] == false);
    REQUIRE(answer.contains("empty")); // the use was seen to start nothing
    CHECK(answer["empty"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("holds 1 item "));
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
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("game action=press"));
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
