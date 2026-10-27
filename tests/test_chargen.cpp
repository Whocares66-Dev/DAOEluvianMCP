#include "core/CharGen.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The character creation screen as DAOLib's dao::chargen answers it (#189):
// what it offers, each enabled as the screen shows it, a choice refused
// where it greys it out, the name, and the end; the points spent by
// levelup.raise and levelup.take in the creation screen's mode.
struct Creation
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    bool up = true;
    int gender = 1;
    int race = 0;
    int klass = 0;
    int background = 0;
    std::string voice;
    int attributePoints = 5;
    int abilityPoints = 2;
    std::vector<std::string> calls; // the steps, in order

    struct Background
    {
        int id;
        const char *name;
        std::vector<int> races;
        std::vector<int> classes;
    };
    const std::vector<Background> backgrounds = {{1, "Human Noble", {1, 3}, {1, 3}},
                                                 {2, "Magi", {2, 3}, {2}},
                                                 {3, "City Elf", {2}, {1, 3}},
                                                 {4, "Dwarf Commoner", {1}, {1, 3}}};

    Creation()
    {
        AddReads(bench, [this](const Json &args) -> ToolResult { return FakeRead(args); });
        bench.Tools().Add(ToolSpec{"script", "", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { return Json{{"value", "pre100ar_start"}}; });
        for (const char *step : {"chargen.offers", "chargen.select", "chargen.name", "chargen.finish", "levelup.offers",
                                 "levelup.raise", "levelup.take"})
        {
            bench.Tools().Add(
                ToolSpec{step, "", {}, Thread::Listener},
                [this, step = std::string(step)](const Json &args) -> ToolResult { return Step(step, args); });
        }
        AddCharGenTool(bench);
    }

    ToolResult FakeRead(const Json &args)
    {
        if (args.value("kind", "") == "gui")
        {
            return Json{{"mode", up ? "chargen" : "explore"},
                        {"screens", up ? Json::array({Json{{"name", "CharGen"}}}) : Json::array()}};
        }
        // character, of the hero.
        return Json{{"name", "Hero"},
                    {"class", {{"id", klass}, {"label", klass == 2 ? "Wizard" : "Warrior"}}},
                    {"race", {{"label", "Human"}}},
                    {"pointsToSpend",
                     {{"attributes", attributePoints},
                      {"skills", abilityPoints / 2},
                      {"talentsOrSpells", abilityPoints - (abilityPoints / 2)}}},
                    {"attributes", {{"strength", {{"total", 10}}}}}};
    }

    static Json Choice(int id, const char *name, bool enabled, bool chosen)
    {
        return Json{{"id", id}, {"name", name}, {"enabled", enabled}, {"chosen", chosen}};
    }

    bool Fits(const Background &each) const
    {
        const auto has = [](const std::vector<int> &list, int id) { return std::ranges::find(list, id) != list.end(); };
        return race != 0 && klass != 0 && has(each.races, race) && has(each.classes, klass);
    }

    Json Offers() const
    {
        Json backs = Json::array();
        for (const auto &each : backgrounds)
        {
            backs.push_back(Choice(each.id, each.name, Fits(each), background == each.id));
        }
        const std::string sex = gender == 1 ? "m" : "f";
        constexpr std::array<const char *, 4> kKinds = {"human", "dwarf", "elf", "human"}; // by race, 0 none
        const std::string kind = kKinds.at(static_cast<std::size_t>(race));
        return Json{
            {"hero", "0x82"},
            {"genders", Json::array({Choice(1, "Male", true, gender == 1), Choice(2, "Female", true, gender == 2)})},
            {"races", Json::array({Choice(1, "Dwarf", true, race == 1), Choice(2, "Elf", true, race == 2),
                                   Choice(3, "Human", true, race == 3)})},
            {"classes",
             Json::array({Choice(1, "Warrior", race != 0, klass == 1), Choice(2, "Mage", race != 0, klass == 2),
                          Choice(3, "Rogue", race != 0, klass == 3)})},
            {"backgrounds", backs},
            {"voices", Json::array({Json{{"name", "Wise"}, {"soundSet", "ss_pc_" + sex + kind + "_wise"}},
                                    Json{{"name", "Brash"}, {"soundSet", "ss_pc_" + sex + kind + "_cocky"}}})}};
    }

    static Json Refused(const char *refusal)
    {
        return Json{{"taken", false}, {"refusal", refusal}};
    }

    Json Select(const Json &args)
    {
        const std::string choice = args.value("choice", "");
        if (choice == "voice")
        {
            voice = args.value("soundSet", "");
            calls.push_back("voice " + voice);
            return Json{{"taken", true}};
        }
        const int id = args.value("id", 0);
        calls.push_back(choice + " " + std::to_string(id));
        if (choice == "gender")
        {
            gender = id;
        }
        else if (choice == "race")
        {
            race = id;
            klass = 0;
            background = 0;
        }
        else if (choice == "class")
        {
            if (race == 0)
            {
                return Refused("disabled");
            }
            klass = id;
            background = 0;
        }
        else
        {
            const auto it = std::ranges::find(backgrounds, id, &Background::id);
            if (it == backgrounds.end() || !Fits(*it))
            {
                return Refused("disabled");
            }
            background = id;
        }
        return Json{{"taken", true}};
    }

    ToolResult Step(const std::string &step, const Json &args)
    {
        if (step == "chargen.offers")
        {
            if (!up)
            {
                throw ToolError("chargen.offers: the character creation screen is not up");
            }
            return Offers();
        }
        if (step == "chargen.select")
        {
            return Select(args);
        }
        if (step == "chargen.name")
        {
            const std::string name = args.value("name", "");
            calls.push_back("name " + name);
            return name.empty() || name.size() > 10 ? Refused("bad name") : Json{{"taken", true}};
        }
        if (step == "chargen.finish")
        {
            calls.push_back("finish " + args.value("exit", "") +
                            (args.contains("difficulty") ? " " + args.at("difficulty").dump() : ""));
            if (args.value("exit", "") == "full" && (attributePoints > 0 || abilityPoints > 0))
            {
                return Refused("points left");
            }
            up = false;
            return Json{{"taken", true}};
        }
        if (step == "levelup.offers")
        {
            return Json{{"skills", Json::array({Json{{"id", 100}, {"state", "learnable"}}})},
                        {klass == 2 ? "spells" : "talents", Json::array({Json{{"id", 200}, {"state", "learnable"}},
                                                                         Json{{"id", 201}, {"state", "not yet"}}})}};
        }
        if (step == "levelup.raise")
        {
            calls.push_back("raise " + args.at("attribute").dump() + " " + args.at("points").dump());
            attributePoints -= args.value("points", 0);
            return Json{{"taken", true}};
        }
        calls.push_back("take " + args.at("ability").dump() + (args.value("chargen", false) ? " chargen" : ""));
        if (args.value("ability", 0) == 201)
        {
            return Refused("locked");
        }
        abilityPoints -= 1;
        return Json{{"taken", true}};
    }

    Json Make(const Json &args)
    {
        return bench.Invoke("chargen", args).data;
    }
};

} // namespace

