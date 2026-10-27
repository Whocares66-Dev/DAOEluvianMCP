#include "core/CharGen.h"

#include "core/Args.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
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

// How long the screen may take to close once ended: a frame or two in play.
constexpr milliseconds kScreenWait{10000};

// The game's origins table: a row race x 1000 + class x 100 + background,
// with the area the origin starts in (dev/CHARGEN.md).
constexpr int kOrigins = 1001;

// PROPERTY_ATTRIBUTE_*, as levelup.raise takes them.
constexpr std::string_view kAttributes[] = {"strength", "dexterity", "willpower", "magic", "cunning", "constitution"};
// The difficulty picker's rows, from 0 (CharGen.as's PromptDifficulty).
constexpr std::string_view kDifficulties[] = {"casual", "normal", "hard", "nightmare"};

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A refusal of the screen's, in words.
std::string Why(const std::string &refusal)
{
    if (refusal == "disabled")
    {
        return "the screen greys it out now";
    }
    if (refusal == "not offered")
    {
        return "the screen offers no such choice";
    }
    if (refusal == "bad name")
    {
        return "the name field takes 1 to 10 letters";
    }
    if (refusal == "points left")
    {
        return "points are left unspent";
    }
    return refusal;
}

class Creating
{
  public:
    Creating(Bench &bench, const Args &args) : bench_(bench), args_(args)
    {
        answer_["cursor"] = bench_.Events().Next();
    }

    Json Run()
    {
        Json offers;
        try
        {
            offers = Offers();
        }
        catch (const ToolError &)
        {
            throw ToolError("chargen: the character creation screen is not up; from the start menu, game_new starts "
                            "a new game and brings it up");
        }
        answer_["hero"] = offers.value("hero", Json());
        if (args_.Bool("offers", false))
        {
            if (args_.Has("race") || args_.Has("class") || args_.Has("background"))
            {
                throw ToolError("chargen: `offers` takes no choices; it lists what the screen offers now, and the "
                                "hero's abilities once a call has chosen a class with `finish`: false");
            }
            return Listed(offers);
        }
        Check();
        Build();
        return answer_;
    }

  private:
    Bench &bench_;
    const Args &args_;
    Json answer_ = Json::object();
    Json chose_ = Json::object();
    int race_ = 0;
    int class_ = 0;
    int background_ = 0;
    std::optional<int> difficulty_;

    Json Offers()
    {
        return bench_.Invoke("chargen.offers", Json::object()).data;
    }

    Json Character()
    {
        return bench_.Invoke("character", Json{{"id", answer_["hero"]}}).data;
    }

    // What the screen offers, and, once the hero has a class, its points and
    // the skills and talents or spells its pages offer.
    Json Listed(const Json &offers)
    {
        for (const char *key : {"genders", "races", "classes", "backgrounds", "voices"})
        {
            answer_[key] = offers.value(key, Json::array());
        }
        const Json hero = Character();
        if (hero.value("class", Json::object()).value("id", 0) == 0)
        {
            answer_["note"] = "the hero has no class yet: chargen with race, class and background and `finish`: "
                              "false chooses them, and then `offers` lists the skills and talents or spells";
            return answer_;
        }
        const Json abilities = bench_.Invoke("levelup.offers", Json{{"id", answer_["hero"]}, {"chargen", true}}).data;
        answer_["pointsToSpend"] = hero.value("pointsToSpend", Json::object());
        for (const char *key : {"skills", "talents", "spells"})
        {
            Json learnable = Json::array();
            for (const auto &ability : abilities.value(key, Json::array()))
            {
                if (ability.value("state", std::string()) != "not yet")
                {
                    learnable.push_back(ability);
                }
            }
            if (!learnable.empty() || std::string_view(key) == "skills")
            {
                answer_[key] = std::move(learnable);
            }
        }
        return answer_;
    }

    // The arguments' shapes, before anything is chosen.
    void Check()
    {
        for (const char *needed : {"race", "class", "background"})
        {
            if (!args_.Has(needed))
            {
                throw ToolError("chargen: give `race`, `class` and `background` (`offers`: true lists what the "
                                "screen offers)");
            }
        }
        const Json attributes = args_.Raw().value("attributes", Json());
        const Json abilities = args_.Raw().value("abilities", Json());
        if (!attributes.is_null() && !attributes.is_object())
        {
            throw ToolError("chargen: `attributes` is an object of points by attribute, as {\"magic\": 5}");
        }
        if (!abilities.is_null() && !abilities.is_array())
        {
            throw ToolError("chargen: `abilities` is a list of ability ids, from `offers`");
        }
        if (args_.Has("difficulty"))
        {
            const std::string asked = Lower(args_.String("difficulty"));
            const auto *found = std::ranges::find(kDifficulties, asked);
            if (found == std::end(kDifficulties))
            {
                throw ToolError("chargen: `difficulty` is casual, normal, hard or nightmare");
            }
            difficulty_ = static_cast<int>(found - std::begin(kDifficulties));
        }
    }

