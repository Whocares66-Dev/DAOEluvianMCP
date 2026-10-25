#include "core/LevelUp.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A rogue with a level's points banked, and the level-up as DAOLib's
// dao::levelup answers it (#180): levelup.offers, take, raise, auto and
// finish, each refused as the engine refuses it; and the level-up screen,
// shown for whoever watches, as input and movie open and end it.
struct Rogue
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    int levels = 1;          // talent points, one a level
    int skillPoints = 1;     // a skill, 50, learnable, on its own points
    int attributePoints = 3; // a level's three
    int specialPoints = 0;
    int strength = 10;       // base
    bool onePerAuto = false; // whether an Auto Level Up spends one level's points, not all
    bool spends = true;      // whether an Auto Level Up spends anything at all
    std::vector<int> abilities = {603, 100011};
    // The class's talent chain as offered: had, learnable, not yet.
    std::vector<std::pair<int, std::string>> chain = {{6, "has"}, {8, "learnable"}, {9, "not yet"}};
    // The specializations offered, by DAOLib's Specializations.
    struct Special
    {
        int id;
        std::string name;
        std::string state; // has, learnable, locked
    };
    std::vector<Special> specials;
    int opensWith = 0; // a specialization that, once had, makes `chainOpened` learnable
    int chainOpened = 0;
    std::vector<std::string> calls; // the level-up's steps, in order
    // The screen.
    bool opens = true;     // whether it opens: not in a fight
    bool closes = true;    // whether it closes when told
    bool takesExit = true; // whether its movie takes the exit point
    bool up = false;
    int exitPoint = 0;             // LevelUp.nExitPoint: ABORT 0 as the screen starts, COMPLETE 1
    std::vector<std::string> sent; // input's messages, in order, and the screen's own CharGenEnd
    bool ended = false;            // the level-up ended, by finish or the screen's Done

    Rogue()
    {
        AddReads(bench, [this](const Json &args) -> ToolResult { return FakeRead(args); });
        bench.Tools().Add(ToolSpec{"input", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeInput(args); });
        bench.Tools().Add(ToolSpec{"movie", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeMovie(args); });
        for (const char *step : {"levelup.offers", "levelup.take", "levelup.raise", "levelup.auto", "levelup.finish"})
        {
            bench.Tools().Add(
                ToolSpec{step, "", {}, Thread::Listener},
                [this, step = std::string(step)](const Json &args) -> ToolResult { return FakeStep(step, args); });
        }
        AddLevelUpTool(bench);
    }

    ToolResult FakeRead(const Json &args)
    {
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
        return Json{{"name", "Lyna"},
                    {"level", 2 + levels},
                    {"canLevelUp", levels > 0}, // as the game: false with only a specialization point (#64)
                    {"pointsToSpend",
                     {{"attributes", attributePoints},
                      {"skills", skillPoints},
                      {"talentsOrSpells", levels},
                      {"specializations", specialPoints}}},
                    {"attributes", {{"strength", {{"base", strength}, {"total", strength + 2}}}}}};
    }

    Json Offers() const
    {
        Json talents = Json::array();
        for (const auto &[id, state] : chain)
        {
            talents.push_back(Json{{"id", id}, {"name", "talent " + std::to_string(id)}, {"state", state}});
        }
        Json specializations = Json::array();
        for (const auto &special : specials)
        {
            const bool noPoint = special.state == "learnable" && specialPoints < 1;
            specializations.push_back(Json{
                {"id", special.id}, {"name", special.name}, {"state", noPoint ? "no point to spend" : special.state}});
        }
        return Json{{"points",
                     {{"attributes", attributePoints},
                      {"skills", skillPoints},
                      {"talentsOrSpells", levels},
                      {"specializations", specialPoints}}},
                    {"abilityPage", "talents"},
                    {"talents", talents},
                    {"spells", Json::array()},
                    {"skills", Json::array({Json{{"id", 50}, {"name", "Stealing"}, {"state", "learnable"}}})},
                    {"specializations", specializations}};
    }

    static Json Refused(const char *refusal)
    {
        return Json{{"taken", false}, {"refusal", refusal}};
    }

    // A take, as dao::levelup::Take and TakeSpecialization refuse it.
    Json TakeSpecial(int ability)
    {
        for (auto &special : specials)
        {
            if (special.id != ability)
            {
                continue;
            }
            if (special.state == "has")
            {
                return Refused("had already");
            }
            if (special.state == "locked")
            {
                return Refused("locked");
            }
            if (specialPoints < 1)
            {
                return Refused("no point to spend");
            }
            special.state = "has";
            specialPoints -= 1;
            for (auto &[id, state] : chain)
            {
                if (special.id == opensWith && id == chainOpened)
                {
                    state = "learnable"; // its own chain opens
                }
            }
            return Json{{"taken", true}};
        }
        return Refused("not offered");
    }

    Json Take(int ability)
    {
        if (ability == 50)
        {
            if (skillPoints < 1)
            {
                return Refused("no point to spend");
            }
            skillPoints -= 1;
            abilities.push_back(50);
            return Json{{"taken", true}};
        }
        for (std::size_t i = 0; i < chain.size(); ++i)
        {
            if (chain[i].first != ability)
            {
                continue;
            }
            if (chain[i].second == "has")
            {
                return Refused("had already");
            }
            if (chain[i].second == "not yet")
            {
                return Refused("locked");
            }
            if (levels < 1)
            {
                return Refused("no point to spend");
            }
            levels -= 1;
            chain[i].second = "has";
            if (i + 1 < chain.size())
            {
                chain[i + 1].second = "learnable"; // the next in the chain opens
            }
            abilities.push_back(ability);
            return Json{{"taken", true}};
        }
        return Refused("not offered");
    }

    ToolResult FakeStep(const std::string &step, const Json &args)
    {
        if (args.value("id", "") != "0x82")
        {
            throw ToolError(step + ": not the creature asked for"); // no CHECK off the test's thread
        }
        calls.push_back(step);
        if (step == "levelup.offers")
        {
            return Offers();
        }
        if (step == "levelup.take")
        {
            calls.back() += " " + args.at("ability").dump() + (args.value("specialization", false) ? " special" : "");
            const int ability = args.at("ability");
            return args.value("specialization", false) ? TakeSpecial(ability) : Take(ability);
        }
        if (step == "levelup.raise")
        {
            calls.back() += " " + args.at("attribute").dump() + " " + args.at("points").dump();
            const int points = args.at("points");
            if (points > attributePoints)
            {
                return Refused("no point to spend");
            }
            attributePoints -= points;
            strength += args.at("attribute") == 1 ? points : 0;
            return Json{{"taken", true}};
        }
        if (step == "levelup.auto")
        {
            int spend = onePerAuto ? std::min(levels, 1) : levels;
            if (!spends)
            {
                spend = 0;
            }
            for (int i = 0; i < spend; ++i)
            {
                strength += 3;
                abilities.push_back(20 + static_cast<int>(abilities.size()));
            }
            levels -= spend;
            attributePoints = 3 * levels;
            skillPoints = std::min(skillPoints, levels);
            return Json{{"done", true}};
        }
        ended = true; // levelup.finish
        return Json{{"done", true}};
    }

    ToolResult FakeInput(const Json &args)
    {
        const std::string text = args.value("message", "");
        sent.push_back(text);
        if (text.starts_with("GUI.GoToLevelUp"))
        {
            up = opens;
            exitPoint = 0; // the screen's StartScene: LEVELUP_EXIT_ABORT
        }
        else if (text == "GUI.HideFullScreenGUI" && up && closes)
        {
            // Its scene's end: CharGenEnd with the exit point, the level-up
            // ended where complete.
            sent.push_back("GUILevelUp.CharGenEnd 130 1 " + std::to_string(exitPoint));
            ended = ended || exitPoint == 1;
            up = false;
        }
        return Json{{"sent", true}};
    }

    ToolResult FakeMovie(const Json &args)
    {
        if (args.value("screen", "") != "LevelUp")
        {
            throw ToolError("movie: not the level-up screen");
        }
        if (args.contains("set"))
        {
            const bool exit = takesExit && args.at("set") == "LevelUp.nExitPoint";
            if (exit)
            {
                exitPoint = args.at("value").get<int>();
            }
            return Json{{"done", exit}};
        }
        const std::string path = args.at("get");
        if (path == "LevelUp.LEVELUP_EXIT_COMPLETE" || path == "LevelUp.LEVELUP_EXIT_ABORT")
        {
            return Json{{"type", "number"}, {"value", path.ends_with("COMPLETE") ? 1 : 0}};
        }
        return Json{{"type", "undefined"}, {"value", nullptr}};
    }

    Json LevelUp(const Json &args)
    {
        return bench.Invoke("levelup", args).data;
    }

    void Specials()
    {
        specials = {{4013, "Champion", "learnable"}, {4014, "Templar", "has"}, {4016, "Reaver", "locked"}};
        specialPoints = 1;
    }
};

} // namespace

