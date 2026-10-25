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

// What became of those taken before a refusal: kept, as the engine took
// them, the level-up ended over them. The screen had given them back
// (#58), its icons' picks reverted as it closed unfinished; levelup.take's
// are the engine's own, and no screen reverts them (#180).
std::string Kept(const Json &taken)
{
    return taken.empty() ? "; nothing was spent"
                         : "; those taken before it are kept (`taken`), and the points left stay to spend";
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
        if (!offers_ && !first.value("canLevelUp", false) && PointsLeft(first) <= 0)
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
            return Offers(first);
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
    // What the screen would offer, read with no screen (levelup.offers);
    // the attributes as the character sheet totals them.
    Json Offers(const Json &character)
    {
        const auto read = Ask("levelup.offers", Json{{"id", id_}});
        if (!read)
        {
            return End(false, "the offers could not be read: " + refused_);
        }
        for (const char *key : {"points", "talents", "spells", "skills", "specializations", "abilityPage"})
        {
            if (read->contains(key))
            {
                answer_[key] = read->at(key);
            }
        }
        Json attributes = Json::object();
        const Json sheet = character.value("attributes", Json::object());
        for (const auto &[name, value] : sheet.items())
        {
            attributes[name] = value.value("total", 0.0);
        }
        answer_["attributes"] = std::move(attributes);
        return End(true, "the offers: levelup with `specialization`, `abilities` (learnable ids) and `attributes` "
                         "spends points on them");
    }

    // The screen up for whoever watches (#14), where it opens: a pick
    // taken while it is up shows on it (DAOLib #259). Where it does not
    // open (a fight, a conversation, a changed shape) the levelling goes on
    // without it, said in `screen`.
    void Show()
    {
        if (show_.count() == 0)
        {
            return;
        }
        if (Send("GUI.GoToLevelUp " + n_ + " 0") && Screen(true))
        {
            screenUp_ = true;
            answer_.erase("screen");
            std::this_thread::sleep_for(show_);
            return;
        }
        answer_["screen"] = "not shown: " + refused_;
        refused_.clear();
    }

    // The level-up ended as the screen's Done ends it: the screen's own,
    // where it is up; else levelup.finish, which is the same with none.
    bool Conclude()
    {
        if (!screenUp_)
        {
            return Step("levelup.finish", Json{{"id", id_}}).has_value();
        }
        std::this_thread::sleep_for(show_);
        screenUp_ = false;
        return Close(true);
    }

    // After a refusal: what was taken ended as a level-up, or, with nothing
    // taken, the screen closed unfinished.
    void Settle(bool spent)
    {
        const std::string why = refused_;
        if (spent)
        {
            Conclude();
        }
        else if (screenUp_)
        {
            screenUp_ = false;
            Close(false);
        }
        refused_ = why;
    }

    // The screen closed, complete (what was spent kept) or abandoned.
    //
    // As the screen's own Finish and Close buttons close it: its
    // LevelUp.nExitPoint set to its LEVELUP_EXIT_COMPLETE or _ABORT, then
    // the screen hidden; its scene's end sends GUILevelUp.CharGenEnd itself
    // (levelup.gfx, read 2026-10-05). Sending CharGenEnd here first, as
    // before, left the screen to end aborted after it (turn-based-agent,
    // 2026-10-05, #58).
    bool Close(bool complete)
    {
        const auto exit = Number(std::string("LevelUp.") + (complete ? "LEVELUP_EXIT_COMPLETE" : "LEVELUP_EXIT_ABORT"));
        if (!exit)
        {
            return false;
        }
        const auto set = Ask("movie", Json{{"screen", "LevelUp"}, {"set", "LevelUp.nExitPoint"}, {"value", *exit}});
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

    // The screen's Auto Level Up, again while points are left and fewer.
    std::optional<Json> Auto(Json &last)
    {
        for (int round = 0; round < kMostRounds; ++round)
        {
            Show();
            if (!Step("levelup.auto", Json{{"id", id_}}))
            {
                Settle(false);
                return End(false, refused_);
            }
            if (!Conclude())
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

    // Points spent on the specialization, abilities and attributes named,
    // each as the screen's own button spends it (levelup.take and
    // levelup.raise, by the engine's own tests). An ability not offered as
    // learnable now stops the levelling there, saying why and what is.
    std::optional<Json> Choose(Json &last)
    {
        Show();
        const auto offers = Ask("levelup.offers", Json{{"id", id_}});
        if (!offers)
        {
            Settle(false);
            return End(false, "the offers could not be read: " + refused_);
        }
        for (const auto &special : offers->value("specializations", Json::array()))
        {
            specializations_[special.value("id", 0)] = special.value("name", std::string());
        }
        if (auto unknown = ChooseSpecialization(*offers))
        {
            return unknown;
        }
        Json taken = Json::array();
        answer_["taken"] = Json::array();
        for (const auto ability : chosen_)
        {
            if (!Take(ability))
            {
                if (const auto now = Ask("levelup.offers", Json{{"id", id_}}))
                {
                    answer_["offers"] = Learnable(*now);
                }
                Settle(!taken.empty());
                return End(false, refused_ + Kept(taken) + ". `offers` lists what is learnable now");
            }
            taken.push_back(ability);
            answer_["taken"] = taken;
        }
        if (!RaiseAttributes())
        {
            Settle(!taken.empty() || raised_);
            return End(false, refused_);
        }
        if (!Conclude())
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

    // The specialization `specialization` names, by name (any case) or
    // ability id, put first among the abilities to take, so its own chain
    // can be spent on after it in the same call (#106). Nothing, or the
    // refusal naming those offered.
    std::optional<Json> ChooseSpecialization(const Json &offers)
    {
        if (specialization_.is_null())
        {
            return std::nullopt;
        }
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const Json listed = offers.value("specializations", Json::array());
        for (const auto &special : listed)
        {
            const auto id = special.value("id", 0);
            const bool byId = specialization_.is_number_integer() && specialization_.get<std::int32_t>() == id;
            const bool byName = specialization_.is_string() && lower(special.value("name", std::string())) ==
                                                                   lower(specialization_.get<std::string>());
            if (byId || byName)
            {
                chosen_.insert(chosen_.begin(), id);
                return std::nullopt;
            }
        }
        std::string names;
        for (const auto &special : listed)
        {
            names += (names.empty() ? "" : ", ") + special.value("name", std::string()) + " (" +
                     special.value("state", std::string()) + ")";
        }
        Settle(false);
        return End(false, "no specialization " + specialization_.dump() +
                              " is offered; the offers list: " + (names.empty() ? std::string("none") : names));
    }

    // An ability or specialization taken; false with `refused_` said.
    bool Take(std::int32_t ability)
    {
        const auto special = specializations_.find(ability);
        const bool isSpecial = special != specializations_.end();
        const auto took = Step("levelup.take", Json{{"id", id_}, {"ability", ability}, {"specialization", isSpecial}});
        if (!took)
        {
            return false;
        }
        if (isSpecial)
        {
            answer_["specialization"] = Json{{"id", ability}, {"name", special->second}};
        }
        return true;
    }

    // The attributes raised as the screen's plus buttons raise them: false,
    // with refused_ said, where one was refused.
    bool RaiseAttributes()
    {
        for (const auto &[name, amount] : raise_.items())
        {
            const Json asked = {{"id", id_}, {"attribute", AttributeIndex(name)}, {"points", amount}};
            if (!Step("levelup.raise", asked))
            {
                refused_ = name + ": " + refused_;
                if (raised_)
                {
                    refused_ += "; the attributes raised before it are kept";
                }
                return false;
            }
            raised_ = true;
        }
        return true;
    }

    // A level-up step (levelup.take, raise, auto, finish): its answer, or
    // nothing with `refused_` said, the step's own refusal among them.
    std::optional<Json> Step(std::string_view tool, const Json &args)
    {
        auto answer = Ask(tool, args);
        if (answer && !answer->value("taken", true))
        {
            const std::string refusal = answer->value("refusal", std::string("refused"));
            const std::string what = args.contains("ability") ? "ability " + args.at("ability").dump() : "it";
            refused_ = what + " was refused: " + refusal;
            if (refusal == "locked")
            {
                refused_ += args.value("specialization", false)
                                ? " (a specialization is unlocked by a manual or a trainer)"
                                : " (not yet: a level, an attribute or the one before it in its chain)";
            }
            return std::nullopt;
        }
        return answer;
    }

    // A number in the screen's movie; nothing with `refused_` said.
    std::optional<double> Number(const std::string &path)
    {
        const auto read = Ask("movie", Json{{"screen", "LevelUp"}, {"get", path}});
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

    // An attribute's place among the six, 1 to 6; 0 for no such name.
    static int AttributeIndex(const std::string &name)
    {
        constexpr std::array<std::string_view, 6> kNames = {"strength", "dexterity", "willpower",
                                                            "magic",    "cunning",   "constitution"};
        const auto at = std::ranges::find(kNames, name);
        return at == kNames.end() ? 0 : static_cast<int>(at - kNames.begin()) + 1;
    }

    // The abilities learnable now, by page.
    static Json Learnable(const Json &offers)
    {
        Json learnable = Json::object();
        for (const char *page : {"talents", "spells", "skills", "specializations"})
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

    // A call refused for the game's state, once the levelling is under
    // way: it ends with the refusal said, as travel's journeys do (#7).
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
    bool screenUp_ = false; // the screen shown for whoever watches, until the level-up ends
    bool raised_ = false;   // an attribute raised already
    std::vector<std::int32_t> chosen_;
    Json raise_ = Json::object();
    Json specialization_;                                 // `specialization`: one to take, by name or id (#106)
    std::map<std::int32_t, std::string> specializations_; // those offered, by ability, with their names
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
        ToolSpec{"levelup",
                 "Spends a creature's level-up points as the game's level-up screen does, by the engine's own "
                 "rules for what may be taken. `id` picks the creature (default: the creature you control). The "
                 "tool spends the points and completes the level-up; with `showMs` above 0 it shows the level-up "
                 "screen meanwhile, where the screen can open. Pass exactly one of:\n\n"
                 "- `offers: true`: returns what the screen offers and spends nothing: `points`, `attributes`, and "
                 "the class's `talents`, `spells` and `skills`, each a list of abilities with `id`, `name` and "
                 "`state` (`has`, `learnable` now, or `not yet`); and the `specializations`, each with `id`, `name` "
                 "and `state` (`has`, `learnable`, `locked` or `no point to spend`).\n"
                 "- The points to spend: `specialization`, `abilities` and `attributes`, in any combination. A "
                 "specialization is taken first, so its abilities can be taken in the same call. List `abilities` "
                 "with the first of a chain before the next. The first one refused (had already, locked, not "
                 "offered, or no point left) stops the tool, saying why; those taken before it are kept "
                 "(`taken`).\n"
                 "- `auto: true`: the screen's Auto Level Up, which spends every point as the game chooses for the "
                 "class (all a follower needs), repeating while points are left.\n\n"
                 "The answer includes `levelled`, `reason`, `before` and `after` (level and points to spend), the "
                 "`attributes` raised and by how much, the talents, spells and skills `gained`, and a `cursor`; "
                 "`screen` says why the screen was not shown, where it was asked for and did not open. It "
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
                          {"description", "How long to show the level-up screen before the points are spent and "
                                          "after, in milliseconds, so whoever watches sees it. Default 1000; 0 "
                                          "for no screen."}}}}}},
                 Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "levelup");
            return Levelling(bench, args, show).Run();
        });
}

} // namespace eluvian
