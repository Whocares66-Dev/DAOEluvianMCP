#include "core/Craft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A poisoner and the crafting screen as the game answers them: the script
// opens the screen and says who has which skill, the data tree lists the
// recipes, input's messages craft and close it, inspect reads the pack, and
// order hands control over.
struct Poisoner
{
    struct Ingredient
    {
        std::string name;
        int need;
    };
    struct Recipe
    {
        int id;
        std::string name;
        std::string product;
        std::string ref;
        int skill;
        std::vector<Ingredient> ingredients;
    };

    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string controlled = "0x82";
    std::set<std::string> party = {"0x82", "0x1b88"};
    std::map<std::string, std::set<int>> skills = {{"0x82", {100071, 100072}}, {"0x1b88", {}}};
    bool opens = true;                                 // whether the screen opens: not in a fight
    bool crafts = true;                                // whether a craft brings the product into the pack
    int lag = 0;                                       // inventory reads before a made item shows in the pack
    std::vector<std::pair<int, std::string>> arriving; // made items on their way: reads left, template
    int selected = -1;                                 // the screen's selected character, if not the one controlled
    bool up = false;                                   // the screen
    std::map<std::string, int> held = {{"Deathroot", 3}, {"Flask", 3}, {"Lyrium Dust", 0}};
    std::map<std::string, int> pack; // products by template, as the pack holds them
    std::vector<Recipe> recipes = {
        // A name as the game gave it in play: "?" before it (2026-10-05).
        {7, "?Deathroot Toxin", "Deathroot Toxin", "gen_im_cft_poi_101.uti", 100071, {{"Deathroot", 1}, {"Flask", 1}}},
        {9, "Magebane", "Magebane", "gen_im_cft_poi_201.uti", 100072, {{"Lyrium Dust", 1}, {"Flask", 1}}},
    };
    std::vector<std::string> sent;    // input's messages, in order
    std::vector<std::string> ordered; // who order control was asked for
    std::vector<Json> scripts;        // script's calls, in order

    static int Number(const std::string &id)
    {
        return std::stoi(id.substr(2), nullptr, 16);
    }

    bool CanMake(const Recipe &recipe) const
    {
        for (const auto &ingredient : recipe.ingredients)
        {
            if (held.at(ingredient.name) < ingredient.need)
            {
                return false;
            }
        }
        return true;
    }

    // A path's value in the tree, as inspect kind=data reads it.
    Json Value(const std::string &path) const
    {
        Json value = {{"path", path}, {"found", true}};
        if (path == "GUI.SelectedChar.ID")
        {
            value["number"] = selected >= 0 ? selected : Number(controlled);
            return value;
        }
        const std::string prefix = "GUICrafting.Recipes.";
        const auto dot = path.find('.', prefix.size());
        const auto &recipe = recipes.at(std::stoul(path.substr(prefix.size(), dot - prefix.size())));
        const std::string rest = path.substr(dot + 1);
        if (rest.starts_with("Ingredients."))
        {
            const auto at = rest.find('.', 12);
            const auto &ingredient = recipe.ingredients.at(std::stoul(rest.substr(12, at - 12)));
            const std::string field = rest.substr(at + 1);
            if (field == "Name")
            {
                value["text"] = ingredient.name;
            }
            else
            {
                value["number"] = field == "Need" ? ingredient.need : held.at(ingredient.name);
            }
            return value;
        }
        if (rest == "ID" || rest == "Skill" || rest == "HaveAllIngredients")
        {
            value["number"] = rest == "ID" ? recipe.id : rest == "Skill" ? recipe.skill : CanMake(recipe) ? 1 : 0;
        }
        else
        {
            value["text"] = rest == "Name" ? recipe.name : rest == "ProductName" ? recipe.product : recipe.ref;
        }
        return value;
    }