TEST_CASE("auto spends every point by the Auto Level Up, then ends the level-up, no screen asked", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"auto", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["creature"]["id"] == "0x82"); // the one controlled, none being named
    CHECK(answer["attributes"]["strength"] == 3);
    REQUIRE(answer["gained"].size() == 1);
    CHECK(answer["gained"][0]["kind"] == "talent");
    CHECK(answer["after"]["canLevelUp"] == false);
    CHECK(rogue.calls == std::vector<std::string>{"levelup.auto", "levelup.finish"});
    CHECK(rogue.sent.empty());
    CHECK_FALSE(answer.contains("screen"));
}

TEST_CASE("two levels banked that auto spends one at a time take two rounds, each ended", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.onePerAuto = true;
    const auto answer = rogue.LevelUp(Json{{"id", "0x82"}, {"auto", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["rounds"] == 2);
    CHECK(answer["attributes"]["strength"] == 6);
    CHECK(answer["gained"].size() == 2);
    CHECK(rogue.calls == std::vector<std::string>{"levelup.auto", "levelup.finish", "levelup.auto", "levelup.finish"});
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

TEST_CASE("without auto, or with no points to spend, it is refused and nothing is asked", "[levelup]")
{
    Rogue rogue;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", false}}), ContainsSubstring("give one of"));
    rogue.levels = 0;
    rogue.skillPoints = 0;
    rogue.attributePoints = 0;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", true}}), ContainsSubstring("no level-up points"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"id", "Lyna"}, {"auto", true}}), ContainsSubstring("like \"0x82\""));
    CHECK(rogue.calls.empty());
}

TEST_CASE("chosen points named wrong are refused, saying what is wanted, and nothing is asked", "[levelup]")
{
    Rogue rogue;
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"abilities", "8"}}), ContainsSubstring("a list of ability ids"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"abilities", Json::array({"8"})}}), ContainsSubstring("a number"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"attributes", {{"luck", 1}}}}), ContainsSubstring("strength, dexterity"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"attributes", {{"strength", 0}}}}), ContainsSubstring("1 or more"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"specialization", true}}), ContainsSubstring("by name or ability id"));
    CHECK_THROWS_WITH(rogue.LevelUp(Json{{"auto", true}, {"offers", true}}), ContainsSubstring("give one of"));
    CHECK(rogue.calls.empty());
}

