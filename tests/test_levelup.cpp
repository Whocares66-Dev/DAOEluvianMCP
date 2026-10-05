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
    bool closes = true;      // whether the screen closes when told
    bool onePerAuto = false; // whether an Auto Level Up spends one level's points, not all
    bool spends = true;      // whether an Auto Level Up spends anything at all
    bool up = false;         // the screen
    std::vector<int> abilities = {603, 100011};
    // The class's talent chain as the screen offers it: had, learnable, not yet.
    std::vector<std::pair<int, std::string>> chain = {{6, "has"}, {8, "learnable"}, {9, "not yet"}};
    int skillPoints = 1;           // a skill, 50, learnable, on its own points
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
            if (kind == "levelup")
            {
                Json talents = Json::array();
                for (const auto &[id, state] : chain)
                {
                    talents.push_back(Json{{"id", id}, {"state", state}});
                }
                return Json{
                    {"points", {{"attributes", 3 * levels}, {"talentsOrSpells", levels}, {"skills", skillPoints}}},
                    {"attributes", {{"strength", strength}}},
                    {"talents", talents},
                    {"spells", Json::array()},
                    {"skills", Json::array({Json{{"id", 50}, {"state", "learnable"}}})}};
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
            else if (text.starts_with("GUICharGen.CharGenAssignAbility"))
            {
                const auto at = text.find(' ', text.find(' ') + 1) + 1;
                const int ability = std::stoi(text.substr(at, text.find(' ', at) - at));
                abilities.push_back(ability);
                for (std::size_t i = 0; i < chain.size(); ++i)
                {
                    if (chain[i].first == ability)
                    {
                        chain[i].second = "has";
                        if (i + 1 < chain.size())
                        {
                            chain[i + 1].second = "learnable"; // the next in the chain opens
                        }
                    }
                }
                levels = 0;
            }
            else if (text.starts_with("GUICharGen.CharGenAssignAttribute"))
            {
                strength += std::stoi(text.substr(text.rfind(' ') + 1));
            }
            else if (text == "GUI.HideFullScreenGUI")
            {
                up = !closes;
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
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", false}}), ContainsSubstring("give one of"));
    rogue.levels = 0;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", true}}), ContainsSubstring("no level-up points"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"id", "Lyna"}, {"auto", true}}), ContainsSubstring("like \"0x82\""));
    CHECK(rogue.sent.empty());
}

TEST_CASE("chosen points named wrong are refused, saying what is wanted, and nothing is sent", "[levelup]")
{
    Rogue rogue;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"abilities", 8}}), ContainsSubstring("a list of ability ids"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"abilities", {"Riposte"}}}), ContainsSubstring("a number"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"attributes", {{"luck", 2}}}}), ContainsSubstring("strength, dexterity"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"attributes", {{"strength", 0}}}}), ContainsSubstring("1 or more"));
    CHECK(rogue.sent.empty());
}

TEST_CASE("a screen that will not close says how to close it", "[levelup]")
{
    Rogue rogue;
    rogue.closes = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not close"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("GUI.HideFullScreenGUI"));
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

TEST_CASE("offers are read with the screen up and nothing spent, the screen abandoned", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"offers", true}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    REQUIRE(answer["talents"].size() == 3); // an ability each, flat
    CHECK(answer["talents"][1]["id"] == 8);
    CHECK(answer["talents"][1]["state"] == "learnable");
    CHECK(rogue.sent ==
          std::vector<std::string>{"GUI.GoToLevelUp 130 0", "GUILevelUp.CharGenEnd 130 1 0", "GUI.HideFullScreenGUI"});
    CHECK(rogue.levels == 1); // nothing spent
}

TEST_CASE("chosen points are spent as the screen's buttons spend them, a chain in its order", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2; // a talent point for each
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 9}}, {"attributes", {{"strength", 2}}}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["attributes"]["strength"] == 2);
    CHECK(answer["gained"].size() == 2);
    CHECK(rogue.sent == std::vector<std::string>{
                            "GUI.GoToLevelUp 130 0", "GUICharGen.CharGenAssignAbility 130 8 0",
                            "Paperdoll.CharGenAssignAbility 130 8 0", "GUICharGen.CharGenAssignAbility 130 9 0",
                            "Paperdoll.CharGenAssignAbility 130 9 0", "GUICharGen.CharGenAssignAttribute 130 1 2",
                            "Paperdoll.CharGenAssignAttribute 130 1 2", "GUILevelUp.CharGenEnd 130 1 1",
                            "GUI.HideFullScreenGUI"});
}

TEST_CASE("an ability not learnable now refuses the lot, nothing spent, and says what is", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", {9}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("not yet"));
    CHECK(answer["offers"]["talents"][0]["id"] == 8);
    CHECK(rogue.sent.back() == "GUI.HideFullScreenGUI");
    CHECK(std::ranges::find(rogue.sent, "GUILevelUp.CharGenEnd 130 1 0") != rogue.sent.end());
    CHECK(rogue.levels == 1);
}

TEST_CASE("more abilities than points stops when the kind's points are gone, saying those taken", "[levelup]")
{
    Rogue rogue; // one level: one talent point
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 9}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("no talent or spell points are left for ability 9"));
    CHECK(answer["taken"] == Json::array({8}));
    CHECK(std::ranges::find(rogue.sent, "GUICharGen.CharGenAssignAbility 130 9 0") == rogue.sent.end());
}

TEST_CASE("a skill is counted on the skills' points, not the talents'", "[levelup]")
{
    Rogue rogue; // one talent point and one skill point
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 50}}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    CHECK(std::ranges::find(rogue.sent, "GUICharGen.CharGenAssignAbility 130 50 0") != rogue.sent.end());
}

TEST_CASE("an ability refused after others were taken says those taken", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 12}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("those before it were taken and kept"));
    CHECK(answer["taken"] == Json::array({8}));
    CHECK(std::ranges::find(rogue.sent, "GUILevelUp.CharGenEnd 130 1 0") != rogue.sent.end());
}

TEST_CASE("offers for a creature with no level-up waiting say so when the screen will not open", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 0;
    rogue.opens = false;
    const auto answer = rogue.LevelUp(Json{{"offers", true}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("`canLevelUp` false"));
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
