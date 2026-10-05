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
    int skillPoints = 1;                // a skill, 50, learnable, on its own points
    bool spell = false;                 // a spell, 60, learnable, on the talents' points
    std::vector<int> skillIcons = {50}; // the skills the skills' page shows
    bool learns = true;                 // whether a press on an icon learns, as the screen's SpendPoint does
    std::string scene;                  // the page shown
    int exitPoint = 0;                  // LevelUp.nExitPoint: LEVELUP_EXIT_ABORT 0 as the screen starts, COMPLETE 1
    std::vector<int> stored;            // the picks its icons stored, which an unfinished end reverts
    bool takesExit = true;              // whether the movie takes its exit point when set
    std::vector<std::string> sent;      // input's messages, in order
    std::vector<Json> movie;            // input's calls into the screen's movie, in order

    static constexpr const char *kContainer = "_level0.ScrollingAbilities_mc.Scrollable_mc.AbilityClassContainer84";

    // The chains a page shows, each its abilities' ids by icon.
    std::vector<std::pair<std::string, std::vector<int>>> Chains() const
    {
        const std::string under = std::string(kContainer) + ".AbilityChainContainer_mc.";
        if (scene == "Skills")
        {
            return {{under + "AbilityChain200", skillIcons}};
        }
        std::vector<int> talents;
        for (const auto &[id, state] : chain)
        {
            talents.push_back(id);
        }
        std::vector<std::pair<std::string, std::vector<int>>> chains = {{under + "AbilityChain95", talents}};
        if (spell)
        {
            chains.emplace_back(under + "AbilityChain96", std::vector<int>{60});
        }
        return chains;
    }

    // A press on an icon, as the screen's Learn and SpendPoint take it: a
    // point while the page has one, the selection sent.
    void Learn(int ability)
    {
        const bool skill = ability == 50;
        int &points = skill ? skillPoints : levels;
        if (!learns || points < 1)
        {
            return;
        }
        points -= 1;
        stored.push_back(ability);
        const std::string pick = "130 " + std::to_string(ability) + " 0";
        sent.push_back("GUICharGen.CharGenAssignAbility " + pick);
        sent.push_back("Paperdoll.CharGenAssignAbility " + pick);
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
    }

    // The screen's end, as its scene's end handler goes: aborted, every
    // pick its icons stored reverted (RevertAllChanges); then CharGenEnd
    // sent with the exit point.
    void End()
    {
        if (exitPoint == 0)
        {
            for (auto it = stored.rbegin(); it != stored.rend(); ++it)
            {
                std::erase(abilities, *it);
                (*it == 50 ? skillPoints : levels) += 1;
                for (std::size_t i = 0; i < chain.size(); ++i)
                {
                    if (chain[i].first == *it)
                    {
                        chain[i].second = "learnable";
                        if (i + 1 < chain.size())
                        {
                            chain[i + 1].second = "not yet";
                        }
                    }
                }
            }
        }
        stored.clear();
        sent.push_back("GUILevelUp.CharGenEnd 130 1 " + std::to_string(exitPoint));
    }

    // input with `screen`: the level-up screen's movie.
    Json Movie(const Json &args)
    {
        movie.push_back(args);
        if (args.contains("set"))
        {
            const bool exit = takesExit && args.at("set") == "LevelUp.nExitPoint";
            if (exit)
            {
                exitPoint = args.at("value").get<int>();
            }
            return Json{{"done", exit}};
        }
        if (args.contains("children"))
        {
            Json children = Json::array();
            if (!scene.empty() && args.at("children") == "ScrollingAbilities_mc.Scrollable_mc")
            {
                children.push_back(Json{{"depth", 0}, {"type", "string"}, {"path", kContainer}});
            }
            return Json{{"children", children}};
        }
        if (args.contains("get"))
        {
            const std::string path = args.at("get");
            if (path == "LevelUp.LEVELUP_EXIT_COMPLETE" || path == "LevelUp.LEVELUP_EXIT_ABORT")
            {
                return Json{{"type", "number"}, {"value", path.ends_with("COMPLETE") ? 1 : 0}};
            }
            if (path == "SpellsTalentsScene.nPointsToSpend" || path == "SkillsScene.nPointsToSpend")
            {
                return Json{{"type", "number"}, {"value", path.starts_with("Skills") ? skillPoints : levels}};
            }
            if (path == std::string(kContainer) + ".AbilityChainContainer_mc.m_aChains")
            {
                std::string list;
                for (const auto &[name, ids] : Chains())
                {
                    list += (list.empty() ? "" : ",") + name;
                }
                return Json{{"type", "string"}, {"value", list}};
            }
            for (const auto &[name, ids] : Chains())
            {
                for (std::size_t k = 0; k < ids.size(); ++k)
                {
                    if (path == name + ".icon" + std::to_string(k) + ".m_oAbility.nID")
                    {
                        return Json{{"type", "number"}, {"value", ids[k]}};
                    }
                }
            }
            return Json{{"type", "undefined"}, {"value", nullptr}};
        }
        const std::string call = args.at("call");
        if (call == "SceneManager.PlayScene")
        {
            scene = args.at("args").at(0);
        }
        for (const auto &[name, ids] : Chains())
        {
            for (std::size_t k = 0; k < ids.size(); ++k)
            {
                if (call == name + ".icon" + std::to_string(k) + ".OnButtonAction")
                {
                    Learn(ids[k]);
                }
            }
        }
        return Json{{"ran", true}, {"type", "undefined"}, {"value", nullptr}};
    }

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
                    {"spells", spell ? Json::array({Json{{"id", 60}, {"state", "learnable"}}}) : Json::array()},
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
            if (args.contains("screen"))
            {
                return Movie(args);
            }
            const std::string text = args.value("message", "");
            sent.push_back(text);
            if (text.starts_with("GUI.GoToLevelUp"))
            {
                up = opens;
                exitPoint = 0; // the screen's StartScene: LEVELUP_EXIT_ABORT
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
            else if (text.starts_with("GUICharGen.CharGenAssignAttribute"))
            {
                strength += std::stoi(text.substr(text.rfind(' ') + 1));
            }
            else if (text == "GUI.HideFullScreenGUI")
            {
                if (up && closes)
                {
                    End();
                }
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
    // The end is the screen's own, sent as it is hidden, finished.
    CHECK(rogue.sent == std::vector<std::string>{"GUI.GoToLevelUp 130 0", "GUILevelUp.AutoLevelUp 130",
                                                 "GUI.HideFullScreenGUI", "GUILevelUp.CharGenEnd 130 1 1"});
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
          std::vector<std::string>{"GUI.GoToLevelUp 130 0", "GUI.HideFullScreenGUI", "GUILevelUp.CharGenEnd 130 1 0"});
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
    // The selections are the screen's own, sent by its SpendPoint as each
    // icon is pressed; Eluvian sends only the page's chains and the end.
    CHECK(rogue.sent == std::vector<std::string>{
                            "GUI.GoToLevelUp 130 0", "GameModeController.FindAbilityChains 130 1 2",
                            "GUICharGen.CharGenAssignAbility 130 8 0", "Paperdoll.CharGenAssignAbility 130 8 0",
                            "GUICharGen.CharGenAssignAbility 130 9 0", "Paperdoll.CharGenAssignAbility 130 9 0",
                            "GUICharGen.CharGenAssignAttribute 130 1 2", "Paperdoll.CharGenAssignAttribute 130 1 2",
                            "GUI.HideFullScreenGUI", "GUILevelUp.CharGenEnd 130 1 1"});
}

