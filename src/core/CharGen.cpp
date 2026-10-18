#include "core/CharGen.h"

#include "core/Args.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
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

// The game's tables the screen reads (dev/CHARGEN.md): the backgrounds, which
// race and class may take each by a column of each race's and each class's
// name; the origins, a row race x 1000 + class x 100 + background with its
// start and default names; the player's sound sets.
constexpr int kBackgrounds = 97;
constexpr int kOrigins = 1001;
constexpr int kVoices = 212;

struct Named
{
    const char *name;
    int id;
    const char *column; // its column in the backgrounds' table
};

// The ids are the scripts' (GENDER_*, RACE_*, CLASS_*), as the screen sends
// them; the columns are the backgrounds table's (read with ghilana,
// 2026-10-18). Wizard is the class's own label, mage what the game shows.
constexpr Named kGenders[] = {{"male", 1, ""}, {"female", 2, ""}};
constexpr Named kRaces[] = {{"dwarf", 1, "Dwarf"}, {"elf", 2, "Elf"}, {"human", 3, "Human"}};
constexpr Named kClasses[] = {
    {"warrior", 1, "Warrior"}, {"mage", 2, "Mage"}, {"wizard", 2, "Mage"}, {"rogue", 3, "Rogue"}};
// PROPERTY_ATTRIBUTE_*, as the screen's CharGenAssignAttribute sends them.
constexpr Named kAttributes[] = {{"strength", 1, ""}, {"dexterity", 2, ""}, {"willpower", 3, ""},
                                 {"magic", 4, ""},    {"cunning", 5, ""},   {"constitution", 6, ""}};
// GameOptions.DifficultyLevel, by the screen's picker's rows (CharGen.as's
// PromptDifficulty).
constexpr const char *kDifficulties[] = {"casual", "normal", "hard", "nightmare"};

// The engine's types for a creature's chains of abilities, as the screen
// asks for them (GameModeController.FindAbilityChains <creature> <type> 1),
// and where it then finds them in the data tree; DisplayState 1 held, 2
// available, 3 locked (chargen.gfx's Ability.as).
struct Chains
{
    int type;
    const char *list;
};
constexpr Chains kSkills{3, "CharGenCreature.Stats.SkillChains"};
constexpr Chains kTalents{1, "CharGenCreature.Stats.TalentChains"};
constexpr Chains kSpells{2, "CharGenCreature.Stats.SpellChains"};
constexpr int kAvailable = 2;
constexpr int kMostChains = 128;

// The movie's exits (CharGen.as): BASIC is its Quick Play, after which
// sys_chargen spends the points as an auto level-up does; ADVANCED the run
// through every page.
constexpr int kExitQuickPlay = 1;
constexpr int kExitFull = 2;

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Names(std::span<const Named> list)
{
    std::string out;
    for (const auto &each : list)
    {
        out += out.empty() ? "" : ", ";
        out += each.name;
    }
    return out;
}

const Named &Choose(const Json &value, std::span<const Named> list, std::string_view what)
{
    for (const auto &each : list)
    {
        if ((value.is_string() && Lower(value.get<std::string>()) == each.name) ||
            (value.is_number_integer() && value.get<int>() == each.id))
        {
            return each;
        }
    }
    throw ToolError(std::format("chargen: `{}` is one of {}", what, Names(list)));
}

// A background as asked: its id, or its label in the backgrounds' table.
struct Background
{
    int id = 0;
    std::string label;
    bool race = false;  // the hero's race may take it
    bool klass = false; // and its class
};

class Creating
{
  public:
    Creating(Bench &bench, const Args &args) : bench_(bench), args_(args)
    {
        answer_["cursor"] = bench_.Events().Next();
    }