    Json Data(const Json &args) const
    {
        if (args.value("members", false))
        {
            const std::string path = args.at("path");
            Json members = Json::array();
            if (path == "GUICrafting.Recipes")
            {
                for (std::size_t i = 0; i < recipes.size(); ++i)
                {
                    members.push_back(path + "." + std::to_string(i));
                }
            }
            else
            {
                const auto i = std::stoul(path.substr(20, path.find('.', 20) - 20));
                for (std::size_t k = 0; k < recipes.at(i).ingredients.size(); ++k)
                {
                    members.push_back(std::to_string(k)); // a bare name: craft takes either
                }
            }
            return Json{{"path", path}, {"found", true}, {"count", members.size()}, {"members", members}};
        }
        if (args.contains("paths"))
        {
            if (args.at("paths").size() > 64)
            {
                throw ToolError("inspect: `paths` is a list of up to 64 dotted paths");
            }
            Json values = Json::array();
            for (const auto &path : args.at("paths"))
            {
                values.push_back(Value(path.get<std::string>()));
            }
            return Json{{"values", values}};
        }
        return Value(args.at("path"));
    }

    Poisoner()
    {
        bench.Tools().Add(ToolSpec{"inspect", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string kind = args.value("kind", "");
            if (kind == "game")
            {
                return Json{{"controlled", {{"id", controlled}}}};
            }
            if (kind == "character")
            {
                return Json{{"name", args.at("id") == "0x82" ? "Lyna" : "Zevran"}};
            }
            if (kind == "gui")
            {
                return Json{{"fullScreen", up ? Json("Crafting") : Json(nullptr)}};
            }
            if (kind == "data")
            {
                return Data(args);
            }
            for (auto it = arriving.begin(); it != arriving.end();)
            {
                if (--it->first > 0)
                {
                    ++it;
                    continue;
                }
                pack[it->second] += 1;
                it = arriving.erase(it);
            }
            Json items = Json::array({Json{{"template", "gen_im_wep_mel_dag_01"}, {"stack", 1}}});
            for (const auto &[ref, count] : pack)
            {
                items.push_back(Json{{"template", ref}, {"stack", count}});
            }
            return Json{{"items", items}};
        });
        bench.Tools().Add(ToolSpec{"script", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            scripts.push_back(args);
            const std::string command = args.at("command");
            if (command == "HasAbility")
            {
                const std::string who = args.at("args").at(0);
                return Json{{"value", skills[who].contains(args.at("args").at(1).get<int>()) ? 1 : 0}};
            }
            up = up || opens; // ShowCraftingGUI
            return Json{{"value", nullptr}};
        });
        bench.Tools().Add(ToolSpec{"input", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string text = args.at("message");
            sent.push_back(text);
            if (text == "GUI.HideFullScreenGUI")
            {
                up = false;
            }
            else if (text.starts_with("GUICrafting.CraftItem ") && crafts)
            {
                const int id = std::stoi(text.substr(22));
                for (const auto &recipe : recipes)
                {
                    if (recipe.id == id && CanMake(recipe))
                    {
                        for (const auto &ingredient : recipe.ingredients)
                        {
                            held[ingredient.name] -= ingredient.need;
                        }
                        const std::string ref = recipe.ref.substr(0, recipe.ref.size() - 4);
                        if (lag > 0)
                        {
                            arriving.emplace_back(lag, ref);
                        }
                        else
                        {
                            pack[ref] += 1;
                        }
                    }
                }
            }
            return Json{{"sent", true}};
        });
        bench.Tools().Add(ToolSpec{"order", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            const std::string id = args.at("id");
            ordered.push_back(id);
            if (party.contains(id))
            {
                controlled = id;
            }
            return Json{{"taken", controlled == id}};
        });
    }

    Json Craft(Json args)
    {
        args["showMs"] = 0;
        return bench.Invoke("craft", args).data;
    }

    int Crafted() const
    {
        int count = 0;
        for (const auto &text : sent)
        {
            count += text.starts_with("GUICrafting.CraftItem ") ? 1 : 0;
        }
        return count;
    }
};

} // namespace

