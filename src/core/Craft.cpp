#include "core/Craft.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// How long the screen may take to open or close: a frame or two in play.
constexpr milliseconds kScreenWait{5000};
// How long a made item may take to reach the pack.
constexpr milliseconds kMadeWait{3000};
constexpr int kMostCount = 99;
// inspect's `paths` reads up to 64 at once.
constexpr std::size_t kMostPaths = 64;

// The crafting skills by their first rank, the ability ShowCraftingGUI is
// given: crafting.gfx's Crafting.POISONS, HERBALISM and TRAPS, the scripts'
// ABILITY_SKILL_POISON_1, HERBALISM_1 and TRAPS_1 (2da_constants_h.nss).
struct Kind
{
    std::string_view name;
    std::int32_t ability;
    std::string_view skill;
};
constexpr std::array<Kind, 3> kKinds{{
    {"poison", 100071, "Poison-Making"},
    {"herbalism", 100061, "Herbalism"},
    {"traps", 100041, "Trap-Making"},
}};

// A creature's id as the screen's messages and the data tree carry it, in
// decimal: "0x82" is 130.
std::optional<std::uint32_t> Decimal(std::string_view id)
{
    if (id.size() < 3 || !id.starts_with("0x"))
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto *end = id.data() + id.size();
    const auto [stop, error] = std::from_chars(id.data() + 2, end, value, 16);
    if (error != std::errc{} || stop != end)
    {
        return std::nullopt;
    }
    return value;
}

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

// A template's name as an item's and a recipe's product compare: the
// recipe's may carry the ".uti" an item's does not, or the other way.
std::string ResRef(std::string_view name)
{
    std::string ref = Lower(name);
    if (ref.ends_with(".uti"))
    {
        ref.resize(ref.size() - 4);
    }
    return ref;
}

// A skill rank's first, as crafting.gfx's BaseCraftingSkillOf gives it: a
// craft's four ranks follow its first (TRAPS_1 to _4 are 100041 to
// 100044); 0 for an ability of none.
std::int32_t BaseOf(std::int32_t skill)
{
    for (const auto &kind : kKinds)
    {
        if (skill >= kind.ability && skill < kind.ability + 4)
        {
            return kind.ability;
        }
    }
    return 0;
}

// A script's or a tree's yes: a bool, or a number not zero.
bool Yes(const Json &value)
{
    return value.is_boolean() ? value.get<bool>() : value.is_number() && value.get<double>() != 0;
}

struct Ingredient
{
    std::string name;
    int need = 0;
    int have = 0;
};

// A recipe as the screen lists it.
struct Recipe
{
    std::string path; // GUICrafting.Recipes.<i>
    int id = 0;
    std::string name;
    std::string product;
    std::string productRef;
    int skill = 0; // the ability the maker needs: a rank of the craft's skill
    bool canMake = false;
    std::vector<Ingredient> ingredients;
};

// One crafting: its arguments and the answer it builds.
class Crafting
{
  public:
    Crafting(Bench &bench, const Args &args, milliseconds show)
        : bench_(bench), show_(milliseconds(args.Integer("showMs", show.count(), 0, 10000)))
    {
        const std::string kind = Lower(args.String("kind", ""));
        const auto found = std::ranges::find(kKinds, std::string_view(kind), &Kind::name);
        if (found == kKinds.end())
        {
            throw ToolError("craft: `kind` is poison, herbalism or traps: the crafting skill whose recipes to use");
        }
        kind_ = *found;
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
        const Json game = bench_.Invoke("inspect", Json{{"kind", "game"}}).data;
        const std::string controlled = game.value(Json::json_pointer("/controlled/id"), std::string());
        if (maker_.empty())
        {
            maker_ = controlled;
        }
        const auto number = Decimal(maker_);
        if (!number)
        {
            throw ToolError("craft: `id` is a party member's id, like \"0x82\" (inspect kind=game lists the party)");
        }
        n_ = *number;
        const Json character = bench_.Invoke("inspect", Json{{"kind", "character"}, {"id", maker_}}).data;
        const std::string name = character.value("name", maker_);
        answer_["maker"] = Json{{"id", maker_}, {"name", name}};
        answer_["kind"] = kind_.name;
        if (!Held(kind_.ability))
        {
            throw ToolError("craft: " + name + " has no " + std::string(kind_.skill) +
                            "; inspect kind=abilities lists a creature's skills, and another of the party may have it");
        }
        // The screen crafts for the character the interface has selected,
        // the one controlled.
        if (maker_ != controlled)
        {
            const Json order = bench_.Invoke("order", Json{{"action", "control"}, {"id", maker_}}).data;
            if (!order.value("taken", false))
            {
                throw ToolError("craft: " + name + " could not be given control, which the crafting screen wants " +
                                "(at the party camp the game may not hand it over); order control said so");
            }
            handBack_ = controlled;
        }
        Json answer = Craft();
        if (!handBack_.empty())
        {
            const auto back = Ask("order", Json{{"action", "control"}, {"id", handBack_}});
            answer["controlBack"] = back && back->value("taken", false);
        }
        return answer;
    }