TEST_CASE("chargen makes each choice as the screen does and ends it with its Quick Play", "[chargen]")
{
    Creation screen;
    const auto answer = screen.Make(Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}});
    CHECK(screen.calls == std::vector<std::string>{"gender 1", "race 3", "class 1", "background 1",
                                                   "voice ss_pc_mhuman_wise", "finish quick"});
    CHECK(answer["chose"] == Json{{"gender", "male"},
                                  {"race", "human"},
                                  {"class", "warrior"},
                                  {"background", "human noble"},
                                  {"voice", "ss_pc_mhuman_wise"}});
    CHECK(answer["exit"] == "quick play");
    CHECK(answer["finished"] == true);
    CHECK(answer["origin"] == Json{{"row", 3101}, {"startArea", "pre100ar_start"}});
    CHECK(answer["hero"] == "0x82");
}

TEST_CASE("chargen spends every point as the screen's pages do, then ends it full", "[chargen]")
{
    Creation screen;
    const auto answer = screen.Make(Json{{"race", 3},
                                         {"class", "wizard"},
                                         {"background", "magi"},
                                         {"attributes", {{"magic", 5}}},
                                         {"abilities", Json::array({100, 200})}});
    CHECK(screen.calls == std::vector<std::string>{"gender 1", "race 3", "class 2", "background 2",
                                                   "voice ss_pc_mhuman_wise", "raise 4 5", "take 100 chargen",
                                                   "take 200 chargen", "finish full"});
    CHECK(answer["exit"] == "full");
    CHECK(answer["taken"] == Json::array({100, 200}));
    CHECK(answer["chose"]["class"] == "mage");
}

