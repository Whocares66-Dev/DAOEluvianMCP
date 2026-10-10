#include "core/LevelUp.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
// The screen's Auto Level Up spent two levels' points at once in play;
// rounds are asked again while points are left and fewer each time.
constexpr int kMostRounds = 5;
// The most icons a chain of abilities shows (levelup.gfx's
// AbilityChainContainer.MAX_CHAIN_LENGTH).
constexpr int kChainLength = 4;
// A page of the screen: the scene that shows it, the scene's object that
// counts its points, the engine's type for its chains (levelup.gfx:
// SceneManager.PlayScene("SpellsTalents" or "Skills"); TALENTS 1, SPELLS 2,
// SKILLS 3), and the scroller its abilities are listed in. Talents and
// spells share a scene, its points and its scroller; the skills page has a
// scroller of its own (ScrollingSkills_mc_init), where a skill was looked
// for in the talents' and not found (turn-based-agent, 2026-10-05, #63).
struct Page
{
    const char *scene;
    const char *object;
    int type;
    const char *scroller;
};

Page PageNamed(const std::string &page)
{
    if (page == "skills")
    {
        return Page{"Skills", "SkillsScene", 3, "ScrollingSkills_mc.Scrollable_mc"};
    }
    return Page{"SpellsTalents", "SpellsTalentsScene", page == "spells" ? 2 : 1, "ScrollingAbilities_mc.Scrollable_mc"};
}

// Where the screen lists a creature's specializations: one icon a
// specialization of its base class, icon0 up, each with sName,
// nSpecializationAbilityID, bSelected (had: HasAbility) and bGrey (not to
// be taken: its achievement not earned on the player's profile, or no
// point), as specializations_mc_init fills them (levelup.gfx); read in play
// for Lyna, Wynne and Oghren (turn-based-agent, 2026-10-05, #64).
constexpr const char *kSpecializations = "specializations_mc.Scrollable_mc";
// More icons than any class has specializations: Origins' four, Awakening's
// two more (#106).
constexpr int kMostSpecializations = 16;

// The paths in a list this GFx gives as its string: comma-joined.
std::vector<std::string> Split(const std::string &text)
{
    std::vector<std::string> parts;
    std::size_t from = 0;
    while (from < text.size())
    {
        const auto comma = std::min(text.find(',', from), text.size());
        if (comma > from)
        {
            parts.push_back(text.substr(from, comma - from));
        }
        from = comma + 1;
    }
    return parts;
}

// What became of those taken before a refusal: the screen, closed
// unfinished, reverts every pick its icons stored (its RevertAllChanges),
// as talent 7 went at an abandon in play (turn-based-agent, 2026-10-05,
// #58). Picks sent as messages before (#54) were not the screen's, and
// stayed.
std::string GivenBack(const Json &taken)
{
    return taken.empty() ? "; nothing was spent"
                         : "; nothing was spent: the screen, closed unfinished, gave back those taken before it "
                           "(`givenBack`)";
}

// A creature's id as the screen's messages carry it, in decimal: "0x82" is 130.
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

// Every point left to spend, of every kind.
double PointsLeft(const Json &character)
{
    double sum = 0;
    const Json points = character.value("pointsToSpend", Json::object());
    for (const auto &kind : points)
    {
        sum += kind.is_number() ? kind.get<double>() : 0;
    }
    return sum;
}

// A creature's talents, spells and skills by id.
std::set<int> AbilityIds(const Json &abilities)
{
    std::set<int> ids;
    for (const char *kind : {"talents", "spells", "skills"})
    {
        const Json list = abilities.value(kind, Json::array());
        for (const auto &ability : list)
        {
            ids.insert(ability.value("id", 0));
        }
    }
    return ids;
}