  private:
    Json Craft()
    {
        if (!Open())
        {
            return End(false, refused_);
        }
        const auto selected = Ask("inspect", Json{{"kind", "data"}, {"path", "GUI.SelectedChar.ID"}});
        if (!selected || selected->value("number", -1.0) != static_cast<double>(n_))
        {
            Close();
            return End(false, "the crafting screen opened for another character than " +
                                  answer_["maker"]["name"].get<std::string>() + "; nothing was made");
        }
        auto recipes = Recipes();
        if (!recipes)
        {
            Close();
            return End(false, refused_);
        }
        if (recipe_.is_null())
        {
            return List(*recipes);
        }
        const Recipe *chosen = Choose(*recipes);
        if (chosen == nullptr)
        {
            Close();
            return End(false, refused_);
        }
        const int id = chosen->id;
        answer_["recipe"] = Json{{"id", id}, {"name", chosen->name}, {"product", chosen->product}};
        answer_["asked"] = count_;
        std::this_thread::sleep_for(show_);
        int made = 0;
        Recipe now = *chosen;
        const std::string stopped = Make(now, made);
        answer_["made"] = made;
        answer_["ingredients"] = RecipeJson(now)["ingredients"];
        std::this_thread::sleep_for(show_);
        if (!Close())
        {
            return End(made > 0, refused_);
        }
        if (!stopped.empty())
        {
            return End(made > 0, (made > 0 ? std::to_string(made) + " made, then " : std::string()) + stopped);
        }
        return End(true, std::to_string(made) + " made");
    }

    // The recipes the screen lists, when none was asked for: nothing made.
    Json List(const std::vector<Recipe> &recipes)
    {
        Json list = Json::array();
        for (const auto &recipe : recipes)
        {
            list.push_back(RecipeJson(recipe));
        }
        answer_["recipes"] = std::move(list);
        std::this_thread::sleep_for(show_);
        if (!Close())
        {
            return End(false, refused_);
        }
        answer_["reason"] = recipes.empty()
                                ? "no recipes known for " + std::string(kind_.skill) + ": merchants sell them"
                                : "the recipes the screen lists; nothing made";
        return answer_;
    }

    // One at a time, as the screen's button makes them, up to `count_`:
    // `made` counts them and `now` is the recipe as the screen lists it
    // after the last. Why it stopped short, or nothing.
    std::string Make(Recipe &now, int &made)
    {
        const int id = now.id;
        while (made < count_)
        {
            if (!Held(now.skill))
            {
                return now.name + " needs a rank of " + std::string(kind_.skill) + " (ability " +
                       std::to_string(now.skill) + ") that " + answer_["maker"]["name"].get<std::string>() + " has not";
            }
            if (!now.canMake)
            {
                return "the screen does not let " + now.name + " be made: " + Short(now);
            }
            const auto before = InPack(now.productRef);
            if (!before || !Send("GUICrafting.CraftItem " + std::to_string(id) + " " + std::to_string(n_)))
            {
                return refused_;
            }
            const auto after = Made(now.productRef, *before);
            if (!after)
            {
                return refused_;
            }
            made += *after - *before;
            // The screen's own list again, as its RefreshRecipes reads it
            // after a craft: what is left, and whether another may be made.
            const auto again = Recipes();
            if (!again)
            {
                return refused_;
            }
            const auto it = std::ranges::find(*again, id, &Recipe::id);
            if (it == again->end())
            {
                return now.name + " is no longer listed";
            }
            now = *it;
        }
        return {};
    }

