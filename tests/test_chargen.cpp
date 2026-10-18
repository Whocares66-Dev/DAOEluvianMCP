#include "core/CharGen.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The creation screen as the game answered it on 2026-10-18 (#153): the
// hero 130, the movie's background box up, the screen's messages taken by
// input as sys_chargen takes their events, the game's tables by script.
struct Creation
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string mode = "chargen";
    bool up = true;
    bool box = true;
    int race = 3;
    int klass = 0;
    int background = 0;
    int gender = 1;
    std::map<int, int> attributes; // by PROPERTY_ATTRIBUTE_*
    int attributePoints = 0;
    int skillPoints = 0;
    int talentPoints = 0;
    std::set<int> held;
    std::string name = "Jaden";
    std::map<std::string, Json> exitState; // the movie's CharGen.* as set
    bool crashed = false;                  // a hide with the movie's exit unset
    std::vector<std::string> sent;         // input's messages, in order
    std::vector<std::string> order;        // input's messages and the movie's sets, in order

    // Each chain: an ability, and whether it waits on another held.
    struct Chain
    {
        int id;
        std::string name;
        int after; // held first, or 0
        bool skill;
    };
    std::vector<Chain> chains = {{100061, "HERBALISM", 0, true},     {100100, "COMBAT_TRAINING", 0, true},
                                 {200254, "ARCANE_BOLT", 0, false},  {10104, "HEAL", 0, false},
                                 {13001, "WINTERS_GRASP", 0, false}, {11001, "GLYPH_OF_PARALYSIS", 10104, false}};

    Creation()
    {
        Add("gui", [this](const Json &) {
            Json screens = Json::array({Json{{"name", "Tooltips"}}, Json{{"name", "PopupLayer"}}});
            if (up)
            {
                screens.push_back(Json{{"name", "CharGen"}});
            }
            return Json{{"mode", mode}, {"screens", screens}};
        });
        Add("data", [this](const Json &args) { return Data(args); });
        Add("input", [this](const Json &args) { return Input(args.value("message", "")); });
        Add("movie", [this](const Json &args) { return Movie(args); });
        Add("character", [this](const Json &) { return Character(); });
        Add("script", [](const Json &args) { return Json{{"value", Script(args)}}; });
    }

    void Add(const char *tool, const std::function<Json(const Json &)> &answer)
    {
        bench.Tools().Add(ToolSpec{tool, "", {}, Thread::Listener},
                          [answer](const Json &args) -> ToolResult { return answer(args); });
    }

    int State(const Chain &chain) const
    {
        if (held.contains(chain.id))
        {
            return 1;
        }
        return chain.after == 0 || held.contains(chain.after) ? 2 : 3;
    }

    std::vector<const Chain *> ChainsOf(const std::string &list) const
    {
        std::vector<const Chain *> out;
        for (const auto &chain : chains)
        {
            if (chain.skill == (list == "SkillChains") && (list != "TalentChains" || klass != 2) &&
                (list != "SpellChains" || klass == 2))
            {
                out.push_back(&chain);
            }
        }
        return out;
    }

    Json Read(const std::string &path) const
    {
        const auto number = [&](double value) {
            return Json{{"path", path}, {"found", true}, {"number", value}, {"text", std::format("{}", value)}};
        };
        if (path == "CharGenCreature.ID")
        {
            return number(130);
        }
        if (path == "GUIPopupLayer.DialogBoxText")
        {
            return Json{{"path", path}, {"found", true}, {"text", "The background you select..."}};
        }
        // CharGenCreature.Stats.<list>.<i>.<field>
        std::istringstream in(path);
        std::vector<std::string> parts;
        for (std::string part; std::getline(in, part, '.');)
        {
            parts.push_back(part);
        }
        if (parts.size() >= 5 && parts[1] == "Stats")
        {
            const auto list = ChainsOf(parts[2]);
            const auto i = static_cast<std::size_t>(std::stoi(parts[3]));
            if (i < list.size())
            {
                const Chain &chain = *list[i];
                if (parts[4] == "DisplayState")
                {
                    return number(State(chain));
                }
                if (parts.size() == 6 && parts[5] == "ID")
                {
                    return number(chain.id);
                }
                return Json{{"path", path}, {"found", true}, {"text", chain.name}};
            }
        }
        return Json{{"path", path}, {"found", false}};
    }

    Json Data(const Json &args) const
    {
        if (args.contains("paths"))
        {
            Json values = Json::array();
            for (const auto &path : args.at("paths"))
            {
                values.push_back(Read(path.get<std::string>()));
            }
            return Json{{"values", values}};
        }
        const std::string path = args.value("path", "");
        if (args.value("members", false))
        {
            const auto dot = path.rfind('.');
            return Json{{"path", path}, {"found", true}, {"count", ChainsOf(path.substr(dot + 1)).size()}};
        }
        return Read(path);
    }

    Json Input(const std::string &text)
    {
        sent.push_back(text);
        order.push_back(text);
        std::istringstream in(text);
        std::string message;
        in >> message;
        std::vector<int> n;
        for (std::string word; in >> word;)
        {
            n.push_back(static_cast<int>(std::strtol(word.c_str(), nullptr, 10)));
        }
        if (message == "GUICharGen.CharGenStart")
        {
            for (int a = 1; a <= 6; ++a)
            {
                attributes[a] = 10;
            }
            attributePoints = 5;
            skillPoints = 1;
            talentPoints = 2;
            klass = 0;
            held.clear();
        }
        else if (message == "GUICharGen.CharGenSelectGender")
        {
            gender = n.at(2);
        }
        else if (message == "GUICharGen.CharGenSelectRace")
        {
            race = n.at(2);
        }
        else if (message == "GUICharGen.CharGenSelectClass")
        {
            klass = n.at(2);
            held.insert(klass == 2 ? 200254 : 0);
            held.insert(100061);
        }
        else if (message == "GUICharGen.CharGenSelectBackground")
        {
            background = n.at(2);
        }
        else if (message == "GUICharGen.CharGenAssignAttribute")
        {
            attributes[n.at(1)] += n.at(2);
            attributePoints -= n.at(2);
        }
        else if (message == "GUICharGen.CharGenAssignAbility")
        {
            held.insert(n.at(1));
            const bool skill = std::ranges::any_of(chains, [&](const Chain &c) { return c.id == n.at(1) && c.skill; });
            (skill ? skillPoints : talentPoints) -= 1;
        }
        else if (message == "GUI.HideFullScreenGUI")
        {
            if (exitState.contains("CharGen.bUserExit") && exitState.at("CharGen.bUserExit") == true)
            {
                sent.push_back(std::format("GUICharGen.CharGenEnd {} 0 {}", exitState["CharGen.CreatureID"].dump(),
                                           exitState["CharGen.nExitPoint"].dump()));
                mode = "cutscene";
            }
            else
            {
                crashed = true;
            }
            up = false;
        }
        return Json{{"message", text}, {"sent", true}};
    }

    Json Movie(const Json &args)
    {
        if (args.contains("get"))
        {
            return box ? Json{{"type", "string"}, {"value", "_level0.null"}} : Json{{"type", "null"}};
        }
        if (args.contains("set"))
        {
            const std::string path = args.at("set");
            exitState[path] = args.at("value");
            order.push_back("set " + path);
            return Json{{"done", true}, {"existed", path.starts_with("CharGen.")}};
        }
        const std::string call = args.value("call", "");
        if (call == "PopupLayer.HideAllPopups")
        {
            box = false;
        }
        if (call == "ExternalCommands.SetValueUnicode")
        {
            name = args.at("args").at(1).get<std::string>();
        }
        return Json{{"ran", true}};
    }

    Json Character() const
    {
        static const char *kNames[] = {"", "strength", "dexterity", "willpower", "magic", "cunning", "constitution"};
        Json attrs = Json::object();
        for (int a = 1; a <= 6; ++a)
        {
            attrs[kNames[a]] = Json{{"total", attributes.contains(a) ? attributes.at(a) : 0}};
        }
        return Json{{"name", name},
                    {"gender", gender == 1 ? "male" : "female"},
                    {"race", {{"id", race}, {"label", race == 3 ? "Human" : "Elf"}}},
                    {"class", {{"id", klass}, {"label", klass == 2 ? "Wizard" : "Warrior"}}},
                    {"level", 1},
                    {"attributes", attrs},
                    {"pointsToSpend",
                     {{"attributes", attributePoints}, {"skills", skillPoints}, {"talentsOrSpells", talentPoints}}},
                    {"abilityCount", held.size()}};
    }

    // The game's tables as shipped, the rows a test needs.
    static Json Script(const Json &args)
    {
        const std::string command = args.at("command");
        const Json &a = args.at("args");
        struct Row
        {
            const char *label;
            int elf, human, dwarf, warrior, rogue, mage;
        };
        static const Row kBackgrounds[] = {{"Invalid", 0, 0, 0, 0, 0, 0},  {"Dalish", 1, 0, 0, 1, 1, 0},
                                           {"Commoner", 0, 0, 1, 1, 1, 0}, {"City", 1, 0, 0, 1, 1, 0},
                                           {"Magi", 1, 1, 0, 0, 0, 1},     {"Noble", 0, 1, 1, 1, 1, 0}};
        static const char *kVoices[] = {"", "ss_pc_fhuman_wise", "ss_pc_mhuman_wise", "ss_pc_mhuman_cocky",
                                        "ss_pc_melf_wise"};
        const int table = a.at(0);
        if (command == "GetM2DARows")
        {
            return table == 97 ? 6 : 5;
        }
        if (command == "GetM2DARowIdFromRowIndex")
        {
            return a.at(1);
        }
        if (command == "GetStringByStringId")
        {
            return a.at(0) == 245276 ? "Daylen" : "";
        }
        const std::string column = a.at(1);
        const int row = a.at(2);
        if (table == 97)
        {
            const Row &r = kBackgrounds[row];
            if (column == "Label")
            {
                return r.label;
            }
            const std::map<std::string, int> cols = {{"Elf", r.elf},         {"Human", r.human}, {"Dwarf", r.dwarf},
                                                     {"Warrior", r.warrior}, {"Rogue", r.rogue}, {"Mage", r.mage}};
            return cols.at(column);
        }
        if (table == 212)
        {
            return kVoices[row];
        }
        if (table == 1001 && row == 3204)
        {
            if (column == "StartArea")
            {
                return "bhm500ar_tower_harrowing";
            }
            return column == "DefaultNameMale" ? 245276 : 245277;
        }
        return command == "GetM2DAString" ? Json("") : Json(0);
    }

    Json Call(const Json &args)
    {
        return bench.Invoke("chargen", args).data;
    }

    std::string Refusal(const Json &args)
    {
        try
        {
            bench.Invoke("chargen", args);
        }
        catch (const ToolError &e)
        {
            return e.what();
        }
        return "";
    }

    bool Sent(const std::string &text) const
    {
        return std::ranges::find(sent, text) != sent.end();
    }
};

