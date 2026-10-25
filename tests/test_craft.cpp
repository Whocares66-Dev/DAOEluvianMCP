#include "core/Craft.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The party's recipes of a craft as DAOLib's dao::crafting answers them
// (#186): craft.recipes lists the known ones as the screen shows them to a
// crafter, and craft.make makes one as its Create does, refused where the
// screen greys it out.
struct Workshop
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    int elfroot = 3;                // in the pack
    int flasks = 2;                 // in the pack
    bool improved = false;          // whether the maker has Improved Herbalism
    bool madeNothing = false;       // the engine makes nothing of a craft
    std::vector<std::string> calls; // the steps, in order
    std::string askedKind;          // the kind craft.recipes was asked for

    Workshop()
    {
        AddReads(bench, [](const Json &args) -> ToolResult { return FakeRead(args); });
        bench.Tools().Add(ToolSpec{"craft.recipes", "", {}, Thread::Listener}, [this](const Json &args) -> ToolResult {
            askedKind = args.value("kind", "");
            if (args.value("id", "") != "0x82" && args.value("id", "") != "0x10d")
            {
                throw ToolError("craft.recipes: not a party member");
            }
            return Json{{"recipes", Recipes()}};
        });
        bench.Tools().Add(ToolSpec{"craft.make", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return Make(args); });
        AddCraftTool(bench);
    }

    static ToolResult FakeRead(const Json &args)
    {
        if (args.value("kind", "") == "game")
        {
            return Json{{"controlled", {{"id", "0x82"}, {"name", "Lyna"}}}};
        }
        return Json{{"name", args.value("id", "") == "0x10d" ? "Wynne" : "Lyna"}};
    }

    static Json Recipe(int id, const char *name, const char *product, int skill, bool held,
                       const std::vector<std::pair<const char *, std::pair<int, int>>> &ingredients)
    {
        Json list = Json::array();
        bool all = true;
        for (const auto &[ingredient, counts] : ingredients)
        {
            list.push_back(Json{{"name", ingredient}, {"tag", "tag"}, {"need", counts.first}, {"have", counts.second}});
            all = all && counts.second >= counts.first;
        }
        return Json{{"id", id},          {"name", name},           {"product", product}, {"skill", skill},
                    {"skillHeld", held}, {"canMake", held && all}, {"ingredients", list}};
    }

    Json Recipes() const
    {
        return Json::array({Recipe(1, "Lesser Health Poultice", "Lesser Health Poultice", 100061, true,
                                   {{"Elfroot", {1, elfroot}}, {"Flask", {1, flasks}}}),
                            Recipe(2, "Health Poultice", "Health Poultice", 100062, improved,
                                   {{"Elfroot", {2, elfroot}}, {"Flask", {1, flasks}}}),
                            Recipe(3, "Lesser Lyrium Potion", "Lesser Lyrium Potion", 100061, true,
                                   {{"Lyrium Dust", {1, 0}}, {"Flask", {1, flasks}}})});
    }

    static Json Refused(const char *refusal)
    {
        return Json{{"taken", false}, {"refusal", refusal}};
    }

    ToolResult Make(const Json &args)
    {
        const int recipe = args.value("recipe", 0);
        calls.push_back("make " + std::to_string(recipe) + " by " + args.value("id", ""));
        if (recipe < 1 || recipe > 3)
        {
            return Refused("unknown");
        }
        if (recipe == 2 && !improved)
        {
            return Refused("no rank");
        }
        const int roots = recipe == 2 ? 2 : 1;
        if ((recipe == 3) || elfroot < roots || flasks < 1)
        {
            return Refused("no ingredients");
        }
        if (madeNothing)
        {
            return Refused("not made");
        }
        elfroot -= roots;
        flasks -= 1;
        return Json{{"taken", true}};
    }

    Json Craft(const Json &args)
    {
        return bench.Invoke("craft", args).data;
    }
};

} // namespace

TEST_CASE("craft lists the recipes known, nothing made", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}});
    CHECK(answer["done"] == true);
    CHECK(answer["maker"]["id"] == "0x82"); // the one controlled, none being named
    REQUIRE(answer["recipes"].size() == 3);
    CHECK(answer["recipes"][1]["skillHeld"] == false);
    CHECK(shop.askedKind == "herbalism");
    CHECK(shop.calls.empty());
}

TEST_CASE("craft makes as many as asked, as the Create button does", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", "lesser health poultice"}, {"count", 2}});
    CHECK(answer["done"] == true);
    CHECK(answer["made"] == 2);
    CHECK(answer["reason"] == "2 made");
    CHECK(answer["recipe"]["id"] == 1);
    CHECK(answer["ingredients"][0]["have"] == 1); // the elfroot left
    CHECK(shop.calls == std::vector<std::string>{"make 1 by 0x82", "make 1 by 0x82"});
}

TEST_CASE("craft takes a recipe by its id, and by the one name a part of it names", "[craft]")
{
    Workshop shop;
    CHECK(shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 1}})["made"] == 1);
    CHECK(shop.Craft(Json{{"kind", "herbalism"}, {"recipe", "lyrium"}})["recipe"]["id"] == 3);
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "herbalism"}, {"recipe", "poultice"}}),
                      ContainsSubstring("more than one recipe names \"poultice\""));
}

TEST_CASE("craft names the recipes known when one asked is not", "[craft]")
{
    Workshop shop;
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "herbalism"}, {"recipe", "Fire Bomb"}}),
                      ContainsSubstring("known: Lesser Health Poultice (1), Health Poultice (2)"));
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 9}}), ContainsSubstring("no recipe 9"));
    CHECK(shop.calls.empty());
}

TEST_CASE("craft stops when an ingredient runs short and says which", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 1}, {"count", 5}});
    CHECK(answer["made"] == 2); // two flasks
    CHECK(answer["done"] == true);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("2 made, then"));
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("short of Flask (0 of 1)"));
}

TEST_CASE("craft makes none of a recipe short from the start, saying why", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 3}});
    CHECK(answer["made"] == 0);
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("short of Lyrium Dust (0 of 1)"));
}

TEST_CASE("craft says a recipe needs a skill rank the maker has not", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", "Health Poultice"}});
    CHECK(answer["made"] == 0);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("ability 100062) that Lyna has not"));
}

TEST_CASE("craft says so when the game makes nothing", "[craft]")
{
    Workshop shop;
    shop.madeNothing = true;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 1}});
    CHECK(answer["done"] == false);
    CHECK_THAT(answer["reason"].get<std::string>(), ContainsSubstring("the game made nothing of"));
}

TEST_CASE("craft makes for the maker named, control left where it is", "[craft]")
{
    Workshop shop;
    const auto answer = shop.Craft(Json{{"kind", "herbalism"}, {"recipe", 1}, {"id", "0x10d"}});
    CHECK(answer["maker"]["name"] == "Wynne");
    CHECK(shop.calls == std::vector<std::string>{"make 1 by 0x10d"});
    CHECK_FALSE(answer.contains("controlBack"));
}

TEST_CASE("craft is refused a kind that is not a craft, or a maker not named by id", "[craft]")
{
    Workshop shop;
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "smithing"}}), ContainsSubstring("poison"));
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "traps"}, {"id", "Lyna"}}), ContainsSubstring("like \"0x82\""));
    CHECK_THROWS_WITH(shop.Craft(Json{{"kind", "traps"}, {"recipe", true}}), ContainsSubstring("id or its name"));
    CHECK(shop.Craft(Json{{"kind", "runes"}})["kind"] == "runes");
}