    // The recipe asked for, by id or by name (the recipe's or its
    // product's); nothing with `refused_` said.
    const Recipe *Choose(const std::vector<Recipe> &recipes)
    {
        const auto names = [&recipes] {
            std::string all;
            for (const auto &recipe : recipes)
            {
                all += (all.empty() ? "" : ", ") + recipe.name + " (" + std::to_string(recipe.id) + ")";
            }
            return all.empty() ? std::string("none") : all;
        };
        if (recipe_.is_number_integer())
        {
            const auto it = std::ranges::find(recipes, recipe_.get<int>(), &Recipe::id);
            if (it != recipes.end())
            {
                return &*it;
            }
            refused_ = "no recipe " + recipe_.dump() + " for " + std::string(kind_.skill) + "; known: " + names();
            return nullptr;
        }
        const std::string asked = Lower(recipe_.get<std::string>());
        std::vector<const Recipe *> near;
        for (const auto &recipe : recipes)
        {
            const std::string name = Lower(recipe.name);
            const std::string product = Lower(recipe.product);
            if (name == asked || product == asked)
            {
                return &recipe;
            }
            if (name.find(asked) != std::string::npos || product.find(asked) != std::string::npos)
            {
                near.push_back(&recipe);
            }
        }
        if (near.size() == 1)
        {
            return near.front();
        }
        refused_ = (near.empty() ? "no recipe named \"" : "more than one recipe names \"") +
                   recipe_.get<std::string>() + "\"; known: " + names();
        return nullptr;
    }

    // What a recipe lacks, as its ingredients say.
    static std::string Short(const Recipe &recipe)
    {
        std::string lacks;
        for (const auto &ingredient : recipe.ingredients)
        {
            if (ingredient.have < ingredient.need)
            {
                lacks += (lacks.empty() ? "" : ", ") + ingredient.name + " (" + std::to_string(ingredient.have) +
                         " of " + std::to_string(ingredient.need) + ")";
            }
        }
        return lacks.empty() ? "it says not all its ingredients are held" : "short of " + lacks;
    }

    Json RecipeJson(const Recipe &recipe)
    {
        Json ingredients = Json::array();
        for (const auto &ingredient : recipe.ingredients)
        {
            ingredients.push_back(
                Json{{"name", ingredient.name}, {"need", ingredient.need}, {"have", ingredient.have}});
        }
        return Json{{"id", recipe.id},
                    {"name", recipe.name},
                    {"product", recipe.product},
                    {"canMake", recipe.canMake},
                    {"skill", recipe.skill},
                    {"skillHeld", Held(recipe.skill)},
                    {"ingredients", std::move(ingredients)}};
    }

    // Whether the maker has an ability, as the screen asks it
    // (DAExternalCommands.HasAbility, SelectedCharMeetsRequirements); the
    // scripts' HasAbility answers the same. Asked once each.
    bool Held(int ability)
    {
        if (const auto known = held_.find(ability); known != held_.end())
        {
            return known->second;
        }
        const auto has = Ask("script", Json{{"command", "HasAbility"}, {"args", Json::array({maker_, ability})}});
        const bool yes = has && Yes(has->value("value", Json()));
        held_[ability] = yes;
        return yes;
    }