const Json kMage = {{"race", "human"},
                    {"class", "mage"},
                    {"background", "magi"},
                    {"attributes", {{"magic", 5}}},
                    {"abilities", {100100, 10104, 13001}},
                    {"name", "Elora"}};

} // namespace

TEST_CASE("chargen builds a mage as the screen does and ends it as the movie does", "[chargen]")
{
    Creation game;
    AddCharGenTool(game.bench);
    const Json answer = game.Call(kMage);

    // The movie's box closed, then the hero started again with its id: the
    // movie's own start carried creature 0.
    CHECK_FALSE(game.box);
    CHECK(answer["closedBox"] == "The background you select...");
    const std::vector<std::string> first(game.sent.begin(), game.sent.begin() + 6);
    CHECK(first == std::vector<std::string>{"GUICharGen.CharGenStart 130 0", "GUICharGen.CharGenSelectGender 130 0 1 1",
                                            "GUICharGen.CharGenSelectRace 130 0 3 1",
                                            "GUICharGen.CharGenSelectClass 130 0 2 1",
                                            "GUICharGen.CharGenSelectBackground 130 0 4 1",
                                            "GUICharGen.CharGenSelectAppearanceAndSoundSet 130 0 ss_pc_mhuman_wise"});
    CHECK(std::ranges::count(game.sent, std::string("GUICharGen.CharGenAssignAttribute 130 4 1")) == 5);
    CHECK(game.Sent("GUICharGen.CharGenAssignAbility 130 13001 0"));
    CHECK(game.name == "Elora");
    CHECK(game.Sent("GUICharGen.CharGenSelectName 130"));

    // The exit state set before the hide, so the movie's FinishScene sends
    // the end as the screen closes; no crash.
    const auto hide = std::ranges::find(game.order, std::string("GUI.HideFullScreenGUI"));
    REQUIRE(hide != game.order.end());
    CHECK(std::find(game.order.begin(), hide, std::string("set CharGen.bUserExit")) != hide);
    CHECK_FALSE(game.crashed);
    CHECK(game.Sent("GUICharGen.CharGenEnd 130 0 2"));
    CHECK(answer["finished"] == true);
    CHECK(answer["exit"] == "full");
    CHECK(answer["origin"]["row"] == 3204);
    CHECK(answer["origin"]["startArea"] == "bhm500ar_tower_harrowing");
    CHECK(answer["after"]["attributes"]["magic"] == 15);
    CHECK(answer["after"]["pointsToSpend"]["talentsOrSpells"] == 0);
    CHECK(answer["chose"]["name"] == "Elora");
    CHECK(answer["hero"] == "0x82");
}