// One levelling: its arguments and the answer it builds.
class Levelling
{
  public:
    Levelling(Bench &bench, const Args &args, milliseconds show)
        : bench_(bench), show_(milliseconds(args.Integer("showMs", show.count(), 0, 10000)))
    {
        const bool chosen = args.Has("abilities") || args.Has("attributes") || args.Has("specialization");
        auto_ = args.Bool("auto", false);
        offers_ = args.Bool("offers", false);
        if ((auto_ ? 1 : 0) + (offers_ ? 1 : 0) + (chosen ? 1 : 0) != 1)
        {
            throw ToolError("levelup: give one of `auto`: true (the screen's Auto Level Up), `offers`: true (what "
                            "the screen offers, nothing spent), or the points to spend: `abilities` (ids the "
                            "screen offers as learnable) and `attributes` ({\"strength\": 2, ...})");
        }
        if (args.Has("abilities"))
        {
            const auto &list = args.Raw().at("abilities");
            if (!list.is_array())
            {
                throw ToolError("levelup: `abilities` is a list of ability ids, from `offers`");
            }
            for (const auto &ability : list)
            {
                if (!ability.is_number_integer())
                {
                    throw ToolError("levelup: each of `abilities` is an ability's id, a number, from `offers`");
                }
                chosen_.push_back(ability.get<std::int32_t>());
            }
        }
        if (args.Has("specialization"))
        {
            specialization_ = args.Raw().at("specialization");
            if (!specialization_.is_string() && !specialization_.is_number_integer())
            {
                throw ToolError("levelup: `specialization` is one the screen offers, by name or ability id, from "
                                "`offers`");
            }
        }
        if (args.Has("attributes"))
        {
            raise_ = args.Raw().at("attributes");
            for (const auto &[name, amount] : raise_.items())
            {
                if (AttributeIndex(name) == 0 || !amount.is_number_integer() || amount.get<int>() < 1)
                {
                    throw ToolError("levelup: `attributes` names strength, dexterity, willpower, magic, cunning or "
                                    "constitution, each with the points to put in it, 1 or more");
                }
            }
        }
        if (args.Has("id"))
        {
            id_ = args.String("id");
        }
        answer_["cursor"] = bench_.Events().Next();
    }

    Json Run()
    {
        if (id_.empty())
        {
            const Json game = bench_.Invoke("state", Json::object()).data;
            id_ = game.value(Json::json_pointer("/controlled/id"), std::string());
        }
        const auto number = Decimal(id_);
        if (!number)
        {
            throw ToolError("levelup: `id` is a creature's id, like \"0x82\" (state lists the party)");
        }
        n_ = std::to_string(*number);
        const Json first = bench_.Invoke("character", Json{{"id", id_}}).data;
        // A level-up waits with a point of any kind: the game answers
        // canLevelUp false with only a specialization point left, and Lyna,
        // Wynne and Oghren were refused theirs so (turn-based-agent,
        // 2026-10-05, #64).
        canLevelUp_ = first.value("canLevelUp", false) || PointsLeft(first) > 0;
        if (!offers_ && !canLevelUp_)
        {
            throw ToolError("levelup: " + first.value("name", id_) +
                            " has no level-up points to spend; character says `canLevelUp` and "
                            "`pointsToSpend`");
        }
        const Json abilities = bench_.Invoke("abilities", Json{{"id", id_}}).data;
        answer_["creature"] = Json{{"id", id_}, {"name", first.value("name", std::string())}};
        answer_["before"] = Sheet(first);
        if (offers_)
        {
            return Offers();
        }
        Json last = first;
        if (auto_)
        {
            if (auto stopped = Auto(last))
            {
                return std::move(*stopped);
            }
        }
        else if (auto stopped = Choose(last))
        {
            return std::move(*stopped);
        }
        answer_["after"] = Sheet(last);
        answer_["attributes"] = Raised(first, last);
        if (const auto after = Ask("abilities", Json{{"id", id_}}))
        {
            answer_["gained"] = Gained(abilities, *after);
        }
        if (PointsLeft(last) >= PointsLeft(first))
        {
            return End(false, auto_ ? "the screen's Auto Level Up spent nothing" : "nothing was spent");
        }
        return End(true, PointsLeft(last) > 0 ? "levelled; points are left to spend" : "levelled: every point spent");
    }

  private:
    // The screen opened for the creature; false with `refused_` said.
    bool Open()
    {
        if (!Send("GUI.GoToLevelUp " + n_ + " 0"))
        {
            return false;
        }
        if (!Screen(true))
        {
            refused_ = "the level-up screen did not open: " + refused_ +
                       " (in a fight, a conversation or a changed shape, Bear Shape and the like, it may not)";
            return false;
        }
        return true;
    }

