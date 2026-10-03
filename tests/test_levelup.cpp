#include "core/LevelUp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A rogue with a level's points banked, and the level-up screen as the
// game answers it: input's messages open it, spend, end and close it, and
// inspect reads the creature and the screens.
struct Rogue
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    int levels = 1;          // levels' points banked
    int strength = 10;       // base
    bool opens = true;       // whether the screen opens: not in a fight
    bool onePerAuto = false; // whether an Auto Level Up spends one level's points, not all
    bool spends = true;      // whether an Auto Level Up spends anything at all
    bool up = false;         // the screen
    std::vector<int> abilities = {603, 100011};
    std::vector<std::string> sent; // input's messages, in order

    Rogue()
    {
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string kind = args.value("kind", "");
            if (kind == "game")
            {
                return Json{{"controlled", {{"id", "0x82"}, {"name", "Lyna"}}}};
            }
            if (kind == "gui")
            {
                return Json{{"fullScreen", up ? Json("LevelUp") : Json(nullptr)}};
            }
            if (kind == "abilities")
            {
                Json talents = Json::array();
                for (const int id : abilities)
                {
                    talents.push_back(Json{{"id", id}, {"name", "ability " + std::to_string(id)}});
                }
                return Json{{"talents", talents}};
            }
            return Json{
                {"name", "Lyna"},
                {"level", 2 + levels},
                {"canLevelUp", levels > 0},
                {"pointsToSpend", {{"attributes", 3 * levels}, {"skills", levels}, {"talentsOrSpells", levels}}},
                {"attributes", {{"strength", {{"base", strength}, {"total", strength + 2}}}}}};
        });
        bench.Tools().Add(ToolSpec{"input", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string text = args.value("message", "");
            sent.push_back(text);
            if (text.starts_with("GUI.GoToLevelUp"))
            {
                up = opens;
            }
            else if (text.starts_with("GUILevelUp.AutoLevelUp"))
            {
                const int spend = !spends ? 0 : (onePerAuto ? 1 : levels);
                for (int i = 0; i < spend; ++i)
                {
                    strength += 3;
                    abilities.push_back(8 + static_cast<int>(abilities.size()));
                }
                levels -= spend;
            }
            else if (text == "GUI.HideFullScreenGUI")
            {
                up = false;
            }
            return Json{{"sent", true}};
        });
        AddLevelUpTool(bench);
    }

    Json LevelUp(Json args)
    {
        return bench.Invoke("levelup", args).data;
    }
};

} // namespace

TEST_CASE("auto spends every point through the screen's own messages", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"auto", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["creature"]["id"] == "0x82"); // the one controlled, none being named
    CHECK(answer["attributes"]["strength"] == 3);
    REQUIRE(answer["gained"].size() == 1);
    CHECK(answer["gained"][0]["kind"] == "talent");
    CHECK(answer["after"]["canLevelUp"] == false);
    CHECK(rogue.sent == std::vector<std::string>{"GUI.GoToLevelUp 130 0", "GUILevelUp.AutoLevelUp 130",
                                                 "GUILevelUp.CharGenEnd 130 1 1", "GUI.HideFullScreenGUI"});
    CHECK_FALSE(rogue.up);
}

TEST_CASE("the screen stays up a moment before and after its Auto Level Up, as long as showMs says", "[levelup]")
{
    Rogue rogue;
    const auto start = std::chrono::steady_clock::now();
    const auto answer = rogue.LevelUp(Json{{"auto", true}, {"showMs", 200}});
    CHECK(answer["levelled"] == true);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(400));
}

TEST_CASE("two levels banked that auto spends one at a time take two rounds", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.onePerAuto = true;
    const auto answer = rogue.LevelUp(Json{{"id", "0x82"}, {"auto", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["rounds"] == 2);
    CHECK(answer["attributes"]["strength"] == 6);
    CHECK(answer["gained"].size() == 2);
}

TEST_CASE("without auto, or with no points to spend, it is refused and nothing is sent", "[levelup]")
{
    Rogue rogue;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", false}}), ContainsSubstring("only `auto`"));
    rogue.levels = 0;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", true}}), ContainsSubstring("no level-up points"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"id", "Lyna"}, {"auto", true}}), ContainsSubstring("like \"0x82\""));
    CHECK(rogue.sent.empty());
}

TEST_CASE("an Auto Level Up that spends nothing is said, and not asked again", "[levelup]")
{
    Rogue rogue;
    rogue.spends = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("spent nothing"));
    CHECK(answer["rounds"] == 1);
}

TEST_CASE("a screen that does not open ends the levelling, saying so", "[levelup]")
{
    Rogue rogue;
    rogue.opens = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open"));
    CHECK(rogue.sent.size() == 1); // nothing spent
}