TEST_CASE("chargen with no points given is the screen's Quick Play, named as the origin names the hero", "[chargen]")
{
    Creation game;
    AddCharGenTool(game.bench);
    const Json answer = game.Call({{"race", "human"}, {"class", "mage"}, {"background", "Magi"}});
    CHECK(answer["exit"] == "quick play");
    CHECK(game.exitState["CharGen.nExitPoint"] == 1);
    CHECK(game.Sent("GUICharGen.CharGenEnd 130 0 1"));
    CHECK(std::ranges::none_of(game.sent, [](const std::string &s) { return s.find("Assign") != std::string::npos; }));
    CHECK(game.name == "Daylen");
}

TEST_CASE("chargen refuses before sending what the screen would not allow", "[chargen]")
{
    Creation game;
    AddCharGenTool(game.bench);
    // A background the race and class may not take, as its button is off.
    CHECK_THAT(game.Refusal({{"race", "human"}, {"class", "warrior"}, {"background", "magi"}}),
               ContainsSubstring("may take the background noble"));
    CHECK_THAT(game.Refusal({{"race", "elf"}, {"class", "warrior"}, {"background", "noble"}}),
               ContainsSubstring("an elf warrior may take the backgrounds dalish, city"));
    CHECK_THAT(game.Refusal({{"race", "orc"}, {"class", "warrior"}, {"background", "noble"}}),
               ContainsSubstring("human"));
    CHECK_THAT(game.Refusal({{"race", "human"}, {"class", "mage"}, {"background", "magi"}, {"voice", "sultry"}}),
               ContainsSubstring("a male human's are wise, cocky"));
    CHECK_THAT(game.Refusal({{"race", "human"}, {"class", "mage"}}), ContainsSubstring("`background`"));
    CHECK(game.sent.empty());
    CHECK(game.box);

    game.up = false;
    game.mode = "explore";
    CHECK_THAT(game.Refusal(kMage), ContainsSubstring("game_new"));
}

