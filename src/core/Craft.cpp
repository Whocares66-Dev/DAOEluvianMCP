#include "core/Craft.h"

#include "core/Args.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eluvian
{

namespace
{

constexpr int kMostCount = 99;

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

// What a recipe lacks, as its ingredients say.
std::string Short(const Json &recipe)
{
    std::string lacks;
    for (const auto &ingredient : recipe.value("ingredients", Json::array()))
    {
        const int have = ingredient.value("have", 0);
        const int need = ingredient.value("need", 0);
        if (have < need)
        {
            lacks += (lacks.empty() ? "" : ", ") + ingredient.value("name", std::string()) + " (" +
                     std::to_string(have) + " of " + std::to_string(need) + ")";
        }
    }
    return lacks.empty() ? "not all its ingredients are held" : "short of " + lacks;
}

// One crafting: its arguments and the answer it builds.
class Crafting
{
  public:
    Crafting(Bench &bench, const Args &args)
        : bench_(bench), kind_(args.Choice("kind", {"poison", "herbalism", "traps", "runes"}))
    {
        if (args.Has("id"))
        {
            maker_ = args.String("id");
        }
        if (args.Has("recipe"))
        {
            const Json &recipe = args.Raw().at("recipe");
            if (!recipe.is_string() && !recipe.is_number_integer())
            {
                throw ToolError("craft: `recipe` is a recipe's id or its name, as craft without one lists them");
            }
            recipe_ = recipe;
        }
        count_ = static_cast<int>(args.Integer("count", 1, 1, kMostCount));
        answer_["cursor"] = bench_.Events().Next();
    }

    Json Run()
    {
        if (maker_.empty())
        {
            const Json game = bench_.Invoke("state", Json::object()).data;
            maker_ = game.value(Json::json_pointer("/controlled/id"), std::string());
        }
        if (!maker_.starts_with("0x"))
        {
            throw ToolError("craft: `id` is a party member's id, like \"0x82\" (state lists the party)");
        }
        const Json character = bench_.Invoke("character", Json{{"id", maker_}}).data;
        name_ = character.value("name", maker_);
        answer_["maker"] = Json{{"id", maker_}, {"name", name_}};
        answer_["kind"] = kind_;
        const Json recipes = Recipes();
        if (recipe_.is_null())
        {
            answer_["recipes"] = recipes;
            return End(true, recipes.empty() ? "no recipes known for " + kind_ + ": merchants sell them"
                                             : "the recipes known; nothing made");
        }
        const Json chosen = Choose(recipes);
        const int id = chosen.value("id", 0);
        answer_["recipe"] = Json{{"id", id},
                                 {"name", chosen.value("name", std::string())},
                                 {"product", chosen.value("product", std::string())}};
        answer_["asked"] = count_;
        int made = 0;
        std::string stopped;
        while (made < count_ && stopped.empty())
        {
            const Json took = bench_.Invoke("craft.make", Json{{"recipe", id}, {"id", maker_}}).data;
            if (took.value("taken", false))
            {
                ++made;
            }
            else
            {
                stopped = Why(took.value("refusal", std::string("refused")), id);
            }
        }
        answer_["made"] = made;
        const Json after = Recipes();
        const auto now = std::ranges::find_if(after, [id](const Json &recipe) { return recipe.value("id", 0) == id; });
        answer_["ingredients"] = now != after.end() ? now->value("ingredients", Json::array()) : Json::array();
        if (!stopped.empty())
        {
            return End(made > 0, (made > 0 ? std::to_string(made) + " made, then " : std::string()) + stopped);
        }
        return End(true, std::to_string(made) + " made");
    }

  private:
    Json Recipes()
    {
        return bench_.Invoke("craft.recipes", Json{{"kind", kind_}, {"id", maker_}})
            .data.value("recipes", Json::array());
    }

    // Why a recipe was not made, by the screen's own test, in words.
    std::string Why(const std::string &refusal, int id)
    {
        const Json recipes = Recipes();
        const auto it = std::ranges::find_if(recipes, [id](const Json &recipe) { return recipe.value("id", 0) == id; });
        const std::string name = it != recipes.end() ? it->value("name", std::string()) : std::to_string(id);
        if (refusal == "no rank")
        {
            return name + " needs a rank of its skill (ability " +
                   std::to_string(it != recipes.end() ? it->value("skill", 0) : 0) + ") that " + name_ + " has not";
        }
        if (refusal == "no ingredients")
        {
            return name + " cannot be made: " + (it != recipes.end() ? Short(*it) : "an ingredient is short");
        }
        if (refusal == "unknown")
        {
            return name + " is not a recipe the party knows";
        }
        return "the game made nothing of " + name;
    }

    // The recipe asked for, by id or by name (the recipe's or its
    // product's), in any case, or by part of one where only one has it.
    Json Choose(const Json &recipes) const
    {
        const auto names = [&recipes] {
            std::string all;
            for (const auto &recipe : recipes)
            {
                all += (all.empty() ? "" : ", ") + recipe.value("name", std::string()) + " (" +
                       std::to_string(recipe.value("id", 0)) + ")";
            }
            return all.empty() ? std::string("none") : all;
        };
        if (recipe_.is_number_integer())
        {
            for (const auto &recipe : recipes)
            {
                if (recipe.value("id", 0) == recipe_.get<int>())
                {
                    return recipe;
                }
            }
            throw ToolError("craft: no recipe " + recipe_.dump() + " for " + kind_ + "; known: " + names());
        }
        const std::string asked = Lower(recipe_.get<std::string>());
        std::vector<Json> near;
        for (const auto &recipe : recipes)
        {
            const std::string name = Lower(recipe.value("name", std::string()));
            const std::string product = Lower(recipe.value("product", std::string()));
            if (name == asked || product == asked)
            {
                return recipe;
            }
            if (name.find(asked) != std::string::npos || product.find(asked) != std::string::npos)
            {
                near.push_back(recipe);
            }
        }
        if (near.size() == 1)
        {
            return near.front();
        }
        throw ToolError(std::string("craft: ") +
                        (near.empty() ? "no recipe named \"" : "more than one recipe names \"") +
                        recipe_.get<std::string>() + "\"; known: " + names());
    }

    Json End(bool done, std::string reason)
    {
        answer_["done"] = done;
        answer_["reason"] = std::move(reason);
        return answer_;
    }

    Bench &bench_;
    std::string kind_;
    std::string maker_;
    std::string name_;
    Json recipe_;
    int count_ = 1;
    Json answer_ = Json::object();
};

} // namespace

void AddCraftTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{
            "craft",
            "Makes poisons, potions, traps and runes as the game's crafting screen does, so the game's own rules "
            "apply. `kind` picks the craft: poison, herbalism, traps or runes (Awakening's). The maker is `id`, a "
            "party member (default: the creature you control); no screen is opened and control stays where it "
            "is.\n\n"
            "- Without `recipe`, lists the party's known recipes of the craft, as the screen shows them to the "
            "maker, and makes nothing. Each has its `id`, `name`, `product`, whether it `canMake` now, the `skill` "
            "rank it needs and whether the maker has it (`skillHeld`), and its `ingredients` with how many it "
            "needs and how many the pack has.\n"
            "- With `recipe` (an id or a name from that list), makes it `count` times, one at a time, like the "
            "screen's Create button, for as long as the game allows.\n\n"
            "The answer includes `made`, the recipe's `ingredients` afterwards, `done`, and `reason`: what ran "
            "short or which skill rank was missing, if it stopped. Recipes are bought from merchants.",
            Json{{"type", "object"},
                 {"properties",
                  {{"kind",
                    {{"type", "string"},
                     {"enum", {"poison", "herbalism", "traps", "runes"}},
                     {"description", "The craft whose recipes to use."}}},
                   {"id",
                    {{"type", "string"},
                     {"description", "The maker, a party member's creature id. Default: the creature you control."}}},
                   {"recipe",
                    {{"description", "The recipe to make: its id (a number) or name, as the list shows. Omit it to "
                                     "list the recipes."}}},
                   {"count",
                    {{"type", "integer"},
                     {"minimum", 1},
                     {"maximum", kMostCount},
                     {"description", "How many to make. Default 1."}}}}},
                 {"required", {"kind"}}},
            Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "craft");
            return Crafting(bench, args).Run();
        });
}

} // namespace eluvian
