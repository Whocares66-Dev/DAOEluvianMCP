#include "core/Ready.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A game that goes through `modes`, one a health call, the last kept; its
// conversation at a choice from `choiceAt` health calls on, its interface
// a message box or a full screen as set.
struct Game
{
    Bench bench;
    std::vector<std::string> modes;
    std::atomic<int> calls{0};
    int choiceAt = 0;
    Json popup = nullptr;
    Json fullScreen = nullptr;

    explicit Game(std::vector<std::string> sequence) : modes(std::move(sequence))
    {
        AddWaitReadyTool(bench);
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            const auto at = static_cast<std::size_t>(calls.fetch_add(1));
            const std::string mode = modes.at(std::min(at, modes.size() - 1));
            if (at > 0 && mode != modes.at(std::min(at - 1, modes.size() - 1)))
            {
                bench.Events().Emit("eluvian.mode.changed", Json{{"to", mode}});
            }
            return Json{{"mode", mode}};
        });
        AddReads(bench, [this](const Json &args) -> ToolResult {
            if (args.value("kind", "") == "dialogue")
            {
                const bool choice = calls >= choiceAt;
                return Json{{"active", true},
                            {"replies", choice ? Json::array({Json{{"index", 0}, {"text", "Yes"}}}) : Json::array()}};
            }
            return Json{{"popup", popup}, {"fullScreen", fullScreen}};
        });
    }

    Json Wait(int timeoutMs = 5000)
    {
        return bench.Invoke("wait_ready", Json{{"timeoutMs", timeoutMs}, {"intervalMs", 50}}).data;
    }
};

} // namespace

TEST_CASE("wait_ready answers when a load and a cutscene are over, with what passed", "[ready]")
{
    Game game({"loading", "loading", "cutscene", "explore"});
    game.bench.Events().Emit("eluvian.game.saved", Json{{"kind", "auto"}}); // before the wait: not passed
    const auto answer = game.Wait();
    CHECK(answer["ready"] == true);
    CHECK(answer["mode"] == "explore");
    CHECK(answer["what"] == "explore");
    REQUIRE(answer["passed"].size() == 2);
    CHECK(answer["passed"][0]["mode"] == "cutscene");
    CHECK(answer["passed"][1]["mode"] == "explore");
}

TEST_CASE("a conversation takes input only at a choice", "[ready]")
{
    Game game({"dialog"});
    game.choiceAt = 3; // two lines spoken first
    const auto answer = game.Wait();
    CHECK(answer["ready"] == true);
    CHECK(answer["what"] == "a conversation at a choice");
    CHECK(game.calls >= 3);
}

TEST_CASE("an interface takes input with a message box or a full screen up, not between them", "[ready]")
{
    Game boxed({"gui"});
    boxed.popup = Json{{"text", "Gather your party and venture forth?"}};
    CHECK(boxed.Wait()["what"] == "a message box");
    Game picker({"gui"});
    picker.fullScreen = "PartyPicker";
    CHECK(picker.Wait()["what"] == "the PartyPicker screen");
    Game nothing({"gui"});
    const auto answer = nothing.Wait(200);
    CHECK(answer["ready"] == false);
    CHECK(answer["mode"] == "gui");
}

TEST_CASE("the party dead and the main menu take input; a game still loading when the time runs out does not",
          "[ready]")
{
    Game dead({"combat"});
    dead.modes = {"dead"};
    CHECK_THAT(dead.Wait()["what"].get<std::string>(), ContainsSubstring("dead"));
    Game menu({"pregame"});
    CHECK(menu.Wait()["what"] == "the main menu");
    Game stuck({"loading"});
    const auto answer = stuck.Wait(200);
    CHECK(answer["ready"] == false);
    CHECK(answer["mode"] == "loading");
    CHECK(answer["waitedMs"].get<double>() >= 150);
}