    // The recipes the screen lists, read from its tree.
    std::optional<std::vector<Recipe>> Recipes()
    {
        const auto members = Members("GUICrafting.Recipes");
        if (!members)
        {
            return std::nullopt;
        }
        std::vector<Recipe> recipes;
        std::vector<std::string> paths;
        std::vector<std::vector<std::string>> ingredientPaths;
        for (const auto &member : *members)
        {
            Recipe recipe;
            recipe.path = member;
            for (const char *field :
                 {".ID", ".Name", ".ProductName", ".ProductResRef", ".Skill", ".HaveAllIngredients"})
            {
                paths.push_back(member + field);
            }
            const auto ingredients = Members(member + ".Ingredients");
            if (!ingredients)
            {
                return std::nullopt;
            }
            for (const auto &ingredient : *ingredients)
            {
                for (const char *field : {".Name", ".Need", ".Have"})
                {
                    paths.push_back(ingredient + field);
                }
            }
            ingredientPaths.push_back(*ingredients);
            recipes.push_back(std::move(recipe));
        }
        const auto values = Read(paths);
        if (!values)
        {
            return std::nullopt;
        }
        const auto text = [&values](const std::string &path) {
            const auto it = values->find(path);
            return it == values->end() ? std::string() : it->second.value("text", std::string());
        };
        const auto number = [&values](const std::string &path) {
            const auto it = values->find(path);
            return it == values->end() ? 0 : static_cast<int>(it->second.value("number", 0.0));
        };
        for (std::size_t i = 0; i < recipes.size(); ++i)
        {
            auto &recipe = recipes[i];
            const std::string &p = recipe.path;
            recipe.id = number(p + ".ID");
            // In play every recipe's name came with a "?" before it, its
            // product's without (turn-based-agent, 2026-10-05): a name the
            // game has no text for, shown by its label.
            recipe.name = text(p + ".Name");
            if (recipe.name.starts_with('?'))
            {
                recipe.name.erase(0, 1);
            }
            recipe.product = text(p + ".ProductName");
            recipe.productRef = text(p + ".ProductResRef");
            recipe.skill = number(p + ".Skill");
            recipe.canMake = number(p + ".HaveAllIngredients") != 0;
            for (const auto &ingredient : ingredientPaths[i])
            {
                recipe.ingredients.push_back(
                    Ingredient{text(ingredient + ".Name"), number(ingredient + ".Need"), number(ingredient + ".Have")});
            }
        }
        // The tree lists every craft's recipes; the screen shows those of
        // the skill it was opened for, grouped by BaseCraftingSkillOf(the
        // recipe's Skill) (crafting.gfx's LoadRecipes). In play a poison's
        // listing held herbalism's and the traps' too (2026-10-05).
        std::erase_if(recipes, [this](const Recipe &recipe) { return BaseOf(recipe.skill) != kind_.ability; });
        return recipes;
    }

    // A list's members in the tree, each as its full path.
    std::optional<std::vector<std::string>> Members(const std::string &path)
    {
        const auto list = Ask("inspect", Json{{"kind", "data"}, {"path", path}, {"members", true}});
        if (!list)
        {
            return std::nullopt;
        }
        const std::string prefix = path + ".";
        std::vector<std::string> members;
        for (const auto &member : list->value("members", Json::array()))
        {
            const std::string name = member.get<std::string>();
            members.push_back(name.starts_with(prefix) ? name : prefix + name);
        }
        return members;
    }

    // The values at paths, by path, read 64 at a time.
    std::optional<std::map<std::string, Json>> Read(const std::vector<std::string> &paths)
    {
        std::map<std::string, Json> values;
        for (std::size_t from = 0; from < paths.size(); from += kMostPaths)
        {
            Json chunk = Json::array();
            for (std::size_t at = from; at < std::min(paths.size(), from + kMostPaths); ++at)
            {
                chunk.push_back(paths[at]);
            }
            const auto read = Ask("inspect", Json{{"kind", "data"}, {"paths", std::move(chunk)}});
            if (!read)
            {
                return std::nullopt;
            }
            for (const auto &value : read->value("values", Json::array()))
            {
                values[value.value("path", std::string())] = value;
            }
        }
        return values;
    }

    // How many of a product the party's pack holds.
    std::optional<int> InPack(const std::string &productRef)
    {
        const auto pack = Ask("inspect", Json{{"kind", "inventory"}});
        if (!pack)
        {
            return std::nullopt;
        }
        int count = 0;
        for (const auto &item : pack->value("items", Json::array()))
        {
            if (ResRef(item.value("template", std::string())) == ResRef(productRef))
            {
                count += item.value("stack", 1);
            }
        }
        return count;
    }