    Json Run()
    {
        const Json gui = bench_.Invoke("gui", Json::object()).data;
        const Json screens = gui.value("screens", Json::array());
        const bool up = std::ranges::any_of(screens, [](const Json &s) { return s.value("name", "") == "CharGen"; });
        if (gui.value("mode", "") != "chargen" || !up)
        {
            throw ToolError("chargen: the character creation screen is not up (mode " + gui.value("mode", "?") +
                            "); from the start menu, game_new starts a new game and brings it up");
        }
        hero_ = static_cast<std::uint32_t>(Number("CharGenCreature.ID"));
        if (hero_ == 0)
        {
            throw ToolError("chargen: the creation screen has no hero yet (CharGenCreature.ID is 0); call again in "
                            "a moment");
        }
        answer_["hero"] = std::format("0x{:x}", hero_);
        if (args_.Bool("offers", false))
        {
            if (args_.Has("race") || args_.Has("class") || args_.Has("background"))
            {
                throw ToolError("chargen: `offers` takes no choices; it lists the backgrounds, and the hero's "
                                "abilities once a call has chosen a class with `finish`: false");
            }
            return Offers();
        }
        Choices();
        Build();
        return answer_;
    }

  private:
    Bench &bench_;
    const Args &args_;
    Json answer_ = Json::object();
    std::uint32_t hero_ = 0;
    int gender_ = 1;
    const Named *race_ = nullptr;
    const Named *class_ = nullptr;
    Background background_;
    std::string voice_;
    Json sent_ = Json::array();

    Json Data(const std::string &path)
    {
        return bench_.Invoke("data", Json{{"path", path}}).data;
    }

    double Number(const std::string &path)
    {
        const Json read = Data(path);
        return read.value("found", false) ? read.value("number", 0.0) : 0.0;
    }

    Json Script(const char *command, const Json &args)
    {
        return bench_.Invoke("script", Json{{"command", command}, {"args", args}}).data.value("value", Json());
    }

    int TableInt(int table, const char *column, int row)
    {
        const Json value = Script("GetM2DAInt", Json::array({table, column, row}));
        return value.is_number_integer() ? value.get<int>() : 0;
    }

    std::string TableString(int table, const char *column, int row)
    {
        const Json value = Script("GetM2DAString", Json::array({table, column, row}));
        return value.is_string() ? value.get<std::string>() : std::string();
    }

    // Each row's id of a table, in its order.
    std::vector<int> Rows(int table)
    {
        const Json count = Script("GetM2DARows", Json::array({table}));
        std::vector<int> rows;
        for (int i = 0; i < (count.is_number_integer() ? count.get<int>() : 0); ++i)
        {
            const Json id = Script("GetM2DARowIdFromRowIndex", Json::array({table, i}));
            rows.push_back(id.is_number_integer() ? id.get<int>() : 0);
        }
        return rows;
    }

    void Send(const std::string &text)
    {
        const Json answer = bench_.Invoke("input", Json{{"message", text}}).data;
        sent_.push_back(text);
        if (!answer.value("sent", false))
        {
            throw ToolError("chargen: the game took no \"" + text + "\" (`sent`); the screen is left as it is");
        }
    }

    // Every background with whether the hero's race and class may take it,
    // from the game's table, as the screen enables its buttons.
    std::vector<Background> Backgrounds()
    {
        std::vector<Background> out;
        for (const int row : Rows(kBackgrounds))
        {
            const std::string label = TableString(kBackgrounds, "Label", row);
            if (row == 0 || label.empty())
            {
                continue;
            }
            Background each{row, label};
            each.race = race_ == nullptr || TableInt(kBackgrounds, race_->column, row) == 1;
            each.klass = class_ == nullptr || TableInt(kBackgrounds, class_->column, row) == 1;
            out.push_back(each);
        }
        return out;
    }

    Json BackgroundsJson()
    {
        Json list = Json::array();
        for (const auto &each : Backgrounds())
        {
            Json races = Json::array();
            for (const auto &race : kRaces)
            {
                if (TableInt(kBackgrounds, race.column, each.id) == 1)
                {
                    races.push_back(race.name);
                }
            }
            Json classes = Json::array();
            for (const auto &klass : kClasses)
            {
                if (klass.id != 2 || std::string_view(klass.name) == "mage")
                {
                    if (TableInt(kBackgrounds, klass.column, each.id) == 1)
                    {
                        classes.push_back(klass.name);
                    }
                }
            }
            list.push_back(Json{{"id", each.id}, {"name", Lower(each.label)}, {"races", races}, {"classes", classes}});
        }
        return list;
    }

