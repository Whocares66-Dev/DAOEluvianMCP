// Crafting's steps through DAOLib's dao::crafting (DAOLib #259, lib-agent-3,
// 2026-10-25): the party's known recipes of a craft as the screen groups and
// shows them to a crafter, with no screen, and one made as its Create does,
// refused where the screen greys it out. core's craft asks these, unlisted,
// where it opened the screen, handed the maker control, read GUICrafting's
// tree, grouped it by a copy of crafting.gfx's BaseCraftingSkillOf and sent
// GUICrafting.CraftItem (#59, #186).
#include "game/Shared.h"

#include "dao/Crafting.h"

namespace eluvian::game
{

namespace
{

namespace crafting = dao::crafting;

crafting::Craft CraftNamed(const Args &args)
{
    const auto kind = args.Choice("kind", {"traps", "herbalism", "poison", "runes"});
    if (kind == "traps")
    {
        return crafting::Craft::Traps;
    }
    if (kind == "herbalism")
    {
        return crafting::Craft::Herbalism;
    }
    return kind == "poison" ? crafting::Craft::Poisons : crafting::Craft::Runes;
}

Json RecipeJson(const crafting::Recipe &recipe)
{
    Json ingredients = Json::array();
    for (const crafting::Ingredient &ingredient : recipe.ingredients)
    {
        ingredients.push_back(Json{{"name", Narrow(ingredient.name)},
                                   {"tag", Narrow(ingredient.tag)},
                                   {"need", ingredient.need},
                                   {"have", ingredient.have}});
    }
    // Every recipe's name has no text in the game, so the screen shows "?"
    // and its label (turn-based-agent, 2026-10-05; DAOLib the same): the
    // label is the name an agent writes.
    std::string name = Narrow(recipe.name);
    if (name.starts_with('?'))
    {
        name.erase(0, 1);
    }
    return Json{{"id", recipe.id},
                {"name", std::move(name)},
                {"product", Narrow(recipe.productName)},
                {"productTemplate", Narrow(recipe.product)},
                {"skill", recipe.skill},
                {"skillHeld", recipe.rank},
                {"canMake", recipe.makeable},
                {"ingredients", std::move(ingredients)}};
}

ToolSpec Step(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.listed = false;
    return spec;
}

} // namespace

void AddCrafting(Bench &bench, const Watch &watch)
{
    const Json maker = {{"type", "string"}, {"description", "The crafter, by id."}};
    bench.Tools().Add(
        Step("craft.recipes",
             "The party's known recipes of a craft (`kind`: traps, herbalism, poison, runes) as the crafting "
             "screen shows them to the crafter `id`: each with `id`, `name`, `product`, the `skill` rank it needs "
             "and whether the crafter has it (`skillHeld`), whether it can be made now (`canMake`), and its "
             "`ingredients` (name, tag, need, have).",
             Json{{"kind", {{"type", "string"}}}, {"id", maker}}, {"kind", "id"}),
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "craft.recipes");
            const Args args(raw, "craft.recipes");
            const auto craft = CraftNamed(args);
            const ObjectId crafter = args.Id("id", "a party member");
            Json recipes = Json::array();
            for (const crafting::Recipe &recipe : crafting::Recipes(craft, crafter))
            {
                recipes.push_back(RecipeJson(recipe));
            }
            return Json{{"recipes", std::move(recipes)}};
        });

    bench.Tools().Add(Step("craft.make",
                           "Makes one of `recipe` by the crafter `id`, as the screen's Create does: `taken`, or the "
                           "`refusal` (unknown, no rank, no ingredients, not made).",
                           Json{{"recipe", {{"type", "integer"}}}, {"id", maker}}, {"recipe", "id"}),
                      [&watch](const Json &raw) -> ToolResult {
                          RequireWorld(watch, "craft.make");
                          const Args args(raw, "craft.make");
                          const auto recipe = static_cast<std::int32_t>(args.Integer("recipe", 0, 0, INT32_MAX));
                          const auto refusal = crafting::Make(recipe, args.Id("id", "a party member"));
                          if (refusal == crafting::Refusal::None)
                          {
                              return Json{{"taken", true}};
                          }
                          return Json{{"taken", false},
                                      {"refusal", Label(refusal, {{crafting::Refusal::Unknown, "unknown"},
                                                                  {crafting::Refusal::NoRank, "no rank"},
                                                                  {crafting::Refusal::NoIngredients, "no ingredients"},
                                                                  {crafting::Refusal::NotMade, "not made"}})}};
                      });
}

} // namespace eluvian::game