    // The screen closed, complete (what was spent kept) or abandoned.
    //
    // As the screen's own Finish and Close buttons close it: its
    // LevelUp.nExitPoint set to its LEVELUP_EXIT_COMPLETE or _ABORT, then
    // the screen hidden; its scene's end reverts every pick it stored if
    // aborted, and sends GUILevelUp.CharGenEnd itself (levelup.gfx, read
    // 2026-10-05). Sending CharGenEnd here first, as before, left the
    // screen to end aborted after it and revert what its icons had taken
    // (turn-based-agent, 2026-10-05, #58).
    bool Close(bool complete)
    {
        const auto exit = Number(std::string("LevelUp.") + (complete ? "LEVELUP_EXIT_COMPLETE" : "LEVELUP_EXIT_ABORT"));
        if (!exit)
        {
            return false;
        }
        const auto set = Movie(Json{{"set", "LevelUp.nExitPoint"}, {"value", *exit}});
        if (!set)
        {
            return false;
        }
        if (!set->value("done", false))
        {
            refused_ = "the screen did not take LevelUp.nExitPoint";
            return false;
        }
        if (!Send("GUI.HideFullScreenGUI"))
        {
            return false;
        }
        if (!Screen(false))
        {
            refused_ = "the level-up screen did not close: " + refused_ +
                       R"(; input {"message": "GUI.HideFullScreenGUI"} closes it)";
            return false;
        }
        return true;
    }

    // What the screen offers, read with it up, nothing spent: the screen is
    // abandoned after, as its close button leaves it.
    Json Offers()
    {
        if (!Open())
        {
            // The screen opens only for a creature with a level-up waiting:
            // Oghren, joined a minute before, gave the screen's timeout twice
            // (turn-based-agent, 2026-10-04, #54).
            if (!canLevelUp_)
            {
                return End(false, refused_ + "; it opens only with a level-up waiting, and character "
                                             "says `canLevelUp` false");
            }
            return End(false, refused_);
        }
        const auto read = Ask("levelup.screen", Json::object());
        // The specializations as the screen shows them: an empty list read
        // as none unlocked, where the tool had listed none at all
        // (turn-based-agent, 2026-10-05, #64).
        if (const auto specializations = Specializations())
        {
            Json listed = Json::array();
            for (const auto &specialization : *specializations)
            {
                listed.push_back(
                    Json{{"id", specialization.id}, {"name", specialization.name}, {"state", specialization.state}});
            }
            answer_["specializations"] = std::move(listed);
        }
        Close(false); // abandoned: a look spends nothing
        if (!read)
        {
            return End(false, "the screen's offers could not be read: " + refused_);
        }
        for (const char *key : {"points", "attributes", "talents", "spells", "skills"})
        {
            answer_[key] = read->value(key, Json());
        }
        // Which of talents and spells the screen shows, where it said (#71):
        // the other list is empty, the class having none to take.
        if (read->contains("abilityPage"))
        {
            answer_["abilityPage"] = read->at("abilityPage");
        }
        return End(true, "the screen's offers: levelup with `abilities` (learnable ids) and `attributes` spends "
                         "points on them");
    }

    // The screen's Auto Level Up, again while points are left and fewer.
    std::optional<Json> Auto(Json &last)
    {
        for (int round = 0; round < kMostRounds; ++round)
        {
            if (!Open())
            {
                return End(false, refused_);
            }
            // The screen up a moment before and after its Auto Level Up, for
            // whoever watches: in one call it came and went in under a
            // second, never seen (the user, 2026-10-03, #14).
            std::this_thread::sleep_for(show_);
            if (!Send("GUILevelUp.AutoLevelUp " + n_))
            {
                return End(false, refused_);
            }
            std::this_thread::sleep_for(show_);
            if (!Close(true))
            {
                return End(false, refused_);
            }
            const auto read = Ask("character", Json{{"id", id_}});
            if (!read)
            {
                return End(false, "the level-up is done, but its result could not be read: " + refused_);
            }
            const bool spent = PointsLeft(*read) < PointsLeft(last);
            last = *read;
            ++rounds_;
            if (!last.value("canLevelUp", false) || !spent)
            {
                break;
            }
        }
        return std::nullopt;
    }