    // Waits for more of a product in the pack than `before`; its count, or
    // nothing with `refused_` said.
    std::optional<int> Made(const std::string &productRef, int before)
    {
        const auto deadline = Clock::now() + kMadeWait;
        while (Clock::now() < deadline)
        {
            const auto count = InPack(productRef);
            if (!count)
            {
                return std::nullopt;
            }
            if (*count > before)
            {
                return count;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        refused_ = "the screen took the craft and nothing came into the pack in 3 s";
        return std::nullopt;
    }

    // The screen opened as the skill's use opens it; false with `refused_`
    // said.
    bool Open()
    {
        if (!Ask("script", Json{{"command", "ShowCraftingGUI"}, {"args", Json::array({kind_.ability})}}))
        {
            return false;
        }
        if (!Screen(true))
        {
            refused_ = "the crafting screen did not open: " + refused_ + " (in a fight or a conversation it may not)";
            return false;
        }
        return true;
    }

    // The screen closed as its Close button closes it.
    bool Close()
    {
        if (!Send("GUI.HideFullScreenGUI"))
        {
            return false;
        }
        if (!Screen(false))
        {
            refused_ = "the crafting screen did not close: " + refused_ +
                       R"(; input {"message": "GUI.HideFullScreenGUI"} closes it)";
            return false;
        }
        return true;
    }

    // Waits for the crafting screen to be up, or gone.
    bool Screen(bool up)
    {
        const auto deadline = Clock::now() + kScreenWait;
        while (Clock::now() < deadline)
        {
            const auto gui = Ask("inspect", Json{{"kind", "gui"}});
            if (!gui)
            {
                return false;
            }
            const bool shown = gui->value("fullScreen", Json()) == "Crafting";
            if (shown == up)
            {
                return true;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        refused_ = up ? "it was not up after 5 s" : "it was still up after 5 s";
        return false;
    }

    // One of the game's messages, sent; false with `refused_` said.
    bool Send(const std::string &text)
    {
        const auto sent = Ask("input", Json{{"message", text}});
        if (!sent)
        {
            return false;
        }
        if (!sent->value("sent", false))
        {
            refused_ = "the game took no \"" + text + "\"";
            return false;
        }
        return true;
    }

    // A tool's answer, or nothing with `refused_` said: once the screen is
    // in play a refusal ends the crafting with it said, as levelup's does.
    std::optional<Json> Ask(std::string_view tool, const Json &args)
    {
        try
        {
            return bench_.Invoke(tool, args).data;
        }
        catch (const ToolError &e)
        {
            refused_ = e.what();
        }
        catch (const FrameTimeout &e)
        {
            refused_ = std::string("no answer from ") + std::string(tool) + ": " + e.what();
        }
        return std::nullopt;
    }

    Json End(bool made, std::string reason)
    {
        answer_["done"] = made;
        answer_["reason"] = std::move(reason);
        return answer_;
    }

    Bench &bench_;
    milliseconds show_;
    Kind kind_{};
    std::string maker_;
    std::uint32_t n_ = 0; // the maker's id in decimal, as the screen's messages carry it
    Json recipe_;
    int count_ = 1;
    std::string handBack_;
    std::map<int, bool> held_;
    std::string refused_;
    Json answer_ = Json::object();
};

} // namespace

void AddCraftTool(Bench &bench, milliseconds show)
{
    bench.Tools().Add(
        ToolSpec{
            "craft",
            "Make poisons, potions and traps as the game's crafting screen makes them, so its rules hold. "
            "`kind`: poison, herbalism or traps. Without `recipe` it lists what the screen offers the maker "
            "(`id`, a party member, default the one controlled): each recipe's `id`, `name`, `product`, "
            "whether it `canMake` now, the `skill` rank it needs and whether the maker has it (`skillHeld`), "
            "and its `ingredients` with how many it needs and the pack has; nothing is made. With `recipe` (an "
            "id or a name, from that list) and `count` (default 1) it makes it as the screen's Create button "
            "does, one at a time, while the screen allows. Answers `made`, the recipe's `ingredients` after, "
            "`done` and `reason`: what was short, or which skill rank was missing, if it stopped. The screen "
            "opens for the character controlled, so another maker is given control first and the one before "
            "given it back (`controlBack`). Refused for a maker without the skill; recipes are bought from "
            "merchants. `showMs` (default 1000) is how long it lingers on the screen, so whoever watches sees "
            "it; 0 for none.",
            Json{{"type", "object"},
                 {"properties",
                  {{"kind",
                    {{"type", "string"},
                     {"enum", {"poison", "herbalism", "traps"}},
                     {"description", "The crafting skill whose recipes to use"}}},
                   {"id",
                    {{"type", "string"}, {"description", "The maker, a party member; default the one controlled"}}},
                   {"recipe", {{"description", "A recipe's id (a number) or name, as the list without one gives"}}},
                   {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMostCount}}},
                   {"showMs", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}},
                 {"required", {"kind"}}},
            Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "craft");
            return Crafting(bench, args, show).Run();
        });
}

} // namespace eluvian
