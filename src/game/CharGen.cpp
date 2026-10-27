// Character creation's steps through DAOLib's dao::chargen (DAOLib #259,
// c3ced45, lib-agent-3, 2026-10-27): what the creation screen offers, each
// choice as it shows it, a choice made as its buttons make it and refused
// where it greys it out, the name, and the screen ended as its Play buttons
// end it. core's chargen asks these, unlisted, where it sent the GUICharGen
// texts itself, read the game's tables by script, named the hero through the
// movie and ended the movie by setting its exit state (#153, #189).
#include "game/Shared.h"

#include "dao/CharGen.h"

namespace eluvian::game
{

namespace
{

namespace chargen = dao::chargen;

Json ChoicesJson(const std::vector<chargen::Choice> &choices)
{
    Json list = Json::array();
    for (const chargen::Choice &choice : choices)
    {
        list.push_back(Json{
            {"id", choice.id}, {"name", Narrow(choice.name)}, {"enabled", choice.enabled}, {"chosen", choice.chosen}});
    }
    return list;
}

Json Taken(chargen::Refusal refusal)
{
    if (refusal == chargen::Refusal::None)
    {
        return Json{{"taken", true}};
    }
    return Json{{"taken", false},
                {"refusal", Label(refusal, {{chargen::Refusal::NoScreen, "no screen"},
                                            {chargen::Refusal::NotOffered, "not offered"},
                                            {chargen::Refusal::Disabled, "disabled"},
                                            {chargen::Refusal::BadName, "bad name"},
                                            {chargen::Refusal::PointsLeft, "points left"}})}};
}

ToolSpec Step(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.listed = false;
    return spec;
}

} // namespace

void AddCharGen(Bench &bench)
{
    bench.Tools().Add(
        Read("chargen.offers",
             "What the character creation screen offers, each as it shows it: the `hero` it makes, and its "
             "`genders`, `races`, `classes` and `backgrounds` (id, name, enabled, chosen), and the `voices` for the "
             "hero's race and gender (name, soundSet), its default first. Refused with no creation screen up.",
             Json::object(), {}, false),
        [](const Json &) -> ToolResult {
            const auto hero = chargen::Hero();
            if (!hero)
            {
                throw ToolError("chargen.offers: the character creation screen is not up");
            }
            Json voices = Json::array();
            for (const chargen::Voice &voice : chargen::Voices())
            {
                voices.push_back(Json{{"name", Narrow(voice.name)}, {"soundSet", Narrow(voice.soundSet)}});
            }
            return Json{{"hero", IdJson(*hero)},
                        {"genders", ChoicesJson(chargen::Genders())},
                        {"races", ChoicesJson(chargen::Races())},
                        {"classes", ChoicesJson(chargen::Classes())},
                        {"backgrounds", ChoicesJson(chargen::Backgrounds())},
                        {"voices", std::move(voices)}};
        });

    bench.Tools().Add(
        Step("chargen.select",
             "Makes a choice as the screen's buttons do: `gender`, `race`, `class` or `background` by its `id`, or "
             "`voice` by its `soundSet`. `taken`, or the `refusal` (no screen, not offered, disabled).",
             Json{{"choice", {{"type", "string"}, {"enum", {"gender", "race", "class", "background", "voice"}}}},
                  {"id", {{"type", "integer"}}},
                  {"soundSet", {{"type", "string"}}}},
             {"choice"}),
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "chargen.select");
            const auto choice = args.Choice("choice", {"gender", "race", "class", "background", "voice"});
            if (choice == "voice")
            {
                return Taken(chargen::SelectVoice(Wide(args.String("soundSet"))));
            }
            const auto id = static_cast<std::int32_t>(args.Integer("id", 0, 0, 1 << 20));
            if (choice == "gender")
            {
                return Taken(chargen::SelectGender(id));
            }
            if (choice == "race")
            {
                return Taken(chargen::SelectRace(id));
            }
            return Taken(choice == "class" ? chargen::SelectClass(id) : chargen::SelectBackground(id));
        });

    bench.Tools().Add(Step("chargen.name", "Names the hero as the screen's name field does: `taken`, or the `refusal`.",
                           Json{{"name", {{"type", "string"}}}}, {"name"}),
                      [](const Json &raw) -> ToolResult {
                          const Args args(raw, "chargen.name");
                          return Taken(chargen::SetName(Wide(args.String("name"))));
                      });

    bench.Tools().Add(
        Step("chargen.finish",
             "Ends the screen as its Play buttons do: `exit` quick (the game spends the points) or full (every "
             "point spent), with `difficulty`, a row of the difficulty picker from 0 (Casual), or none to keep the "
             "game's. `taken`, or the `refusal` (disabled before a background, points left, not offered).",
             Json{{"exit", {{"type", "string"}, {"enum", {"quick", "full"}}}}, {"difficulty", {{"type", "integer"}}}},
             {"exit"}),
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "chargen.finish");
            const auto exit =
                args.Choice("exit", {"quick", "full"}) == "quick" ? chargen::Exit::Quick : chargen::Exit::Full;
            std::optional<std::int32_t> difficulty;
            if (args.Has("difficulty"))
            {
                difficulty = static_cast<std::int32_t>(args.Integer("difficulty", 0, 0, 255));
            }
            return Taken(chargen::Finish(exit, difficulty));
        });
}

} // namespace eluvian::game