    Json Offers()
    {
        answer_["genders"] = Json::array({"male", "female"});
        answer_["races"] = Json::array({"human", "elf", "dwarf"});
        answer_["classes"] = Json::array({"warrior", "mage", "rogue"});
        answer_["backgrounds"] = BackgroundsJson();
        const Json hero = bench_.Invoke("character", Json{{"id", answer_["hero"]}}).data;
        const int klass = hero.value("class", Json::object()).value("id", 0);
        if (klass != 0)
        {
            answer_["pointsToSpend"] = hero.value("pointsToSpend", Json::object());
            answer_["skills"] = ChainJson(kSkills);
            answer_[klass == 2 ? "spells" : "talents"] = ChainJson(klass == 2 ? kSpells : kTalents);
        }
        else
        {
            answer_["note"] = "the hero has no class yet: chargen with race, class and background and `finish`: "
                              "false chooses them, and then `offers` lists the skills and talents or spells";
        }
        return answer_;
    }

    struct Ability
    {
        int id;
        std::string name;
        int state;
    };

    std::vector<Ability> ChainOf(const Chains &chains)
    {
        Send(std::format("GameModeController.FindAbilityChains {} {} 1", hero_, chains.type));
        const Json members = bench_.Invoke("data", Json{{"path", chains.list}, {"members", true}}).data;
        const int count = std::min(members.value("count", 0), kMostChains);
        std::vector<Ability> out;
        // `paths` reads up to 64 at a time: 21 chains of three fields.
        constexpr int kChunk = 21;
        for (int from = 0; from < count; from += kChunk)
        {
            Json paths = Json::array();
            for (int i = from; i < std::min(count, from + kChunk); ++i)
            {
                const std::string at = std::format("{}.{}.", chains.list, i);
                paths.push_back(at + "DisplayState");
                paths.push_back(at + "AbilityTemplate.ID");
                paths.push_back(at + "AbilityTemplate.Name");
            }
            const Json values = bench_.Invoke("data", Json{{"paths", paths}}).data.value("values", Json::array());
            for (std::size_t k = 0; k + 2 < values.size(); k += 3)
            {
                if (values[k].value("found", false))
                {
                    out.push_back(Ability{static_cast<int>(values[k + 1].value("number", 0.0)),
                                          Lower(values[k + 2].value("text", "")),
                                          static_cast<int>(values[k].value("number", 0.0))});
                }
            }
        }
        return out;
    }

    Json ChainJson(const Chains &chains)
    {
        Json list = Json::array();
        for (const auto &each : ChainOf(chains))
        {
            if (each.state == 1 || each.state == kAvailable)
            {
                list.push_back(
                    Json{{"id", each.id}, {"name", each.name}, {"state", each.state == 1 ? "has" : "learnable"}});
            }
        }
        return list;
    }

    // The choices read and checked against the game's tables before
    // anything is sent.
    void Choices()
    {
        for (const char *needed : {"race", "class", "background"})
        {
            if (!args_.Has(needed))
            {
                throw ToolError("chargen: give `race`, `class` and `background` (`offers`: true lists the "
                                "backgrounds and which race and class may take each)");
            }
        }
        gender_ = args_.Has("gender") ? Choose(args_.Raw().at("gender"), kGenders, "gender").id : 1;
        race_ = &Choose(args_.Raw().at("race"), kRaces, "race");
        class_ = &Choose(args_.Raw().at("class"), kClasses, "class");
        const Json asked = args_.Raw().at("background");
        const auto backgrounds = Backgrounds();
        const auto found = std::ranges::find_if(backgrounds, [&](const Background &b) {
            return (asked.is_string() && Lower(asked.get<std::string>()) == Lower(b.label)) ||
                   (asked.is_number_integer() && asked.get<int>() == b.id);
        });
        std::string fits;
        for (const auto &b : backgrounds)
        {
            if (b.race && b.klass)
            {
                fits += (fits.empty() ? "" : ", ") + Lower(b.label);
            }
        }
        if (found == backgrounds.end() || !found->race || !found->klass)
        {
            throw ToolError(std::format("chargen: {} {} {} may take the background{} {}; nothing was sent",
                                        race_->id == 2 ? "an" : "a", race_->name, class_->name,
                                        fits.find(',') == std::string::npos ? "" : "s", fits.empty() ? "none" : fits));
        }
        background_ = *found;
        Voice();
    }