TEST_CASE("craft lists the recipes the screen offers, nothing made", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}});
    REQUIRE(answer["recipes"].size() == 2);
    const auto &toxin = answer["recipes"][0];
    CHECK(toxin["id"] == 7);
    CHECK(toxin["name"] == "Deathroot Toxin");
    CHECK(toxin["canMake"] == true);
    CHECK(toxin["skillHeld"] == true);
    CHECK(toxin["ingredients"][0] == Json{{"name", "Deathroot"}, {"need", 1}, {"have", 3}});
    CHECK(answer["recipes"][1]["canMake"] == false);
    CHECK(answer["recipes"][1]["ingredients"][0] == Json{{"name", "Lyrium Dust"}, {"need", 1}, {"have", 0}});
    CHECK(answer["maker"]["name"] == "Lyna");
    CHECK(world.Crafted() == 0);
    CHECK_FALSE(world.up);
    CHECK(world.sent.back() == "GUI.HideFullScreenGUI");
}

TEST_CASE("craft makes as many as asked, as the Create button does", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "deathroot toxin"}, {"count", 2}});
    CHECK(answer["made"] == 2);
    CHECK(answer["done"] == true);
    CHECK(answer["recipe"]["id"] == 7);
    CHECK(world.sent[0] == "GUICrafting.CraftItem 7 130");
    CHECK(world.Crafted() == 2);
    CHECK(world.pack["gen_im_cft_poi_101"] == 2);
    CHECK(answer["ingredients"][0]["have"] == 1);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft takes a recipe by its id", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", 7}});
    CHECK(answer["made"] == 1);
    CHECK(world.Crafted() == 1);
}

TEST_CASE("craft stops when an ingredient runs short and says which", "[craft]")
{
    Poisoner world;
    world.held["Deathroot"] = 1;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}, {"count", 3}});
    CHECK(answer["made"] == 1);
    CHECK(answer["done"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("1 made, then"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Deathroot (0 of 1)"));
    CHECK(world.Crafted() == 1);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft makes none of a recipe the screen does not allow", "[craft]")
{
    Poisoner world;
    world.skills["0x82"].insert(100073);
    world.recipes[1].skill = 100073;
    world.held["Lyrium Dust"] = 0;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Magebane"}});
    CHECK(answer["made"] == 0);
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Lyrium Dust (0 of 1)"));
    CHECK(world.Crafted() == 0);
}

TEST_CASE("craft says a recipe needs a skill rank the maker has not", "[craft]")
{
    Poisoner world;
    world.recipes[0].skill = 100073;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}});
    CHECK(answer["made"] == 0);
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("needs a rank of Poison-Making"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("100073"));
    CHECK(world.Crafted() == 0);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft is refused a maker without the skill, the screen unopened", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    CHECK_THROWS_WITH(world.Craft(Json{{"kind", "poison"}, {"id", "0x1b88"}}),
                      ContainsSubstring("Zevran has no Poison-Making"));
    CHECK_FALSE(world.up);
    CHECK(world.sent.empty());
    CHECK(world.ordered.empty());
}

TEST_CASE("craft hands control to the maker and gives it back", "[craft]")
{
    Poisoner world;
    world.controlled = "0x1b88";
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"id", "0x82"}, {"recipe", "Deathroot Toxin"}});
    CHECK(answer["made"] == 1);
    CHECK(answer["controlBack"] == true);
    CHECK(world.ordered == std::vector<std::string>{"0x82", "0x1b88"});
    CHECK(world.controlled == "0x1b88");
    CHECK(world.sent[0] == "GUICrafting.CraftItem 7 130");
}

TEST_CASE("craft is refused a maker who cannot be given control", "[craft]")
{
    Poisoner world;
    world.controlled = "0x1b88";
    world.party.erase("0x82");
    AddCraftTool(world.bench);
    CHECK_THROWS_WITH(world.Craft(Json{{"kind", "poison"}, {"id", "0x82"}}),
                      ContainsSubstring("could not be given control"));
    CHECK_FALSE(world.up);
}

TEST_CASE("craft names the recipes known when one asked is not", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Fire Bomb"}});
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("no recipe named \"Fire Bomb\""));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("Deathroot Toxin (7)"));
    CHECK(world.Crafted() == 0);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft takes a recipe by part of its name only when one has it", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    CHECK(world.Craft(Json{{"kind", "poison"}, {"recipe", "deathroot"}})["made"] == 1);
    // A name in full is that recipe, though a longer one has it too.
    world.recipes[1].name = "Deathroot Toxin Extract";
    CHECK(world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}})["recipe"]["id"] == 7);
    world.recipes[1].name = "Deathroot Draught";
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "deathroot"}});
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("more than one recipe"));
}