TEST_CASE("offers are read with no screen and nothing spent, the attributes as the sheet totals them", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    const auto answer = rogue.LevelUp(Json{{"offers", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["points"]["talentsOrSpells"] == 1);
    CHECK(answer["talents"][1] == Json{{"id", 8}, {"name", "talent 8"}, {"state", "learnable"}});
    CHECK(answer["abilityPage"] == "talents");
    CHECK(answer["attributes"]["strength"] == 12);
    REQUIRE(answer["specializations"].size() == 3);
    CHECK(answer["specializations"][2]["state"] == "locked");
    CHECK(rogue.calls == std::vector<std::string>{"levelup.offers"});
    CHECK(rogue.sent.empty());
    CHECK_FALSE(rogue.ended);
}

TEST_CASE("offers for a creature with no level-up waiting are read all the same", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 0;
    rogue.skillPoints = 0;
    rogue.attributePoints = 0;
    const auto answer = rogue.LevelUp(Json{{"offers", true}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["points"]["attributes"] == 0);
}

TEST_CASE("chosen points are spent as the screen's buttons spend them, a chain in its order", "[levelup]")
{
    Rogue rogue;
    rogue.levels = 2;
    rogue.attributePoints = 6;
    const auto answer =
        rogue.LevelUp(Json{{"abilities", Json::array({8, 9, 50})}, {"attributes", {{"strength", 2}, {"cunning", 1}}}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["taken"] == Json::array({8, 9, 50}));
    CHECK(answer["attributes"]["strength"] == 2);
    CHECK(answer["gained"].size() == 3);
    CHECK(rogue.calls == std::vector<std::string>{"levelup.offers", "levelup.take 8", "levelup.take 9",
                                                  "levelup.take 50", "levelup.raise 1 2", "levelup.raise 5 1",
                                                  "levelup.finish"});
    CHECK(answer["rounds"] == 1);
}

TEST_CASE("an ability refused first stops the levelling, nothing spent and the level-up not ended", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", Json::array({9, 8})}});
    CHECK(answer["levelled"] == false);
    const std::string reason = answer["reason"];
    CHECK_THAT(reason, ContainsSubstring("ability 9 was refused: locked"));
    CHECK_THAT(reason, ContainsSubstring("the one before it in its chain"));
    CHECK_THAT(reason, ContainsSubstring("nothing was spent"));
    CHECK(answer["offers"]["talents"] == Json::array({Json{{"id", 8}, {"name", "talent 8"}, {"state", "learnable"}}}));
    CHECK(answer["taken"].empty());
    CHECK_FALSE(rogue.ended);
}

TEST_CASE("an ability refused after others were taken keeps those, the level-up ended over them", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", Json::array({8, 9})}});
    CHECK(answer["levelled"] == false);
    const std::string reason = answer["reason"];
    CHECK_THAT(reason, ContainsSubstring("ability 9 was refused: no point to spend"));
    CHECK_THAT(reason, ContainsSubstring("are kept (`taken`)"));
    CHECK(answer["taken"] == Json::array({8}));
    CHECK(rogue.ended);
}

TEST_CASE("a raise refused is said by its attribute, and those before it kept", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"attributes", {{"cunning", 2}, {"strength", 2}}}});
    CHECK(answer["levelled"] == false);
    const std::string reason = answer["reason"];
    CHECK_THAT(reason, ContainsSubstring("strength: it was refused: no point to spend"));
    CHECK_THAT(reason, ContainsSubstring("raised before it are kept"));
    CHECK(rogue.ended);
}

TEST_CASE("a raise refused with nothing spent before it leaves the level-up as it was", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"attributes", {{"strength", 4}}}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), !ContainsSubstring("are kept"));
    CHECK_FALSE(rogue.ended);
}