TEST_CASE("chargen spends every point or none, and only what is learnable", "[chargen]")
{
    Creation game;
    AddCharGenTool(game.bench);
    Json some = kMage;
    some["attributes"] = {{"magic", 3}};
    CHECK_THAT(game.Refusal(some), ContainsSubstring("5 attribute points"));
    CHECK_FALSE(game.Sent("GUI.HideFullScreenGUI"));
    CHECK_FALSE(game.Sent("GUICharGen.CharGenAssignAttribute 130 4 1"));

    // Glyph of Paralysis waits on Heal: the screen shows it locked.
    Json locked = kMage;
    locked["abilities"] = {100100, 11001, 13001};
    game.sent.clear();
    CHECK_THAT(game.Refusal(locked),
               ContainsSubstring("ability 11001 is not learnable now; learnable: 10104 heal, 13001 winters_grasp"));
    CHECK_FALSE(game.Sent("GUI.HideFullScreenGUI"));
    // In order, so Heal first opens the Glyph.
    locked["abilities"] = {100100, 10104, 11001};
    CHECK(game.Call(locked)["finished"] == true);
}

TEST_CASE("chargen with finish false leaves the screen up, and offers lists what it may take", "[chargen]")
{
    Creation game;
    AddCharGenTool(game.bench);
    Json offers = game.Call({{"offers", true}});
    REQUIRE(offers["backgrounds"].size() == 5);
    CHECK(offers["backgrounds"][3] ==
          Json{{"id", 4}, {"name", "magi"}, {"races", {"elf", "human"}}, {"classes", {"mage"}}});
    CHECK_FALSE(offers.contains("spells"));
    CHECK(game.sent.empty());

    Json chosen = {{"race", "human"}, {"class", "mage"}, {"background", "magi"}, {"finish", false}};
    const Json answer = game.Call(chosen);
    CHECK(answer["finished"] == false);
    CHECK(game.up);
    CHECK_FALSE(game.Sent("GUI.HideFullScreenGUI"));

    offers = game.Call({{"offers", true}});
    CHECK(offers["pointsToSpend"]["attributes"] == 5);
    CHECK(offers["skills"] == Json::array({Json{{"id", 100061}, {"name", "herbalism"}, {"state", "has"}},
                                           Json{{"id", 100100}, {"name", "combat_training"}, {"state", "learnable"}}}));
    REQUIRE(offers["spells"].size() == 3); // the Glyph locked behind Heal is left out
    CHECK(offers["spells"][0]["state"] == "has");
}