TEST_CASE("an ability is taken by pressing its own icon, its page played once", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.LevelUp(Json{{"abilities", {8, 9}}, {"showMs", 0}});
    std::vector<std::string> calls;
    for (const auto &each : rogue.movie)
    {
        if (each.contains("call"))
        {
            calls.push_back(each.at("call").get<std::string>() +
                            (each.contains("args") ? " " + each.at("args").dump() : std::string()));
        }
    }
    const std::string chain = std::string(Rogue::kContainer) + ".AbilityChainContainer_mc.AbilityChain95";
    CHECK(calls == std::vector<std::string>{"SceneManager.PlayScene [\"SpellsTalents\"]",
                                            chain + ".icon1.OnButtonAction", chain + ".icon2.OnButtonAction"});
    CHECK(rogue.movie.front().at("screen") == "LevelUp");
}

TEST_CASE("a press the screen takes no point for is said, and what was taken before it", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.learns = false;
    const auto answer = rogue.LevelUp(Json{{"abilities", {8}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("the screen took no point for ability 8 when its icon was pressed"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("nothing was spent"));
    CHECK(std::ranges::find(rogue.sent, "GUILevelUp.CharGenEnd 130 1 0") != rogue.sent.end());
}

TEST_CASE("an ability its page does not show is said", "[levelup]")
{
    Rogue rogue;
    rogue.skillIcons = {51};
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 50}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("ability 50 is not shown on the screen's skills page"));
    CHECK(answer["givenBack"] == Json::array({8}));
}

TEST_CASE("a spell is pressed on the talents' and spells' page, its chains asked as spells", "[levelup]")
{
    Rogue rogue;
    rogue.spell = true;
    const auto answer = rogue.LevelUp(Json{{"abilities", {60}}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    CHECK(std::ranges::find(rogue.sent, "GameModeController.FindAbilityChains 130 2 2") != rogue.sent.end());
    CHECK(std::ranges::find(rogue.sent, "GUICharGen.CharGenAssignAbility 130 60 0") != rogue.sent.end());
}

TEST_CASE("a page that shows no abilities is said, not pressed blind", "[levelup]")
{
    Rogue rogue;
    rogue.skillIcons.clear();
    const auto answer = rogue.LevelUp(Json{{"abilities", {50}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Skills page showed no abilities after 5 s"));
}

TEST_CASE("an ability not learnable now refuses the lot, nothing spent, and says what is", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", {9}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("not yet"));
    CHECK(answer["offers"]["talents"][0]["id"] == 8);
    CHECK(rogue.sent.back() == "GUILevelUp.CharGenEnd 130 1 0");
    CHECK(std::ranges::find(rogue.sent, "GUILevelUp.CharGenEnd 130 1 0") != rogue.sent.end());
    CHECK(rogue.levels == 1);
}

TEST_CASE("more abilities than points stops when the kind's points are gone, giving back those taken", "[levelup]")
{
    Rogue rogue; // one level: one talent point
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 9}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(),
               ContainsSubstring("no talent or spell points are left for ability 9"));
    CHECK(answer["givenBack"] == Json::array({8}));
    // The screen, closed unfinished, reverted the press: 8 and its point back.
    CHECK(std::ranges::find(rogue.abilities, 8) == rogue.abilities.end());
    CHECK(rogue.levels == 1);
    CHECK(std::ranges::find(rogue.sent, "GUICharGen.CharGenAssignAbility 130 9 0") == rogue.sent.end());
}

TEST_CASE("a skill is counted on the skills' points, not the talents'", "[levelup]")
{
    Rogue rogue; // one talent point and one skill point
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 50}}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    CHECK(std::ranges::find(rogue.sent, "GameModeController.FindAbilityChains 130 3 2") != rogue.sent.end());
    CHECK(std::ranges::find(rogue.sent, "GUICharGen.CharGenAssignAbility 130 50 0") != rogue.sent.end());
    CHECK(rogue.scene == "Skills");
}

TEST_CASE("an ability refused after others were taken gives those back, the screen closed unfinished", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 12}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("gave back those taken before it"));
    CHECK(answer["givenBack"] == Json::array({8}));
    CHECK(rogue.levels == 1);
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

TEST_CASE("a level-up by name ends as the screen's Finish ends it, so the screen keeps its picks", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    const auto answer = rogue.LevelUp(Json{{"abilities", {8, 9}}, {"showMs", 0}});
    CHECK(answer["levelled"] == true);
    // Its exit point set to its own LEVELUP_EXIT_COMPLETE, then hidden: the
    // screen sends its end and reverts nothing.
    CHECK(std::ranges::find(rogue.movie, Json{{"set", "LevelUp.nExitPoint"}, {"value", 1.0}, {"screen", "LevelUp"}}) !=
          rogue.movie.end());
    CHECK(rogue.sent.back() == "GUILevelUp.CharGenEnd 130 1 1");
    CHECK(std::ranges::find(rogue.abilities, 8) != rogue.abilities.end());
    CHECK(std::ranges::find(rogue.abilities, 9) != rogue.abilities.end());
}

TEST_CASE("a screen that does not take its exit point is left up, not hidden to end aborted", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.takesExit = false;
    const auto answer = rogue.LevelUp(Json{{"abilities", {8}}, {"showMs", 0}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not take LevelUp.nExitPoint"));
    CHECK(std::ranges::find(rogue.sent, "GUI.HideFullScreenGUI") == rogue.sent.end());
}
