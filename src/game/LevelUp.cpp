// The level-up's steps with no screen, through DAOLib's dao::levelup (DAOLib
// #259, 138c47e, lib-agent-3, 2026-10-24): what a creature is offered, an
// ability or specialization taken, an attribute raised, the screen's Auto
// Level Up and its Done, each refused where the screen would refuse it by
// the engine's own test. core's levelup asks these, unlisted, where it
// pressed the screen's icons through its movie (#180): a page's icons made
// late (#106), only the page shown (#71), a paperdoll not every game has
// (#126).
#include "game/Shared.h"

#include "dao/LevelUp.h"

namespace eluvian::game
{

namespace
{

namespace levelup = dao::levelup;

constexpr std::string_view kCreature = "the creature levelling up, by id";

ObjectId Creature(const Args &args)
{
    const ObjectId id = args.Id("id", kCreature);
    if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("{}: {} is not a creature", args.Tool(), FormatId(id)));
    }
    return id;
}

// The states as levelup has always named them: an ability's "not yet" is a
// level, an attribute or the one before it in its chain.
Json Offered(ObjectId id, levelup::Kind kind, levelup::Mode mode)
{
    Json listed = Json::array();
    for (const levelup::Offer &offer : levelup::Offers(id, kind, mode))
    {
        listed.push_back(Json{{"id", offer.ability},
                              {"name", AbilityJson(offer.ability).value("name", std::string())},
                              {"state", Label(offer.state, {{levelup::State::Have, "has"},
                                                            {levelup::State::Learnable, "learnable"},
                                                            {levelup::State::Locked, "not yet"}})}});
    }
    return listed;
}

Json Refused(levelup::Refusal refusal)
{
    if (refusal == levelup::Refusal::None)
    {
        return Json{{"taken", true}};
    }
    return Json{{"taken", false},
                {"refusal", Label(refusal, {{levelup::Refusal::NoCreature, "no party data"},
                                            {levelup::Refusal::NotOffered, "not offered"},
                                            {levelup::Refusal::Held, "had already"},
                                            {levelup::Refusal::Locked, "locked"},
                                            {levelup::Refusal::NoPoints, "no point to spend"}})}};
}

ToolSpec Step(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.listed = false;
    return spec;
}

const Json kIdProperty = {{"type", "string"}, {"description", "The creature, by id."}};
// The creation screen's, where its hero's abilities are offered and taken as
// its pages do (#189); the level-up screen's otherwise.
const Json kChargenProperty = {{"type", "boolean"}, {"description", "As the creation screen does."}};

levelup::Mode ModeOf(const Args &args)
{
    return args.Bool("chargen", false) ? levelup::Mode::CharGen : levelup::Mode::LevelUp;
}

} // namespace