    // A choice of the screen's by its name in any case or its id; the
    // asked-for alias "wizard" is the mage the screen shows.
    static const Json *Find(const Json &choices, const Json &asked)
    {
        std::string name = asked.is_string() ? Lower(asked.get<std::string>()) : std::string();
        name = name == "wizard" ? "mage" : name;
        for (const auto &choice : choices)
        {
            if ((asked.is_number_integer() && choice.value("id", -1) == asked.get<int>()) ||
                (!name.empty() && Lower(choice.value("name", std::string())) == name))
            {
                return &choice;
            }
        }
        // A word of its name, where one choice the screen offers has it: the
        // screen names the backgrounds in full ("City Elf", and "Human Noble"
        // for a dwarf noble too, play 2026-10-27), where an agent writes the
        // origin's word ("city", "noble").
        const Json *only = nullptr;
        for (const auto &choice : choices)
        {
            const std::string full = " " + Lower(choice.value("name", std::string())) + " ";
            if (!name.empty() && choice.value("enabled", false) && full.find(" " + name + " ") != std::string::npos)
            {
                if (only != nullptr)
                {
                    return nullptr;
                }
                only = &choice;
            }
        }
        return only;
    }

    static std::string Enabled(const Json &choices)
    {
        std::string names;
        for (const auto &choice : choices)
        {
            if (choice.value("enabled", false))
            {
                names += (names.empty() ? "" : ", ") + Lower(choice.value("name", std::string()));
            }
        }
        return names.empty() ? "none" : names;
    }

    // One choice made as the screen's buttons make it, from what it offers
    // now: the list read again each time, a race or class changing what the
    // next offers.
    int Select(const char *choice, const char *list, const Json &asked)
    {
        const Json offered = Offers().value(list, Json::array());
        const Json *found = Find(offered, asked);
        if (found == nullptr || !found->value("enabled", false))
        {
            throw ToolError(std::format("chargen: the screen offers no {} {} now; it offers {}{}", choice, asked.dump(),
                                        Enabled(offered), Made()));
        }
        const int id = found->value("id", 0);
        const Json took = bench_.Invoke("chargen.select", Json{{"choice", choice}, {"id", id}}).data;
        if (!took.value("taken", false))
        {
            throw ToolError(std::format("chargen: the {} {} was refused: {}{}", choice, asked.dump(),
                                        Why(took.value("refusal", std::string())), Made()));
        }
        chose_[choice] = Lower(found->value("name", std::string()));
        return id;
    }

    // The voice by its sound set, its name, or its sound set's last word,
    // from those offered for the hero's race and gender; the screen's
    // default, the first, where none is asked.
    void Voice()
    {
        const Json voices = Offers().value("voices", Json::array());
        const std::string asked = Lower(args_.String("voice", ""));
        const Json *found = nullptr;
        std::string offered;
        for (const auto &voice : voices)
        {
            const std::string set = Lower(voice.value("soundSet", std::string()));
            const std::string word = set.substr(set.find_last_of('_') + 1);
            if (found == nullptr &&
                (asked.empty() || asked == set || asked == word || asked == Lower(voice.value("name", std::string()))))
            {
                found = &voice;
            }
            offered += (offered.empty() ? "" : ", ") + word;
        }
        if (found == nullptr)
        {
            throw ToolError(std::format("chargen: no voice \"{}\" for this hero; the screen offers {}{}", asked,
                                        offered.empty() ? "none" : offered, Made()));
        }
        const std::string set = found->value("soundSet", std::string());
        const Json took = bench_.Invoke("chargen.select", Json{{"choice", "voice"}, {"soundSet", set}}).data;
        if (!took.value("taken", false))
        {
            throw ToolError("chargen: the voice " + set + " was refused: " + Why(took.value("refusal", std::string())) +
                            Made());
        }
        chose_["voice"] = set;
    }

    // What a refusal leaves: the choices made before it stand, on the
    // screen, which stays up.
    std::string Made() const
    {
        return chose_.empty() ? "; nothing was chosen" : "; those chosen before it stand and the screen is up";
    }

