#include "core/LevelUp.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
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

// A creature's id as the screen's messages carry it, in decimal: "0x82" is 130.
std::optional<std::uint32_t> Decimal(std::string_view id)
{
    if (id.size() < 3 || id.substr(0, 2) != "0x")
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
        const bool chosen = args.Has("abilities") || args.Has("attributes");
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
            const Json game = bench_.Invoke("inspect", Json{{"kind", "game"}}).data;
            id_ = game.value(Json::json_pointer("/controlled/id"), std::string());
        }
        const auto number = Decimal(id_);
        if (!number)
        {
            throw ToolError("levelup: `id` is a creature's id, like \"0x82\" (inspect kind=game lists the party)");
        }
        n_ = std::to_string(*number);
        const Json first = bench_.Invoke("inspect", Json{{"kind", "character"}, {"id", id_}}).data;
        canLevelUp_ = first.value("canLevelUp", false);
        if (!offers_ && !first.value("canLevelUp", false))
        {
            throw ToolError("levelup: " + first.value("name", id_) +
                            " has no level-up points to spend; inspect kind=character says `canLevelUp` and "
                            "`pointsToSpend`");
        }
        const Json abilities = bench_.Invoke("inspect", Json{{"kind", "abilities"}, {"id", id_}}).data;
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
        if (const auto after = Ask("inspect", Json{{"kind", "abilities"}, {"id", id_}}))
        {
            answer_["gained"] = Gained(abilities, *after);
        }
        if (PointsLeft(last) >= PointsLeft(first))
        {
            return End(false, auto_ ? "the screen's Auto Level Up spent nothing" : "nothing was spent");
        }
        return End(true, last.value("canLevelUp", false) ? "levelled; points are left to spend"
                                                         : "levelled: every point spent");
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
    bool Close(bool complete)
    {
        if (!Send("GUILevelUp.CharGenEnd " + n_ + " 1 " + (complete ? "1" : "0")) || !Send("GUI.HideFullScreenGUI"))
        {
            return false;
        }
        if (!Screen(false))
        {
            refused_ = "the level-up screen did not close: " + refused_ +
                       "; input {\"message\": \"GUI.HideFullScreenGUI\"} closes it";
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
                return End(false, refused_ + "; it opens only with a level-up waiting, and inspect kind=character "
                                             "says `canLevelUp` false");
            }
            return End(false, refused_);
        }
        const auto read = Ask("inspect", Json{{"kind", "levelup"}});
        Close(false); // abandoned: a look spends nothing
        if (!read)
        {
            return End(false, "the screen's offers could not be read: " + refused_);
        }
        for (const char *key : {"points", "attributes", "talents", "spells", "skills"})
        {
            answer_[key] = read->value(key, Json());
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
            const auto read = Ask("inspect", Json{{"kind", "character"}, {"id", id_}});
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
    // own buttons spend them: GUICharGen.CharGenAssignAbility then
    // Paperdoll.CharGenAssignAbility <id> <ability> 0, the screen's
    // SendSelectionMessage, and GUICharGen.CharGenAssignAttribute then
    // Paperdoll.CharGenAssignAttribute <id> <attribute> <amount> (read from
    // levelup.gfx, 2026-10-02 and 2026-10-04). GUILevelUp.CharGenAssign-
    // Ability <id> <ability> 1, sent here before, is the screen's
    // SpendSpecializationPoint: it granted the abilities and spent no talent
    // points, and an abandoned screen kept them (turn-based-agent,
    // 2026-10-04, #54). An ability the screen does not offer as learnable
    // now is refused, the screen abandoned; those taken before it are said.
    std::optional<Json> Choose(Json &last)
    {
        if (!Open())
        {
            return End(false, refused_);
        }
        std::this_thread::sleep_for(show_);
        Json taken = Json::array();
        // The points each kind has, counted here: the screen took a third
        // talent with two points, all three learned (turn-based-agent,
        // 2026-10-04, #54). Talents and spells share theirs; skills their own.
        double talentsLeft = -1;
        double skillsLeft = -1;
        for (const auto ability : chosen_)
        {
            // Read again each time: taking one makes the next in its chain
            // learnable.
            const auto offers = Ask("inspect", Json{{"kind", "levelup"}});
            if (!offers)
            {
                Close(false);
                return End(false, "the screen's offers could not be read: " + refused_);
            }
            if (talentsLeft < 0)
            {
                talentsLeft = offers->value(Json::json_pointer("/points/talentsOrSpells"), 0.0);
                skillsLeft = offers->value(Json::json_pointer("/points/skills"), 0.0);
            }
            const bool skill = PageOf(*offers, ability) == "skills";
            double &left = skill ? skillsLeft : talentsLeft;
            const std::string state = StateOf(*offers, ability);
            if (state == "learnable" && left < 1)
            {
                Close(false);
                answer_["taken"] = taken;
                return End(false, std::string("no ") + (skill ? "skill" : "talent or spell") +
                                      " points are left for ability " + std::to_string(ability) +
                                      (taken.empty() ? std::string("; nothing was spent")
                                                     : std::string("; those before it were taken and kept "
                                                                   "(`taken`): closing the screen does not give "
                                                                   "them back")));
            }
            if (state != "learnable")
            {
                Close(false);
                answer_["offers"] = Learnable(*offers);
                std::string why = "ability " + std::to_string(ability) + " is " +
                                  (state.empty() ? std::string("not among the screen's offers") : state);
                if (taken.empty())
                {
                    why += "; nothing was spent";
                }
                else
                {
                    // Taken, and kept: an abandoned screen gives nothing back,
                    // as Morrigan's four and Lyna's three stayed, their
                    // points spent (turn-based-agent, 2026-10-04, #54).
                    answer_["taken"] = taken;
                    why += "; those before it were taken and kept (`taken`): closing the screen does not give "
                           "them back";
                }
                return End(false, why + ". `offers` lists what is learnable now");
            }
            const std::string pick = n_ + " " + std::to_string(ability) + " 0";
            if (!Send("GUICharGen.CharGenAssignAbility " + pick) || !Send("Paperdoll.CharGenAssignAbility " + pick))
            {
                Close(false);
                return End(false, refused_);
            }
            taken.push_back(ability);
            left -= 1;
        }
        for (const auto &[name, amount] : raise_.items())
        {
            const std::string spend =
                n_ + " " + std::to_string(AttributeIndex(name)) + " " + std::to_string(amount.get<int>());
            if (!Send("GUICharGen.CharGenAssignAttribute " + spend) ||
                !Send("Paperdoll.CharGenAssignAttribute " + spend))
            {
                Close(false);
                return End(false, refused_);
            }
        }
        std::this_thread::sleep_for(show_);
        if (!Close(true))
        {
            return End(false, refused_);
        }
        ++rounds_;
        const auto read = Ask("inspect", Json{{"kind", "character"}, {"id", id_}});
        if (!read)
        {
            return End(false, "the level-up is done, but its result could not be read: " + refused_);
        }
        last = *read;
        return std::nullopt;
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
            const auto gui = Ask("inspect", Json{{"kind", "gui"}});
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
    std::string n_; // the creature's id in decimal, as the screen's messages carry it
    std::string id_;
    std::string refused_;
    int rounds_ = 0;
    Json answer_ = Json::object();
};

} // namespace