void AddLevelUp(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("levelup.offers",
             "What the level-up screen would offer a creature, with no screen: its `points` to spend, and its "
             "`talents`, `spells` and `skills`, each with `id`, `name` and `state` (has, learnable, not yet), and "
             "its `specializations` (has, learnable, locked, no point to spend). A class has talents or spells, "
             "not both: `abilityPage` names the one it is offered. With `chargen`, as the creation screen "
             "offers them to its hero.",
             Json{{"id", kIdProperty}, {"chargen", kChargenProperty}}, {"id"}, false),
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "levelup.offers");
            const Args args(raw, "levelup.offers");
            const ObjectId id = Creature(args);
            const auto points = levelup::PointsOf(id);
            if (!points)
            {
                throw ToolError(std::format("levelup.offers: {} has no points to read", FormatId(id)));
            }
            Json answer = {{"creature", Brief(id)},
                           {"points",
                            {{"attributes", points->attributes},
                             {"skills", points->skills},
                             {"talentsOrSpells", points->talentsOrSpells},
                             {"specializations", points->specializations}}},
                           {"talents", Offered(id, levelup::Kind::Talents, ModeOf(args))},
                           {"spells", Offered(id, levelup::Kind::Spells, ModeOf(args))},
                           {"skills", Offered(id, levelup::Kind::Skills, ModeOf(args))}};
            // The states as the screen's icons had them (#64): its grey one,
            // with no point left, said so.
            Json specializations = Json::array();
            for (const levelup::Offer &offer : levelup::Specializations(id))
            {
                std::string state = "locked";
                if (offer.state == levelup::State::Have)
                {
                    state = "has";
                }
                else if (offer.state == levelup::State::Learnable)
                {
                    state = points->specializations > 0 ? "learnable" : "no point to spend";
                }
                // The screen's name, its class's in Classes: the ability's own
                // is a hidden one, HIDDEN_BARD (play, 2026-10-24).
                specializations.push_back(Json{{"id", offer.ability},
                                               {"name", Narrow(levelup::SpecializationName(offer.ability))},
                                               {"state", std::move(state)}});
            }
            answer["specializations"] = std::move(specializations);
            // DAOLib gives none of the kind the screen does not show (#71).
            if (!answer["spells"].empty())
            {
                answer["abilityPage"] = "spells";
            }
            else if (!answer["talents"].empty())
            {
                answer["abilityPage"] = "talents";
            }
            return answer;
        });

    bench.Tools().Add(
        Step("levelup.take",
             "Takes an ability as a click on its icon does, or with `specialization` true a specialization as "
             "the screen's choice does. `taken`, or the `refusal`: not offered, had already, locked, no point to "
             "spend.",
             Json{{"id", kIdProperty},
                  {"ability", {{"type", "integer"}, {"description", "The ability's id."}}},
                  {"specialization", {{"type", "boolean"}, {"description", "It is a specialization."}}},
                  {"chargen", kChargenProperty}},
             {"id", "ability"}),
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "levelup.take");
            const Args args(raw, "levelup.take");
            const ObjectId id = Creature(args);
            const auto ability = static_cast<std::int32_t>(args.Integer("ability", 0, 1, INT32_MAX));
            return Refused(args.Bool("specialization", false) ? levelup::TakeSpecialization(id, ability)
                                                              : levelup::Take(id, ability, ModeOf(args)));
        });

    bench.Tools().Add(
        Step("levelup.raise",
             "Raises an attribute, 1 (strength) to 6 (constitution), by `points`, as its plus button does. "
             "`taken`, or the `refusal`.",
             Json{{"id", kIdProperty},
                  {"attribute", {{"type", "integer"}, {"minimum", 1}, {"maximum", 6}}},
                  {"points", {{"type", "integer"}, {"minimum", 1}}}},
             {"id", "attribute", "points"}),
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "levelup.raise");
            const Args args(raw, "levelup.raise");
            const ObjectId id = Creature(args);
            const auto attribute = static_cast<std::int32_t>(args.Integer("attribute", 0, 1, 6));
            const auto points = static_cast<std::int32_t>(args.Integer("points", 0, 1, 999));
            return Refused(levelup::RaiseAttribute(id, attribute, points));
        });

    bench.Tools().Add(Step("levelup.auto",
                           "Spends a creature's points as the screen's Auto Level Up does, a specialization's left.",
                           Json{{"id", kIdProperty}}, {"id"}),
                      [&watch](const Json &raw) -> ToolResult {
                          RequireWorld(watch, "levelup.auto");
                          const Args args(raw, "levelup.auto");
                          levelup::AutoLevelUp(Creature(args));
                          return Json{{"done", true}};
                      });

    bench.Tools().Add(Step("levelup.finish",
                           "Ends a level-up as the screen's Done does. Only with no level-up screen up: its own "
                           "Done does the same.",
                           Json{{"id", kIdProperty}}, {"id"}),
                      [&watch](const Json &raw) -> ToolResult {
                          RequireWorld(watch, "levelup.finish");
                          const Args args(raw, "levelup.finish");
                          levelup::Finish(Creature(args));
                          return Json{{"done", true}};
                      });
}

} // namespace eluvian::game