    Json Hero()
    {
        const Json hero = Character();
        Json attributes = Json::object();
        // Held, not iterated as value()'s temporary, which ends before the loop's body.
        const Json totals = hero.value("attributes", Json::object());
        for (const auto &[name, value] : totals.items())
        {
            attributes[name] = value.value("total", 0.0);
        }
        return Json{{"name", hero.value("name", "")},
                    {"gender", hero.value("gender", "")},
                    {"race", hero.value("race", Json::object()).value("label", "")},
                    {"class", hero.value("class", Json::object()).value("label", "")},
                    {"level", hero.value("level", 0)},
                    {"attributes", attributes},
                    {"pointsToSpend", hero.value("pointsToSpend", Json::object())},
                    {"abilityCount", hero.value("abilityCount", 0)}};
    }

    void Build()
    {
        const Json attributes = args_.Raw().value("attributes", Json());
        const Json abilities = args_.Raw().value("abilities", Json());
        Select("gender", "genders", args_.Raw().value("gender", Json("male")));
        race_ = Select("race", "races", args_.Raw().at("race"));
        class_ = Select("class", "classes", args_.Raw().at("class"));
        background_ = Select("background", "backgrounds", args_.Raw().at("background"));
        Voice();
        const bool spending = !attributes.is_null() || !abilities.is_null();
        if (spending)
        {
            Spend(attributes.is_null() ? Json::object() : attributes, abilities.is_null() ? Json::array() : abilities);
        }
        if (args_.Has("name"))
        {
            const std::string name = args_.String("name");
            const Json took = bench_.Invoke("chargen.name", Json{{"name", name}}).data;
            if (!took.value("taken", false))
            {
                throw ToolError("chargen: the name \"" + name +
                                "\" was refused: " + Why(took.value("refusal", std::string())) + Made());
            }
            chose_["name"] = name;
        }
        answer_["chose"] = chose_;
        answer_["exit"] = spending ? "full" : "quick play";
        const int row = (race_ * 1000) + (class_ * 100) + background_;
        const Json start = bench_
                               .Invoke("script", Json{{"command", "GetM2DAString"},
                                                      {"args", Json::array({kOrigins, "StartArea", row})}})
                               .data.value("value", Json());
        answer_["origin"] = Json{{"row", row}, {"startArea", start.is_string() ? start : Json("")}};
        answer_["after"] = Hero();
        if (args_.Bool("finish", true))
        {
            Finish(spending);
        }
        else
        {
            answer_["finished"] = false;
            answer_["next"] = "the screen is up with these choices: chargen again with `finish` true (the choices "
                              "are made again from the start), or `offers` for the abilities";
        }
    }

    // The points, as the screen's pages spend them: every one, since its
    // Next stays off while any is left, or none, as its Quick Play.
    void Spend(const Json &attributes, const Json &abilities)
    {
        const Json points = Character().value("pointsToSpend", Json::object());
        int raising = 0;
        for (const auto &[name, amount] : attributes.items())
        {
            if (std::ranges::find(kAttributes, name) == std::end(kAttributes))
            {
                throw ToolError("chargen: `attributes` names strength, dexterity, willpower, magic, cunning or "
                                "constitution");
            }
            if (!amount.is_number_integer() || amount.get<int>() < 1)
            {
                throw ToolError("chargen: each of `attributes` takes 1 point or more");
            }
            raising += amount.get<int>();
        }
        const int attributePoints = points.value("attributes", 0);
        const int abilityPoints = points.value("skills", 0) + points.value("talentsOrSpells", 0);
        if (raising != attributePoints || std::cmp_not_equal(abilities.size(), abilityPoints))
        {
            throw ToolError(std::format(
                "chargen: the screen spends every point or, as its Quick Play, none: this hero has {} attribute "
                "points and {} for skills and talents or spells (`pointsToSpend`), and {} and {} were given{}",
                attributePoints, abilityPoints, raising, abilities.size(), Made()));
        }
        for (const auto &[name, amount] : attributes.items())
        {
            const auto at = std::ranges::find(kAttributes, name) - std::begin(kAttributes) + 1;
            const Json took =
                bench_.Invoke("levelup.raise", Json{{"id", answer_["hero"]}, {"attribute", at}, {"points", amount}})
                    .data;
            if (!took.value("taken", false))
            {
                throw ToolError("chargen: " + name + " was not raised: " + took.value("refusal", std::string()) +
                                Made());
            }
        }
        Json taken = Json::array();
        for (const auto &ability : abilities)
        {
            if (!ability.is_number_integer())
            {
                throw ToolError("chargen: each of `abilities` is an ability's id, a number, from `offers`");
            }
            const Json took =
                bench_.Invoke("levelup.take", Json{{"id", answer_["hero"]}, {"ability", ability}, {"chargen", true}})
                    .data;
            if (!took.value("taken", false))
            {
                throw ToolError(std::format("chargen: ability {} was not taken: {} (`offers` lists what is learnable "
                                            "now){}",
                                            ability.get<int>(), took.value("refusal", std::string()), Made()));
            }
            taken.push_back(ability);
        }
        answer_["taken"] = taken;
    }