TEST_CASE("a specialization is taken by name, first, so its own chain is learnable in the same call", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    rogue.opensWith = 4013;
    rogue.chainOpened = 9;
    rogue.chain[1].second = "has";
    rogue.levels = 1;
    const auto answer = rogue.LevelUp(Json{{"specialization", "champion"}, {"abilities", Json::array({9})}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["specialization"] == Json{{"id", 4013}, {"name", "Champion"}});
    CHECK(answer["taken"] == Json::array({4013, 9}));
    CHECK(rogue.calls ==
          std::vector<std::string>{"levelup.offers", "levelup.take 4013 special", "levelup.take 9", "levelup.finish"});
}

TEST_CASE("a specialization by id among the abilities is taken as one", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    const auto answer = rogue.LevelUp(Json{{"abilities", Json::array({4013})}});
    CHECK(answer["levelled"] == true);
    CHECK(rogue.calls[1] == "levelup.take 4013 special");
}

TEST_CASE("a specialization not offered is refused, naming those that are, nothing spent", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    const auto answer = rogue.LevelUp(Json{{"specialization", "Bard"}, {"attributes", {{"strength", 1}}}});
    CHECK(answer["levelled"] == false);
    const std::string reason = answer["reason"];
    CHECK_THAT(reason, ContainsSubstring("no specialization \"Bard\""));
    CHECK_THAT(reason, ContainsSubstring("Champion (learnable), Templar (has), Reaver (locked)"));
    CHECK(rogue.calls == std::vector<std::string>{"levelup.offers"});
    CHECK(rogue.strength == 10);
}

TEST_CASE("a locked specialization is refused, saying how one is unlocked", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    const auto answer = rogue.LevelUp(Json{{"specialization", 4016}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("locked (a specialization is unlocked by"));
    CHECK(rogue.specialPoints == 1);
}

TEST_CASE("a creature with only a specialization point left levels up (#64)", "[levelup]")
{
    Rogue rogue;
    rogue.Specials();
    rogue.levels = 0;
    rogue.skillPoints = 0;
    rogue.attributePoints = 0;
    const auto answer = rogue.LevelUp(Json{{"specialization", "Champion"}});
    CHECK(answer["levelled"] == true);
    CHECK(answer["reason"] == "levelled: every point spent");
}

TEST_CASE("with showMs the screen is up while the points are spent, and ends as its Done does", "[levelup]")
{
    Rogue rogue;
    const auto start = std::chrono::steady_clock::now();
    const auto answer = rogue.LevelUp(Json{{"abilities", Json::array({8})}, {"showMs", 200}});
    CHECK(answer["levelled"] == true);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(400));
    CHECK(rogue.sent ==
          std::vector<std::string>{"GUI.GoToLevelUp 130 0", "GUI.HideFullScreenGUI", "GUILevelUp.CharGenEnd 130 1 1"});
    // Its Done ended it: levelup.finish is not asked as well.
    CHECK(rogue.calls == std::vector<std::string>{"levelup.offers", "levelup.take 8"});
    CHECK_FALSE(rogue.up);
    CHECK_FALSE(answer.contains("screen"));
}

TEST_CASE("with showMs, a refusal with nothing taken closes the screen unfinished", "[levelup]")
{
    Rogue rogue;
    const auto answer = rogue.LevelUp(Json{{"abilities", Json::array({9})}, {"showMs", 1}});
    CHECK(answer["levelled"] == false);
    CHECK(rogue.sent.back() == "GUILevelUp.CharGenEnd 130 1 0");
    CHECK_FALSE(rogue.ended);
}

TEST_CASE("a screen that will not open is said, and the level-up goes on without it", "[levelup]")
{
    Rogue rogue;
    rogue.opens = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}, {"showMs", 1}});
    CHECK(answer["levelled"] == true);
    CHECK_THAT(answer["screen"].get<std::string>(), ContainsSubstring("not shown: it was not up after 5 s"));
    CHECK(rogue.calls == std::vector<std::string>{"levelup.auto", "levelup.finish"});
}

TEST_CASE("a screen that will not close says how to close it", "[levelup]")
{
    Rogue rogue;
    rogue.closes = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}, {"showMs", 1}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("GUI.HideFullScreenGUI"));
}

TEST_CASE("a screen that does not take its exit point is left up, not hidden to end aborted", "[levelup]")
{
    Rogue rogue;
    rogue.takesExit = false;
    const auto answer = rogue.LevelUp(Json{{"auto", true}, {"showMs", 1}});
    CHECK(answer["levelled"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not take LevelUp.nExitPoint"));
    CHECK(std::ranges::find(rogue.sent, "GUI.HideFullScreenGUI") == rogue.sent.end());
}