    // Points spent on the abilities and attributes named, as the screen's
    // own buttons spend them. An ability is taken as a click on its icon
    // takes it: the icon's OnButtonAction is its scene's Learn, whose
    // AbilityScene.SpendPoint takes a point only while the page has one,
    // stores the pick and sends the selection (levelup.gfx, read
    // 2026-10-04). Before, Eluvian sent the selection itself and counted the
    // points, a rule the screen has (the user, 2026-10-04, #58); the icons
    // were found in play under the page's class containers
    // (turn-based-agent, 2026-10-05). Whether a press took a point is the
    // page's own count after. Attributes go as before:
    // GUICharGen.CharGenAssignAttribute then Paperdoll.CharGenAssignAttribute
    // <id> <attribute> <amount>. An ability the screen does not offer as
    // learnable now is refused, the screen abandoned; those taken before it
    // are said.
    // The abilities chosen, one at a time as the screen's icons take them,
    // read again each time: nothing, or the answer the screen was
    // abandoned with, what was taken given back.
    std::optional<Json> TakeAbilities()
    {
        Json taken = Json::array();
        for (const auto ability : chosen_)
        {
            // A specialization, taken as a click on its icon takes it.
            if (const auto taking = Specialization(ability))
            {
                if (!*taking)
                {
                    Close(false);
                    if (!taken.empty())
                    {
                        answer_["givenBack"] = taken;
                    }
                    return End(false, refused_ + GivenBack(taken));
                }
                taken.emplace_back(ability);
                continue;
            }
            // Read again each time: taking one makes the next in its chain
            // learnable.
            const auto offers = Ask("levelup.screen", Json::object());
            if (!offers)
            {
                Close(false);
                return End(false, "the screen's offers could not be read: " + refused_);
            }
            const std::string state = StateOf(*offers, ability);
            if (state != "learnable")
            {
                Close(false);
                answer_["offers"] = Learnable(*offers);
                if (!taken.empty())
                {
                    answer_["givenBack"] = taken;
                }
                return End(false, "ability " + std::to_string(ability) + " is " +
                                      (state.empty() ? std::string("not among the screen's offers") : state) +
                                      GivenBack(taken) + ". `offers` lists what is learnable now");
            }
            if (!Press(PageOf(*offers, ability), ability))
            {
                Close(false);
                if (!taken.empty())
                {
                    answer_["givenBack"] = taken;
                }
                return End(false, refused_ + GivenBack(taken));
            }
            taken.push_back(ability);
        }
        return std::nullopt;
    }

    // The attributes raised as the screen's buttons raise them: false,
    // with refused_ said, where the screen took a raise not.
    bool RaiseAttributes()
    {
        // Each raise is a command to the screen, stopping at the first it
        // refuses: a loop says so plainer than all_of with a predicate that
        // sends (#77).
        // NOLINTNEXTLINE(readability-use-anyofallof)
        for (const auto &[name, amount] : raise_.items())
        {
            const std::string spend =
                n_ + " " + std::to_string(AttributeIndex(name)) + " " + std::to_string(amount.get<int>());
            if (!Send("GUICharGen.CharGenAssignAttribute " + spend) ||
                !Send("Paperdoll.CharGenAssignAttribute " + spend))
            {
                return false;
            }
        }
        return true;
    }

    std::optional<Json> Choose(Json &last)
    {
        if (!Open())
        {
            return End(false, refused_);
        }
        std::this_thread::sleep_for(show_);
        if (auto unknown = ChooseSpecialization())
        {
            return unknown;
        }
        if (auto abandoned = TakeAbilities())
        {
            return abandoned;
        }
        if (!RaiseAttributes())
        {
            Close(false);
            return End(false, refused_);
        }
        std::this_thread::sleep_for(show_);
        if (!Close(true))
        {
            return End(false, refused_);
        }
        ++rounds_;
        const auto read = Ask("character", Json{{"id", id_}});
        if (!read)
        {
            return End(false, "the level-up is done, but its result could not be read: " + refused_);
        }
        last = *read;
        return std::nullopt;
    }

    // A specialization as its icon shows it.
    struct Special
    {
        std::int32_t id = 0;
        std::string name;
        std::string state; // has, learnable, locked, or no point to spend
        std::string icon;
    };

