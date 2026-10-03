#include "core/LevelUp.h"

#include "core/Args.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
        if (!args.Bool("auto", false))
        {
            throw ToolError("levelup: only `auto`: true for now, the level-up screen's Auto Level Up, which spends "
                            "every point as the game chooses; choosing the points is to come (#5)");
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
        const Json first = bench_.Invoke("inspect", Json{{"kind", "character"}, {"id", id_}}).data;
        if (!first.value("canLevelUp", false))
        {
            throw ToolError("levelup: " + first.value("name", id_) +
                            " has no level-up points to spend; inspect kind=character says `canLevelUp` and "
                            "`pointsToSpend`");
        }
        const Json abilities = bench_.Invoke("inspect", Json{{"kind", "abilities"}, {"id", id_}}).data;
        answer_["creature"] = Json{{"id", id_}, {"name", first.value("name", std::string())}};
        answer_["before"] = Sheet(first);
        const std::string n = std::to_string(*number);
        Json last = first;
        for (int round = 0; round < kMostRounds; ++round)
        {
            if (!Send("GUI.GoToLevelUp " + n + " 0"))
            {
                return End(false, refused_);
            }
            if (!Screen(true))
            {
                return End(false, "the level-up screen did not open: " + refused_ +
                                      " (in a fight or a conversation it may not)");
            }
            // The screen up a moment before and after its Auto Level Up, for
            // whoever watches: in one call it came and went in under a
            // second, never seen (the user, 2026-10-03, #14).
            std::this_thread::sleep_for(show_);
            if (!Send("GUILevelUp.AutoLevelUp " + n))
            {
                return End(false, refused_);
            }
            std::this_thread::sleep_for(show_);
            if (!Send("GUILevelUp.CharGenEnd " + n + " 1 1") || !Send("GUI.HideFullScreenGUI"))
            {
                return End(false, refused_);
            }
            if (!Screen(false))
            {
                return End(false, "the level-up screen did not close: " + refused_ +
                                      "; input {\"message\": \"GUI.HideFullScreenGUI\"} closes it");
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
        answer_["after"] = Sheet(last);
        answer_["attributes"] = Raised(first, last);
        if (const auto after = Ask("inspect", Json{{"kind", "abilities"}, {"id", id_}}))
        {
            answer_["gained"] = Gained(abilities, *after);
        }
        if (PointsLeft(last) >= PointsLeft(first))
        {
            return End(false, "the screen's Auto Level Up spent nothing");
        }
        return End(true, last.value("canLevelUp", false) ? "levelled; points are left that the screen's Auto Level "
                                                           "Up did not spend"
                                                         : "levelled: every point spent");
    }

  private:
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
            "Spend a creature's level-up points as the game's level-up screen does, so its rules hold: `id` "
            "(default the creature controlled) and `auto`: true, the screen's Auto Level Up, which spends "
            "every point as the game chooses for the class (all a follower needs; choosing the points "
            "yourself is to come). It opens the screen for the creature, presses Auto Level Up, ends it "
            "complete and closes it, again while points are left. Answers `levelled`, `reason`, `before` and "
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
                   {"showMs", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}},
                 {"required", {"auto"}}},
            Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "levelup");
            return Levelling(bench, args, show).Run();
        });
}

} // namespace eluvian