    // The voice: a sound set of the player's table, by its name or the last
    // word of one for the hero's race and gender; the screen's default is
    // the first such row, Wise for every race and gender in the game's table.
    void Voice()
    {
        const std::string asked = Lower(args_.String("voice", "wise"));
        const std::string own = std::format("ss_pc_{}{}_", gender_ == 1 ? "m" : "f", race_->name);
        voice_ = asked.starts_with("ss_") ? asked : own + asked;
        std::string offered;
        for (const int row : Rows(kVoices))
        {
            const std::string set = Lower(TableString(kVoices, "SoundSet", row));
            if (set == voice_)
            {
                return;
            }
            if (set.starts_with(own))
            {
                offered += (offered.empty() ? "" : ", ") + set.substr(own.size());
            }
        }
        throw ToolError(std::format("chargen: the game has no sound set {}; a {} {}'s are {}; nothing was sent", voice_,
                                    gender_ == 1 ? "male" : "female", race_->name, offered.empty() ? "none" : offered));
    }

    int Row() const
    {
        return (race_->id * 1000) + (class_->id * 100) + background_.id;
    }

    Json Hero()
    {
        const Json hero = bench_.Invoke("character", Json{{"id", answer_["hero"]}}).data;
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

    // The movie's background box, shown as the screen opens, closed by its
    // Close: left up, it stayed over the origin's opening (2026-10-18). gui
    // reads a box a movie shows and press closes it as a click does since
    // DAOLib #173; until then this read and hid the popup layer's own clip.
    // A box asking a choice is not this one's to answer.
    void CloseBox()
    {
        const Json popup = bench_.Invoke("gui", Json::object()).data.value("popup", Json());
        if (!popup.is_object())
        {
            return;
        }
        const std::string text = popup.value("text", "");
        const Json buttons = popup.value("buttons", Json::array());
        if (buttons.size() != 1)
        {
            throw ToolError(std::format("chargen: a box asking a choice is up (\"{}\"); press answers it, and "
                                        "nothing was sent",
                                        text));
        }
        answer_["closedBox"] = text;
        bench_.Invoke("press", Json{{"button", 0}});
    }

    void Build()
    {
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
        CloseBox();
        // The movie's own start goes out with creature 0, before its
        // observer of CharGenCreature.ID is answered: the hero kept 0 in
        // every attribute until it was sent again with the id (2026-10-18).
        Send(std::format("GUICharGen.CharGenStart {} 0", hero_));
        Send(std::format("GUICharGen.CharGenSelectGender {} 0 {} 1", hero_, gender_));
        Send(std::format("GUICharGen.CharGenSelectRace {} 0 {} 1", hero_, race_->id));
        Send(std::format("GUICharGen.CharGenSelectClass {} 0 {} 1", hero_, class_->id));
        Send(std::format("GUICharGen.CharGenSelectBackground {} 0 {} 1", hero_, background_.id));
        Send(std::format("GUICharGen.CharGenSelectAppearanceAndSoundSet {} 0 {}", hero_, voice_));
        answer_["chose"] = Json{{"gender", gender_ == 1 ? "male" : "female"},
                                {"race", race_->name},
                                {"class", class_->id == 2 ? "mage" : class_->name},
                                {"background", Lower(background_.label)},
                                {"voice", voice_}};
        const Json points = Hero()["pointsToSpend"];
        const bool spending = !attributes.is_null() || !abilities.is_null();
        if (spending)
        {
            Spend(attributes.is_null() ? Json::object() : attributes, abilities.is_null() ? Json::array() : abilities,
                  points);
        }
        Name();
        if (args_.Has("difficulty"))
        {
            const std::string asked = Lower(args_.String("difficulty"));
            const auto *found = std::ranges::find(kDifficulties, asked);
            if (found == std::end(kDifficulties))
            {
                throw ToolError("chargen: `difficulty` is casual, normal, hard or nightmare");
            }
            Send(std::format("ClientOptions.SetClientOptionMessage GameOptions.DifficultyLevel {}",
                             found - std::begin(kDifficulties)));
            Send("ClientOptions.CommitOptions");
        }
        answer_["exit"] = spending ? "full" : "quick play";
        answer_["origin"] = Json{{"row", Row()}, {"startArea", TableString(kOrigins, "StartArea", Row())}};
        answer_["after"] = Hero();
        if (args_.Bool("finish", true))
        {
            Finish(spending ? kExitFull : kExitQuickPlay);
        }
        else
        {
            answer_["finished"] = false;
            answer_["next"] = "the screen is up with these choices: chargen again with `finish` true (the choices "
                              "are sent again from the start), or `offers` for the abilities";
        }
        answer_["sent"] = sent_;
    }

    // The points, as the screen's pages spend them: every one, since its
    // Next stays off while any is left.
    void Spend(const Json &attributes, const Json &abilities, const Json &points)
    {
        Count(attributes, abilities, points);
        for (const auto &[name, amount] : attributes.items())
        {
            const int id = Choose(Json(name), kAttributes, "attributes").id;
            for (int i = 0; i < amount.get<int>(); ++i)
            {
                Send(std::format("GUICharGen.CharGenAssignAttribute {} {} 1", hero_, id));
            }
        }
        Json taken = Json::array();
        for (const auto &ability : abilities)
        {
            taken.push_back(Take(ability));
        }
        answer_["taken"] = taken;
    }

    // The points given against the hero's, before any is spent.
    void Count(const Json &attributes, const Json &abilities, const Json &points)
    {
        int raising = 0;
        for (const auto &[name, amount] : attributes.items())
        {
            (void)Choose(Json(name), kAttributes, "attributes");
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
                "chargen: the screen spends every point or, as its Quick Play, none: this {} has {} attribute "
                "points and {} for skills and {} (`pointsToSpend`), and {} and {} were given; the choices are "
                "made and the screen is up",
                class_->id == 2 ? "mage" : class_->name, attributePoints, abilityPoints,
                class_->id == 2 ? "spells" : "talents", raising, abilities.size()));
        }
    }

    // One ability taken as its icon takes it, if the screen shows it
    // learnable now: the chains asked again before each, as the screen asks
    // after each pick, so the first of a chain opens the next.
    Json Take(const Json &ability)
    {
        if (!ability.is_number_integer())
        {
            throw ToolError("chargen: each of `abilities` is an ability's id, a number, from `offers`");
        }
        const int id = ability.get<int>();
        std::optional<Ability> found;
        std::string learnable;
        for (const Chains *chains : {&kSkills, class_->id == 2 ? &kSpells : &kTalents})
        {
            for (const auto &each : ChainOf(*chains))
            {
                found = each.id == id ? std::optional<Ability>(each) : found;
                if (each.state == kAvailable)
                {
                    learnable += std::format("{}{} {}", learnable.empty() ? "" : ", ", each.id, each.name);
                }
            }
        }
        if (!found || found->state != kAvailable)
        {
            throw ToolError(std::format("chargen: ability {} is not learnable now; learnable: {}; the screen is up "
                                        "with those before it taken",
                                        id, learnable.empty() ? "none" : learnable));
        }
        Send(std::format("GUICharGen.CharGenAssignAbility {} {} 0", hero_, id));
        return Json{{"id", id}, {"name", found->name}};
    }

    // The name as the screen gives it: written to GUICharGen.TempCharName
    // (the movie's own SetValueUnicode, an FSCommand no message reaches),
    // which CharGenSelectName reads. With none, the origin's own default
    // name, as the screen's field starts with it.
    void Name()
    {
        std::string name = args_.String("name", "");
        if (name.empty())
        {
            const int strref = TableInt(kOrigins, gender_ == 1 ? "DefaultNameMale" : "DefaultNameFemale", Row());
            const Json text = Script("GetStringByStringId", Json::array({strref}));
            name = text.is_string() && !text.get<std::string>().empty() ? text.get<std::string>() : "Hero";
        }
        const Json set = bench_
                             .Invoke("movie", Json{{"screen", "CharGen"},
                                                   {"call", "ExternalCommands.SetValueUnicode"},
                                                   {"args", Json::array({"GUICharGen.TempCharName", name})}})
                             .data;
        if (!set.value("ran", false))
        {
            throw ToolError("chargen: the screen's movie did not take the name; the choices are made and the "
                            "screen is up");
        }
        Send(std::format("GUICharGen.CharGenSelectName {}", hero_));
        answer_["chose"]["name"] = name;
    }

    // The end as the movie ends it: its exit state set, then the screen
    // hidden, whose FinishScene sends CharGenEnd as it closes. A hide with
    // the exit state unset closed the screen with no CharGenEnd, and the
    // game crashed under DragonAgeGUISystem a moment after (2026-10-18).
    void Finish(int exit)
    {
        for (const auto &[path, value] : {std::pair<const char *, Json>{"CharGen.CreatureID", hero_},
                                          {"CharGen.nExitPoint", exit},
                                          {"CharGen.bUserExit", true}})
        {
            const Json set = bench_.Invoke("movie", Json{{"screen", "CharGen"}, {"set", path}, {"value", value}}).data;
            if (!set.value("done", false) || !set.value("existed", false))
            {
                throw ToolError(std::string("chargen: the screen's movie has no ") + path +
                                "; it was not ended, the choices are made and it is up");
            }
        }
        Send("GUI.HideFullScreenGUI");
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
            "screen's Quick Play does. A background must suit the race and class, as the screen allows: `offers`: "
            "true lists which. Choices are checked before anything is sent.\n\n"
            "With `finish`: false the choices are made and the screen stays up; `offers` then also lists the "
            "skills and the talents or spells to choose from, and a later call (with every choice again) "
            "finishes. The answer has the `hero`, what it `chose`, the `exit` (full or quick play), the hero "
            "`after` (attributes, points left, ability count), the `origin` (its row of the origins table and "
            "its `startArea`), whether it `finished`, every message `sent`, and a `cursor`. The origin's opening "
            "follows: a cutscene, then a conversation.",
            Json{{"type", "object"},
                 {"properties",
                  {{"race", {{"description", "human, elf or dwarf (or the race's id)."}}},
                   {"class", {{"description", "warrior, mage or rogue (or the class's id)."}}},
                   {"background",
                    {{"description", "The origin's background by name, as `offers` lists them (noble, magi, dalish, "
                                     "city, commoner in the game as shipped), or its id."}}},
                   {"gender", {{"description", "male or female. Default male."}}},
                   {"voice",
                    {{"type", "string"},
                     {"description", "The sound set: a name from the game's player sound sets (ss_pc_mhuman_wise), "
                                     "or its last word for the hero's race and gender (wise, cocky, exp, mystic, "
                                     "sultry, suave, smart, violent; not every one for every race). Default wise, "
                                     "the screen's own."}}},
                   {"name",
                    {{"type", "string"},
                     {"description", "The hero's name. Default the origin's own (Aedan for a human noble man), as "
                                     "the screen offers it."}}},
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
                     {"description", "List the backgrounds with the races and classes each takes, and, once a "
                                     "class is chosen, the hero's points and abilities to choose from. Sends "
                                     "nothing but the screen's own ability lookups."}}}}}},
            Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "chargen");
            return Creating(bench, args).Run();
        });
}

} // namespace eluvian