void AddLevelUpTool(Bench &bench, milliseconds show)
{
    bench.Tools().Add(
        ToolSpec{
            "levelup",
            "Spend a creature's level-up points as the game's level-up screen does, so its rules hold, for "
            "`id` (default the creature controlled). One of: `offers`: true, what the screen offers, nothing "
            "spent (`points`, `attributes`, and the class's `talents`, `spells` and `skills`, each a list "
            "of abilities with `id`, `name` and `state`: `has`, `learnable` now, or `not yet`); `abilities` "
            "(ids `offers` lists as learnable, the first of a chain before the next) and `attributes` ({\"strength\": "
            "2, ...}), spent as the screen's buttons spend "
            "them, an ability not learnable refusing the lot with nothing spent; or `auto`: true, the screen's "
            "Auto Level Up, which spends every point as the game chooses for the class (all a follower needs), "
            "again while points are left. It opens the screen for the creature, spends, ends it complete and "
            "closes it. Answers `levelled`, `reason`, `before` and "
            "`after` (level, points to spend), the `attributes` raised and by how much, the talents, spells "
            "and skills `gained`, and `cursor`. Refused for a creature with no points to spend "
            "(inspect kind=character says `canLevelUp`). A level-up is the character's for good once the "
            "game is saved. `showMs` (default 1000) is how long it lingers on the screen before Auto Level Up "
            "and on the result after, so whoever watches sees it; 0 for none.",
            Json{{"type", "object"},
                 {"properties",
                  {{"id",
                    {{"type", "string"}, {"description", "Who levels up: a creature id; default the one controlled"}}},
                   {"auto",
                    {{"type", "boolean"}, {"description", "Spend every point as the screen's Auto Level Up does"}}},
                   {"offers", {{"type", "boolean"}, {"description", "What the screen offers; nothing spent"}}},
                   {"abilities",
                    {{"type", "array"}, {"description", "Ability ids to take, learnable in `offers`, in order"}}},
                   {"attributes",
                    {{"type", "object"}, {"description", "Points per attribute: {\"strength\": 2, \"cunning\": 1}"}}},
                   {"showMs", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}}},
            Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "levelup");
            return Levelling(bench, args, show).Run();
        });
}

} // namespace eluvian