    // The specialization `specialization` names, by name (any case) or
    // ability id, put first among the abilities to take: taken as a click on
    // its icon takes it, so its own chain can be spent on after it in the
    // same call (#106). Nothing, or the refusal naming those listed.
    std::optional<Json> ChooseSpecialization()
    {
        if (specialization_.is_null())
        {
            return std::nullopt;
        }
        const auto listed = SpecializationsMade().value_or(std::vector<Special>{});
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        for (const auto &special : listed)
        {
            const bool byId = specialization_.is_number_integer() && specialization_.get<std::int32_t>() == special.id;
            const bool byName =
                specialization_.is_string() && lower(special.name) == lower(specialization_.get<std::string>());
            if (byId || byName)
            {
                chosen_.insert(chosen_.begin(), special.id);
                return std::nullopt;
            }
        }
        std::string names;
        for (const auto &special : listed)
        {
            names += (names.empty() ? "" : ", ") + special.name + " (" + special.state + ")";
        }
        Close(false);
        return End(false, "no specialization " + specialization_.dump() +
                              " on the screen; it lists: " + (names.empty() ? std::string("none") : names));
    }

    // The specializations the screen lists once it has made their icons,
    // which it does over its first frames, as a page's: a take with
    // `showMs` 0 read them at once, found none, and refused Sigrun's Bard
    // that offers had just listed (turn-based-agent, 2026-10-09). Read until
    // there are some, or the screen's wait is out.
    std::optional<std::vector<Special>> SpecializationsMade()
    {
        const auto deadline = Clock::now() + kScreenWait;
        while (true)
        {
            auto read = Specializations();
            if (!read || !read->empty() || Clock::now() >= deadline)
            {
                return read;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    // The specializations the screen lists for the creature as its icons
    // stand now; nothing with `refused_` said.
    std::optional<std::vector<Special>> Specializations()
    {
        // Counted by the icons themselves, read until one is missing: in
        // Awakening the screen showed six (icon0 to icon5, the two new ones
        // not grey) and the tool listed none, LevelUp.nSpecializationsFound
        // not reading as the icons stand (turn-based-agent, 2026-10-09,
        // #106).
        const double points = Number("LevelUp.nSpecializationPoints").value_or(0);
        std::vector<Special> specializations;
        for (int n = 0; n < kMostSpecializations; ++n)
        {
            const std::string icon = std::string(kSpecializations) + ".icon" + std::to_string(n);
            const auto id = Number(icon + ".nSpecializationAbilityID");
            if (!id)
            {
                break;
            }
            const auto name = Movie(Json{{"get", icon + ".sName"}});
            const auto selected = Movie(Json{{"get", icon + ".bSelected"}});
            const auto grey = Movie(Json{{"get", icon + ".bGrey"}});
            if (!name || !selected || !grey)
            {
                return std::nullopt;
            }
            const bool had = selected->value("value", Json()) == true;
            const bool shut = grey->value("value", Json()) == true;
            const Json label = name->value("value", Json());
            const char *state = "locked";
            if (had)
            {
                state = "has";
            }
            else if (!shut)
            {
                state = "learnable";
            }
            else if (points < 1)
            {
                state = "no point to spend";
            }
            specializations.push_back(Special{static_cast<std::int32_t>(*id),
                                              label.is_string() ? label.get<std::string>() : std::string(), state,
                                              icon});
        }
        return specializations;
    }

    // A specialization taken by a click on its icon (its onPressHitArea,
    // which spends only on an icon not grey, not had, with a point left:
    // SpendSpecializationPoint, GUILevelUp.CharGenAssignAbility <id> <ability>
    // 1). Nothing if the ability is no specialization of the creature's;
    // false with `refused_` said if it was not taken.
    std::optional<bool> Specialization(std::int32_t ability)
    {
        if (!specializations_)
        {
            specializations_ = Specializations();
            if (!specializations_)
            {
                specializations_ = std::vector<Special>{}; // unread: none taken as one
            }
        }
        const auto it = std::ranges::find(*specializations_, ability, &Special::id);
        if (it == specializations_->end())
        {
            return std::nullopt;
        }
        if (it->state == "has")
        {
            refused_ = "the creature has the specialization " + it->name + " (" + std::to_string(ability) + ") already";
            return false;
        }
        const auto before = Number("LevelUp.nSpecializationPoints");
        if (!before || !Movie(Json{{"call", it->icon + ".onPressHitArea"}}))
        {
            return false;
        }
        const auto left = Number("LevelUp.nSpecializationPoints");
        if (!left)
        {
            return false;
        }
        if (*left < *before)
        {
            answer_["specialization"] = Json{{"id", ability}, {"name", it->name}};
            return true;
        }
        refused_ = *before < 1
                       ? "no specialization point is left for " + it->name
                       : "the screen took no point for the specialization " + it->name + ": it is " + it->state +
                             " (a specialization is unlocked by its achievement, kept on the "
                             "player's profile)";
        return false;
    }

    // An ability taken by pressing its icon on its page; false with
    // `refused_` said.
    bool Press(const std::string &pageName, std::int32_t ability)
    {
        const Page page = PageNamed(pageName);
        if (!ShowPage(page))
        {
            return false;
        }
        const auto icon = icons_.find(ability);
        if (icon == icons_.end())
        {
            refused_ = "ability " + std::to_string(ability) + " is not shown on the screen's " + pageName + " page";
            return false;
        }
        const std::string points = std::string(page.object) + ".nPointsToSpend";
        const auto before = Number(points);
        if (!before || !Movie(Json{{"call", icon->second + ".OnButtonAction"}}))
        {
            return false;
        }
        const auto after = Number(points);
        if (!after)
        {
            return false;
        }
        if (*after < *before)
        {
            return true;
        }
        // The screen took no point: none left, as its SpendPoint wants one
        // (Zevran's third talent with two points, 2026-10-04, #54).
        refused_ = *before < 1 ? std::string("no ") + (pageName == "skills" ? "skill" : "talent or spell") +
                                     " points are left for ability " + std::to_string(ability)
                               : "the screen took no point for ability " + std::to_string(ability) +
                                     " when its icon was pressed";
        return false;
    }

    // The page shown, as the screen's own navigation shows it: its chains
    // asked of the engine, then its scene played; and its icons read.
    bool ShowPage(const Page &page)
    {
        if (shown_ == page.scene)
        {
            return true;
        }
        if (!Send("GameModeController.FindAbilityChains " + n_ + " " + std::to_string(page.type) + " 2") ||
            !Movie(Json{{"call", "SceneManager.PlayScene"}, {"args", Json::array({page.scene})}}))
        {
            return false;
        }
        // The page's icons are made over its first frames.
        const auto deadline = Clock::now() + kScreenWait;
        while (true)
        {
            auto icons = Icons(page.scroller);
            if (!icons)
            {
                return false;
            }
            if (!icons->empty())
            {
                icons_ = std::move(*icons);
                shown_ = page.scene;
                return true;
            }
            if (Clock::now() >= deadline)
            {
                refused_ = std::string("the screen's ") + page.scene + " page showed no abilities after 5 s";
                return false;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    // The abilities a page shows, each by the path to its icon: its chains,
    // and each chain's icons, icon0 up, an icon's m_oAbility the ability
    // (seen in play, turn-based-agent, 2026-10-05). The chains are the
    // scroller's own where it keeps them, the skills page adding each
    // ability to its scrollable itself; else each of its class containers'
    // (AbilityChainContainer_mc), as on the talents' page.
    std::optional<std::map<std::int32_t, std::string>> Icons(const std::string &scroller)
    {
        auto chains = Chains(scroller);
        if (!chains)
        {
            return std::nullopt;
        }
        if (chains->empty())
        {
            const auto containers = Movie(Json{{"children", scroller}});
            if (!containers)
            {
                return std::nullopt;
            }
            for (const auto &container : containers->value("children", Json::array()))
            {
                const auto theirs = Chains(container.value("path", std::string()) + ".AbilityChainContainer_mc");
                if (!theirs)
                {
                    return std::nullopt;
                }
                chains->insert(chains->end(), theirs->begin(), theirs->end());
            }
        }
        std::map<std::int32_t, std::string> icons;
        for (const auto &chain : *chains)
        {
            for (int k = 0; k < kChainLength; ++k)
            {
                const std::string icon = chain + ".icon" + std::to_string(k);
                const auto id = Movie(Json{{"get", icon + ".m_oAbility.nID"}});
                if (!id)
                {
                    return std::nullopt;
                }
                const Json value = id->value("value", Json());
                if (!value.is_number())
                {
                    break; // the chain is shorter
                }
                icons[static_cast<std::int32_t>(value.get<double>())] = icon;
            }
        }
        return icons;
    }

    // A holder's chains (its m_aChains, which this GFx gives as their paths
    // comma-joined); none where it has no such list.
    std::optional<std::vector<std::string>> Chains(const std::string &holder)
    {
        const auto read = Movie(Json{{"get", holder + ".m_aChains"}});
        if (!read)
        {
            return std::nullopt;
        }
        const Json list = read->value("value", Json());
        return Split(list.is_string() ? list.get<std::string>() : std::string());
    }

    // A number in the screen's movie; nothing with `refused_` said.
    std::optional<double> Number(const std::string &path)
    {
        const auto read = Movie(Json{{"get", path}});
        if (!read)
        {
            return std::nullopt;
        }
        const Json value = read->value("value", Json());
        if (!value.is_number())
        {
            refused_ = "the screen's " + path + " read " + read->value("type", Json()).dump() + ", not a number";
            return std::nullopt;
        }
        return value.get<double>();
    }

    // The level-up screen's own script, through movie.
    std::optional<Json> Movie(Json args)
    {
        args["screen"] = "LevelUp";
        return Ask("movie", args);
    }

    // An attribute's place in the screen's list, 1 to 6; 0 for no such name.
    static int AttributeIndex(const std::string &name)
    {
        constexpr std::array<std::string_view, 6> kNames = {"strength", "dexterity", "willpower",
                                                            "magic",    "cunning",   "constitution"};
        const auto at = std::ranges::find(kNames, name);
        return at == kNames.end() ? 0 : static_cast<int>(at - kNames.begin()) + 1;
    }

    // The page an ability is on among the screen's offers: talents, spells
    // or skills; empty if not offered.
    static std::string PageOf(const Json &offers, std::int32_t ability)
    {
        for (const char *page : {"talents", "spells", "skills"})
        {
            for (const auto &entry : offers.value(page, Json::array()))
            {
                if (entry.value("id", 0) == ability)
                {
                    return page;
                }
            }
        }
        return {};
    }

    // An ability's state among the screen's offers; empty if not offered.
    static std::string StateOf(const Json &offers, std::int32_t ability)
    {
        for (const char *page : {"talents", "spells", "skills"})
        {
            for (const auto &entry : offers.value(page, Json::array()))
            {
                if (entry.value("id", 0) == ability)
                {
                    return entry.value("state", std::string());
                }
            }
        }
        return {};
    }

    // The abilities learnable now, by page.
    static Json Learnable(const Json &offers)
    {
        Json learnable = Json::object();
        for (const char *page : {"talents", "spells", "skills"})
        {
            Json list = Json::array();
            for (const auto &entry : offers.value(page, Json::array()))
            {
                if (entry.value("state", std::string()) == "learnable")
                {
                    list.push_back(entry);
                }
            }
            learnable[page] = std::move(list);
        }
        return learnable;
    }

    // The game's own message by its text, as `input` sends it.
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

    // Waits for the level-up screen to be up, or gone.
    bool Screen(bool up)
    {
        const auto deadline = Clock::now() + kScreenWait;
        while (Clock::now() < deadline)
        {
            const auto gui = Ask("gui", Json::object());
            if (!gui)
            {
                return false;
            }
            const bool shown = gui->value("fullScreen", Json()) == "LevelUp";
            if (shown == up)
            {
                return true;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        refused_ = up ? "it was not up after 5 s" : "it was still up after 5 s";
        return false;
    }

    // A call refused for the game's state, once the screen is in play: the
    // levelling ends with the refusal said, as travel's journeys do (#7).
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

    static Json Sheet(const Json &character)
    {
        return Json{{"level", character.value("level", 0)},
                    {"canLevelUp", character.value("canLevelUp", false)},
                    {"pointsToSpend", character.value("pointsToSpend", Json::object())}};
    }

    // Each attribute's base raised, by how much.
    static Json Raised(const Json &before, const Json &after)
    {
        Json raised = Json::object();
        const Json was = before.value("attributes", Json::object());
        const Json is = after.value("attributes", Json::object());
        for (const auto &[name, now] : is.items())
        {
            const double from = was.contains(name) ? was.at(name).value("base", 0.0) : 0.0;
            const double to = now.value("base", 0.0);
            if (to != from)
            {
                raised[name] = to - from;
            }
        }
        return raised;
    }

    // The talents, spells and skills it has now and had not.
    static Json Gained(const Json &before, const Json &after)
    {
        const auto had = AbilityIds(before);
        Json gained = Json::array();
        for (const char *kind : {"talents", "spells", "skills"})
        {
            const Json list = after.value(kind, Json::array());
            for (const auto &ability : list)
            {
                if (!had.contains(ability.value("id", 0)))
                {
                    gained.push_back(Json{{"id", ability.value("id", 0)},
                                          {"name", ability.value("name", std::string())},
                                          {"kind", std::string(kind).substr(0, std::string(kind).size() - 1)}});
                }
            }
        }
        return gained;
    }

    Json End(bool levelled, std::string reason)
    {
        answer_["levelled"] = levelled;
        answer_["reason"] = std::move(reason);
        answer_["rounds"] = rounds_;
        return answer_;
    }

    Bench &bench_;
    milliseconds show_;
    bool auto_ = false;
    bool offers_ = false;
    bool canLevelUp_ = false; // whether the creature had a level-up waiting when asked
    std::vector<std::int32_t> chosen_;
    Json raise_ = Json::object();
    Json specialization_; // `specialization`: one to take, by name or id (#106)
    std::string n_;       // the creature's id in decimal, as the screen's messages carry it
    std::string shown_;   // the scene of the page shown, once one is
    std::optional<std::vector<Special>> specializations_; // as the screen listed them, once read
    std::map<std::int32_t, std::string> icons_;           // its abilities' icons
    std::string id_;
    std::string refused_;
    int rounds_ = 0;
    Json answer_ = Json::object();
};

} // namespace

void AddLevelUpTool(Bench &bench, milliseconds show)
{
    bench.Tools().Add(
        ToolSpec{"levelup",
                 "Spends a creature's level-up points through the game's level-up screen, so the game's own rules "
                 "apply. `id` picks the creature (default: the creature you control). The tool opens the screen, "
                 "spends the points, completes the level-up and closes the screen. Pass exactly one of:\n\n"
                 "- `offers: true`: returns what the screen offers and spends nothing: `points`, `attributes`, and "
                 "the class's `talents`, `spells` and `skills`, each a list of abilities with `id`, `name` and "
                 "`state` (`has`, `learnable` now, or `not yet`); and the `specializations`, each with `id`, `name` "
                 "and `state` (`has`, `learnable`, `locked` or `no point to spend`).\n"
                 "- The points to spend: `specialization`, `abilities` and `attributes`, in any combination, spent as "
                 "the screen's buttons spend them. A specialization is taken first, like a click on its icon, so its "
                 "abilities can be taken in the same call; it is refused, saying why, if already taken, locked, or "
                 "there is no point for it. List `abilities` with the first of a chain before the next. If any "
                 "ability is not learnable, nothing is spent.\n"
                 "- `auto: true`: the screen's Auto Level Up, which spends every point as the game chooses for the "
                 "class (all a follower needs), repeating while points are left.\n\n"
                 "The answer includes `levelled`, `reason`, `before` and `after` (level and points to spend), the "
                 "`attributes` raised and by how much, the talents, spells and skills `gained`, and a `cursor`. It "
                 "refuses a creature with no points to spend (`character` shows `canLevelUp`). A level-up is "
                 "permanent once the game is saved.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"id",
                         {{"type", "string"},
                          {"description", "Who levels up, by creature id. Default: the creature you control."}}},
                        {"auto",
                         {{"type", "boolean"},
                          {"description", "If true, spend every point as the screen's Auto Level Up does."}}},
                        {"offers",
                         {{"type", "boolean"},
                          {"description", "If true, return what the screen offers and spend nothing."}}},
                        {"abilities",
                         {{"type", "array"},
                          {"description", "Ability ids to take, in order, from those `offers` lists as learnable."}}},
                        {"attributes",
                         {{"type", "object"},
                          {"description", R"(Points to put in each attribute, e.g. {"strength": 2, "cunning": 1}.)"}}},
                        {"specialization",
                         {{"description", "A specialization to take, by name or ability id, as `offers` lists it."}}},
                        {"showMs",
                         {{"type", "integer"},
                          {"minimum", 0},
                          {"maximum", 10000},
                          {"description", "How long to linger on the screen before Auto Level Up and on the result, in "
                                          "milliseconds, so whoever watches sees it. Default 1000; 0 for none."}}}}}},
                 Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "levelup");
            return Levelling(bench, args, show).Run();
        });
}

} // namespace eluvian