    // The end as the screen's Play buttons end it, the origin starting once
    // the screen has closed.
    void Finish(bool full)
    {
        Json asked = {{"exit", full ? "full" : "quick"}};
        if (difficulty_)
        {
            asked["difficulty"] = *difficulty_;
        }
        const Json took = bench_.Invoke("chargen.finish", asked).data;
        if (!took.value("taken", false))
        {
            throw ToolError("chargen: the screen was not ended: " + Why(took.value("refusal", std::string())) + Made());
        }
        const auto until = Clock::now() + kScreenWait;
        while (Clock::now() < until)
        {
            const Json screens = bench_.Invoke("gui", Json::object()).data.value("screens", Json::array());
            if (std::ranges::none_of(screens, [](const Json &s) { return s.value("name", "") == "CharGen"; }))
            {
                answer_["finished"] = true;
                answer_["next"] = "the origin's opening follows (a cutscene, then a conversation): wait_ready, or "
                                  "wait_for eluvian.mode.changed";
                return;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        answer_["finished"] = false;
        answer_["note"] = "the creation screen was still up 10 s after it was ended; gui says what is up";
    }
};

} // namespace

void AddCharGenTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{
            "chargen",
            "Builds the hero on the game's character creation screen, as a player's clicks do, so the game's own "
            "rules and scripts apply; then ends the screen, and the origin starts. The screen must be up (`gui` "
            "shows CharGen, mode chargen): `game_new` starts a new game and brings it up. Pass `race`, `class` "
            "and `background` (and optionally `gender`, `voice`, `name`, `difficulty`), and either both "
            "`attributes` and `abilities` spending every point, or neither, and the game spends them as the "
            "screen's Quick Play does. Each choice is made as the screen makes it, from what it offers then: a "
            "background must suit the race and class, and one the screen greys out is refused, naming those it "
            "offers. `offers`: true lists what it offers now.\n\n"
            "With `finish`: false the choices are made and the screen stays up; `offers` then also lists the "
            "skills and the talents or spells to choose from, and a later call (with every choice again) "
            "finishes. The answer has the `hero`, what it `chose`, the `exit` (full or quick play), the hero "
            "`after` (attributes, points left, ability count), the `origin` (its row of the origins table and "
            "its `startArea`), whether it `finished`, and a `cursor`. The origin's opening follows: a cutscene, "
            "then a conversation.",
            Json{{"type", "object"},
                 {"properties",
                  {{"race", {{"description", "human, elf or dwarf, as the screen names them (or the race's id)."}}},
                   {"class", {{"description", "warrior, mage or rogue (or the class's id)."}}},
                   {"background",
                    {{"description", "The origin's background by name, as `offers` lists them (noble, magi, dalish, "
                                     "city, commoner in the game as shipped), or its id."}}},
                   {"gender", {{"description", "male or female. Default male."}}},
                   {"voice",
                    {{"type", "string"},
                     {"description", "The voice: its sound set (ss_pc_mhuman_wise), its name, or its sound set's "
                                     "last word (wise, cocky ...), from those `offers` lists for the hero's race and "
                                     "gender. Default the screen's own, the first."}}},
                   {"name",
                    {{"type", "string"},
                     {"description", "The hero's name, 1 to 10 letters. Default the origin's own (Aedan for a human "
                                     "noble man), as the screen offers it."}}},
                   {"attributes",
                    {{"type", "object"}, {"description", R"(Points by attribute, every one spent, as {"magic": 5}.)"}}},
                   {"abilities",
                    {{"type", "array"},
                     {"description", "Ability ids to take, every point spent: the skills and the talents or spells "
                                     "`offers` lists as learnable, the first of a chain before the next."}}},
                   {"difficulty",
                    {{"type", "string"},
                     {"description", "casual, normal, hard or nightmare, as the screen asks at its end. It is the "
                                     "game's option, kept in its profile's settings. Default: left as it is."}}},
                   {"finish",
                    {{"type", "boolean"},
                     {"description", "End the screen once the hero is built, and so start the origin. Default "
                                     "true."}}},
                   {"offers",
                    {{"type", "boolean"},
                     {"description", "List what the screen offers now (genders, races, classes, backgrounds, "
                                     "voices, each enabled or not), and, once a class is chosen, the hero's points "
                                     "and the abilities to choose from. Chooses nothing."}}}}}},
            Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "chargen");
            return Creating(bench, args).Run();
        });
}

} // namespace eluvian
