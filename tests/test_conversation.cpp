#include "core/Conversation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A conversation as the game plays it: each node a line the other speaks,
// then the replies offered, each leading to a node or (-1) the end. The
// stand-in step answers as dialogue.step does, the bus carries the lines,
// replies and choices as the game's events do, and inspect reads the screen.
struct Talker
{
    struct Node
    {
        std::string line;
        std::vector<std::string> replies; // "" is the game's continue
        std::vector<int> next;
    };

    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::vector<Node> nodes;
    int at = 0;
    bool active = true;
    bool ignoreOne = false; // whether the game ignores the first choice
    bool slow = false;      // whether a line's replies come only once it is cut short or done
    bool speaking = false;
    std::string endsInto = "explore"; // the mode the conversation's end leaves the game in
    bool silentEnd = false;           // whether the end comes with a stale "continue" and no mode change: a race
    bool blip = false;                // whether each line comes after a mode change back to dialog (out of a cutscene)
    std::vector<std::string> steps;

    Talker()
    {
        bench.Tools().Add(ToolSpec{"dialogue.step", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string action = args.value("action", "");
            if (!active)
            {
                throw ToolError("dialogue: no conversation is on the screen (health's mode is dialog while one is)");
            }
            steps.push_back(action == "choose" ? "choose " + std::to_string(args.value("place", -1)) : action);
            if (action == "continue")
            {
                if (speaking)
                {
                    speaking = false;
                    Offer();
                }
                else if (at >= 0 && nodes[at].replies.at(0).empty())
                {
                    Go(nodes[at].next.at(0));
                }
            }
            else if (action == "choose")
            {
                if (ignoreOne)
                {
                    ignoreOne = false;
                }
                else
                {
                    const int place = args.value("place", 0);
                    bench.Events().Emit("eluvian.dialogue.chosen", Json{{"text", nodes[at].replies.at(place)}});
                    Go(nodes[at].next.at(place));
                }
            }
            return Json{{"action", action}, {"asked", true}};
        });
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            return Json{{"active", active}, {"cutscene", false}, {"replies", speaking ? Json::array() : Replies()}};
        });
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", active ? "dialog" : endsInto}}; });
        AddDialogueTool(bench);
    }

    [[nodiscard]] Json Replies() const
    {
        Json replies = Json::array();
        if (at >= 0)
        {
            for (std::size_t i = 0; i < nodes[at].replies.size(); ++i)
            {
                replies.push_back(
                    Json{{"place", i}, {"index", at * 10 + static_cast<int>(i)}, {"text", nodes[at].replies[i]}});
            }
        }
        return replies;
    }

    void Offer()
    {
        bench.Events().Emit("eluvian.dialogue.replies", Json{{"replies", Replies()}});
    }

    void Go(int node)
    {
        at = node;
        if (node < 0)
        {
            // The old replies taken away, then the end: into a store, the
            // game's "replies: []" comes first (seen in play, 2026-10-03).
            if (silentEnd)
            {
                // The conversation gone under a "continue" the game had
                // already offered: the runner's continue is then refused.
                active = false;
                bench.Events().Emit("eluvian.dialogue.replies",
                                    Json{{"replies", Json::array({Json{{"place", 0}, {"index", 99}, {"text", ""}}})}});
                return;
            }
            bench.Events().Emit("eluvian.dialogue.replies", Json{{"replies", Json::array()}});
            active = false;
            bench.Events().Emit("eluvian.mode.changed", Json{{"from", "dialog"}, {"to", endsInto}});
            return;
        }
        if (blip)
        {
            bench.Events().Emit("eluvian.mode.changed", Json{{"from", "cutscene"}, {"to", "dialog"}});
        }
        bench.Events().Emit("eluvian.dialogue.line", Json{{"speaker", "OWNER"}, {"text", nodes[node].line}});
        speaking = slow;
        if (!slow)
        {
            Offer();
        }
    }

    Json Dialogue(Json args)
    {
        return bench.Invoke("dialogue", args).data;
    }
};

// A merchant's talk: a greeting, a "Hmm." with only the game's continue,
// then the choice of the wares.
void Merchant(Talker &talk)
{
    talk.nodes = {{"Welcome, traveller.", {"Hello.", "Goodbye."}, {1, -1}},
                  {"Hmm.", {""}, {2}},
                  {"What will it be?", {"Your wares.", "Nothing."}, {-1, -1}}};
}

} // namespace

TEST_CASE("a choice goes on to the next real one, with every line spoken on the way", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    REQUIRE(answer["lines"].size() == 2);
    CHECK(answer["lines"][0]["text"] == "Hmm.");
    CHECK(answer["lines"][1]["text"] == "What will it be?");
    REQUIRE(answer["replies"].size() == 2);
    CHECK(answer["replies"][0]["text"] == "Your wares.");
    CHECK(talk.steps == std::vector<std::string>{"choose 0", "continue"}); // the reply with no text gone through
}

TEST_CASE("the conversation's end is said, with the mode after", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 1}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["ended"] == true);
    CHECK(answer["mode"] == "explore");
    CHECK(answer["replies"].is_null());
}

TEST_CASE("a continue with a real choice already up answers it at once", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    const auto answer = talk.Dialogue(Json{{"action", "continue"}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    REQUIRE(answer["replies"].size() == 2);
    CHECK(answer["replies"][0]["text"] == "Hello.");
}

TEST_CASE("a conversation that ends into a store is said ended, with the mode it leaves", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    talk.endsInto = "gui";
    talk.at = 2; // "What will it be?": the wares
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    CHECK(answer["ended"] == true);
    CHECK(answer["mode"] == "gui");
}

TEST_CASE("a continue refused because the conversation is gone is its end, not a failure", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    talk.silentEnd = true;
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 1}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    CHECK(answer["ended"] == true);
    CHECK(answer["mode"] == "explore");
}

TEST_CASE("a mode change back to dialog goes on, it is not the end", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    talk.blip = true; // a cutscene in the conversation, and back
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    CHECK_FALSE(answer.contains("ended"));
    CHECK(answer["replies"].size() == 2);
}

TEST_CASE("a choice the game ignored is asked again", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    talk.ignoreOne = true;
    const auto answer =
        talk.Dialogue(Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    CHECK(talk.steps.front() == "choose 0");
    CHECK(talk.steps.at(1) == "choose 0");
}

TEST_CASE("with skipLines a line being spoken is cut short; without, it plays out", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    talk.slow = true;
    auto answer = talk.Dialogue(
        Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"skipLines", true}, {"budgetMs", 5000}});
    CHECK(answer["done"] == true);
    CHECK(answer["replies"].size() == 2);

    Talker patient;
    Merchant(patient);
    patient.slow = true;
    answer = patient.Dialogue(Json{{"action", "choose"}, {"place", 0}, {"until", "choice"}, {"budgetMs", 1500}});
    CHECK(answer["done"] == false); // nothing cut it short, and the line never ended here
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("time ran out"));
}

TEST_CASE("without until it is the one step", "[dialogue]")
{
    Talker talk;
    Merchant(talk);
    const auto answer = talk.Dialogue(Json{{"action", "choose"}, {"place", 0}});
    CHECK(answer["asked"] == true);
    CHECK(talk.steps == std::vector<std::string>{"choose 0"});
}