TEST_CASE("chargen spends every point or none, saying what the hero has", "[chargen]")
{
    Creation screen;
    CHECK_THROWS_WITH(screen.Make(Json{{"race", "human"},
                                       {"class", "warrior"},
                                       {"background", "noble"},
                                       {"attributes", {{"strength", 3}}},
                                       {"abilities", Json::array({100, 200})}}),
                      ContainsSubstring("5 attribute points and 2 for skills"));
    CHECK(std::ranges::find(screen.calls, "finish full") == screen.calls.end());
    CHECK(std::ranges::find(screen.calls, "raise 1 3") == screen.calls.end()); // nothing spent
}

TEST_CASE("chargen is refused a background the race and class cannot take, naming those they can", "[chargen]")
{
    Creation screen;
    CHECK_THROWS_WITH(
        screen.Make(Json{{"race", "elf"}, {"class", "rogue"}, {"background", "human noble"}}),
        ContainsSubstring("offers no background \"human noble\" now; it offers city elf; those chosen before it"));
    CHECK(screen.race == 2);
}

TEST_CASE("chargen is refused a race the screen does not offer, nothing chosen past it", "[chargen]")
{
    Creation screen;
    CHECK_THROWS_WITH(screen.Make(Json{{"race", "qunari"}, {"class", "warrior"}, {"background", "noble"}}),
                      ContainsSubstring("it offers dwarf, elf, human"));
    CHECK(screen.calls == std::vector<std::string>{"gender 1"});
}

TEST_CASE("chargen takes a voice by its last word for the hero's race and gender", "[chargen]")
{
    Creation screen;
    const auto answer = screen.Make(
        Json{{"gender", "female"}, {"race", "elf"}, {"class", "rogue"}, {"background", "city"}, {"voice", "cocky"}});
    CHECK(answer["chose"]["voice"] == "ss_pc_felf_cocky");
    Creation other;
    CHECK_THROWS_WITH(
        other.Make(Json{{"race", "elf"}, {"class", "rogue"}, {"background", "city"}, {"voice", "sultry"}}),
        ContainsSubstring("the screen offers wise, cocky"));
}

TEST_CASE("chargen names the hero, and says a name the field does not take", "[chargen]")
{
    Creation screen;
    const auto answer =
        screen.Make(Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}, {"name", "Tabris"}});
    CHECK(answer["chose"]["name"] == "Tabris");
    CHECK(std::ranges::find(screen.calls, "name Tabris") != screen.calls.end());
    Creation other;
    CHECK_THROWS_WITH(
        other.Make(
            Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}, {"name", "Bartholomew the Third"}}),
        ContainsSubstring("1 to 10 letters"));
}

TEST_CASE("chargen gives the difficulty to the end, and refuses one not offered before choosing", "[chargen]")
{
    Creation screen;
    screen.Make(Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}, {"difficulty", "Hard"}});
    CHECK(screen.calls.back() == "finish quick 2");
    Creation other;
    CHECK_THROWS_WITH(
        other.Make(Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}, {"difficulty", "easy"}}),
        ContainsSubstring("casual, normal, hard or nightmare"));
    CHECK(other.calls.empty());
}

TEST_CASE("chargen with finish false leaves the screen up with the choices made", "[chargen]")
{
    Creation screen;
    const auto answer =
        screen.Make(Json{{"race", "human"}, {"class", "warrior"}, {"background", "noble"}, {"finish", false}});
    CHECK(answer["finished"] == false);
    CHECK(screen.up);
    CHECK(screen.calls.back() == "voice ss_pc_mhuman_wise");
}

TEST_CASE("chargen's offers list what the screen offers, and the abilities once a class is chosen", "[chargen]")
{
    Creation screen;
    auto answer = screen.Make(Json{{"offers", true}});
    CHECK(answer["races"].size() == 3);
    CHECK(answer["classes"][0]["enabled"] == false);
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("no class yet"));
    screen.race = 3;
    screen.klass = 2;
    answer = screen.Make(Json{{"offers", true}});
    CHECK(answer["skills"].size() == 1);
    CHECK(answer["spells"] == Json::array({Json{{"id", 200}, {"state", "learnable"}}}));
    CHECK_FALSE(answer.contains("talents"));
    CHECK(screen.calls.empty());
    CHECK_THROWS_WITH(screen.Make(Json{{"offers", true}, {"race", "elf"}}), ContainsSubstring("takes no choices"));
}

TEST_CASE("chargen says how to bring the screen up when it is not", "[chargen]")
{
    Creation screen;
    screen.up = false;
    CHECK_THROWS_WITH(screen.Make(Json{{"race", "human"}}), ContainsSubstring("game_new"));
    CHECK_THROWS_WITH(Creation().Make(Json{{"race", "human"}}), ContainsSubstring("give `race`, `class`"));
}