TEST_CASE("craft makes nothing on a screen open for another character", "[craft]")
{
    Poisoner world;
    world.selected = 99;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}});
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("another character than Lyna"));
    CHECK(world.Crafted() == 0);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft says so when the screen does not open", "[craft]")
{
    Poisoner world;
    world.opens = false;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}});
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("did not open"));
}

TEST_CASE("craft says so when a craft brings nothing into the pack", "[craft]")
{
    Poisoner world;
    world.crafts = false;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}, {"count", 2}});
    CHECK(answer["made"] == 0);
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("nothing came into the pack"));
    CHECK(world.Crafted() == 1);
    CHECK_FALSE(world.up);
}

TEST_CASE("craft is refused a kind that is not a crafting skill", "[craft]")
{
    Poisoner world;
    AddCraftTool(world.bench);
    CHECK_THROWS_WITH(world.Craft(Json{{"kind", "smithing"}}), ContainsSubstring("poison, herbalism or traps"));
    CHECK_THROWS_WITH(world.Craft(Json{{"kind", "poison"}, {"recipe", Json::array()}}),
                      ContainsSubstring("a recipe's id or its name"));
}

TEST_CASE("craft opens the screen for the kind's skill", "[craft]")
{
    Poisoner world;
    world.skills["0x82"].insert(100061);
    AddCraftTool(world.bench);
    world.Craft(Json{{"kind", "herbalism"}});
    CHECK(world.scripts.at(0) == Json{{"command", "HasAbility"}, {"args", {"0x82", 100061}}});
    CHECK(world.scripts.at(1) == Json{{"command", "ShowCraftingGUI"}, {"args", {100061}}});
}

TEST_CASE("craft reads a long list of recipes, as many paths at a time as inspect takes", "[craft]")
{
    Poisoner world;
    for (int i = 0; i < 10; ++i)
    {
        world.recipes.push_back({20 + i,
                                 "Draught " + std::to_string(i),
                                 "Draught",
                                 "gen_im_cft_her_10" + std::to_string(i) + ".uti",
                                 100071,
                                 {{"Deathroot", 1}, {"Flask", 1}}});
    }
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}});
    REQUIRE(answer["recipes"].size() == 12);
    CHECK(answer["recipes"][11]["name"] == "Draught 9");
    CHECK(answer["recipes"][11]["ingredients"][1]["have"] == 3);
}

TEST_CASE("craft waits for what it made to reach the pack", "[craft]")
{
    Poisoner world;
    world.lag = 3;
    AddCraftTool(world.bench);
    const auto answer = world.Craft(Json{{"kind", "poison"}, {"recipe", "Deathroot Toxin"}});
    CHECK(answer["made"] == 1);
    CHECK(answer["done"] == true);
    CHECK(world.Crafted() == 1);
}

TEST_CASE("craft lists only the recipes of the skill asked for, as the screen shows them", "[craft]")
{
    Poisoner world;
    world.skills["0x82"].insert(100061);
    world.recipes.push_back(
        {2, "?Lesser Health Poultice", "Lesser Health Poultice", "gen_im_cft_her_101.uti", 100061, {{"Flask", 1}}});
    world.recipes.push_back({67, "?Rope Trap", "Spring Trap", "gen_im_cft_trp_101.uti", 100044, {{"Flask", 1}}});
    AddCraftTool(world.bench);
    const auto poisons = world.Craft(Json{{"kind", "poison"}});
    REQUIRE(poisons["recipes"].size() == 2);
    CHECK(poisons["recipes"][1]["name"] == "Magebane");
    const auto herbs = world.Craft(Json{{"kind", "herbalism"}});
    REQUIRE(herbs["recipes"].size() == 1);
    CHECK(herbs["recipes"][0]["name"] == "Lesser Health Poultice");
    world.skills["0x82"].insert(100041);
    CHECK(world.Craft(Json{{"kind", "traps"}})["recipes"][0]["product"] == "Spring Trap");
}
