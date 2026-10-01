#include "core/Travel.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// One journey: its arguments, its clock, and the transcript it answers with.
class Journey
{
  public:
    Journey(Bench &bench, const Args &args)
        : bench_(bench), deadline_(Clock::now() + milliseconds(args.Integer("budgetMs", 60000, 1000, 600000))),
          maxDoors_(static_cast<std::size_t>(args.Integer("maxDoors", 4, 0, 20))), within_(args.Number("within", 1.5))
    {
        if (args.Has("target") == args.Has("point"))
        {
            throw ToolError("travel: give one of `target` (a creature or object id) or `point`");
        }
        if (args.Has("target"))
        {
            where_["target"] = args.Raw().at("target");
        }
        else
        {
            where_["point"] = args.Raw().at("point");
        }
        if (args.Has("id"))
        {
            who_ = args.String("id");
        }
    }

    Json Run()
    {
        const Json game = bench_.Invoke("inspect", Json{{"kind", "game"}}).data;
        const std::string controlled = game.value(Json::json_pointer("/controlled/id"), std::string());
        if (who_.empty())
        {
            who_ = controlled;
        }
        if (who_.empty())
        {
            throw ToolError("travel: nobody is controlled and no `id` was given");
        }
        // A use by the controlled creature goes by click, which chooses what
        // the object takes; another's goes by command and must name it.
        byClick_ = who_ == controlled;

        std::string lastBlocker;
        for (int step = 0; step < 4 * (static_cast<int>(maxDoors_) + 2); ++step)
        {
            if (const auto reason = NotMine())
            {
                return End(false, *reason);
            }
            Json ask = where_;
            ask.update(Json{{"kind", "geometry"}, {"id", who_}, {"walk", true}});
            const Json geometry = bench_.Invoke("inspect", ask).data;
            const Json walk = geometry.value("walk", Json::object());
            const double distance = geometry.value("distance", 0.0);
            answer_["distance"] = distance;
            if (distance <= within_)
            {
                return End(true, "there");
            }
            if (walk.value("found", false))
            {
                const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", geometry.at("to")}},
                                          "MoveToLocation", "move");
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out while walking"));
                }
                if (*result == "success")
                {
                    continue; // measured again above: there, or stopped short of it
                }
                if (*result != "COMMAND_FAILED_PATH_ACTION_REQUIRED")
                {
                    return End(false, "the walk ended " + *result);
                }
                continue; // something shut on the way: asked again from where it stopped
            }
            const std::string blocker = walk.value(Json::json_pointer("/blocker/id"), std::string());
            if (!blocker.empty())
            {
                if (doors_.size() >= maxDoors_)
                {
                    return End(false, "more shut on the way than `maxDoors` allows; the next is " + blocker);
                }
                if (blocker == lastBlocker)
                {
                    return End(false, "what is in the way did not open: " + blocker);
                }
                lastBlocker = blocker;
                Json use = {{"action", "use"}, {"id", who_}, {"target", blocker}};
                if (!byClick_)
                {
                    use["via"] = "command";
                    use["objectAction"] = "open";
                }
                const auto result = Order(use, "UseObject", "use " + blocker);
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out at " + blocker));
                }
                doors_.push_back(walk.at("blocker"));
                continue;
            }
            // No walk and nothing named in the way: go as far as the search
            // got, once, and ask from there; a creature standing at a door
            // gets the pathfinder's own word for it.
            if (walk.contains("reached") && !walk.at("reached").is_null() && !wentToReached_)
            {
                wentToReached_ = true;
                const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", walk.at("reached")}},
                                          "MoveToLocation", "move as far as the search got");
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out while walking"));
                }
                continue;
            }
            return End(false, "no walk there: " + walk.value("result", std::string("the search found none")));
        }
        return End(false, "too many steps without arriving");
    }

  private:
    [[nodiscard]] milliseconds Left() const
    {
        return std::max(milliseconds(0), std::chrono::duration_cast<milliseconds>(deadline_ - Clock::now()));
    }

    // Why the journey is no longer this tool's to make, if it is not.
    std::optional<std::string> NotMine()
    {
        if (Left().count() == 0)
        {
            return "the time ran out";
        }
        const Json health = bench_.Invoke("health", Json::object()).data;
        const std::string mode = health.value("mode", std::string());
        answer_["mode"] = mode;
        if (mode == "combat")
        {
            return "a fight began";
        }
        if (mode == "dialog" || mode == "cutscene")
        {
            return "a " + std::string(mode == "dialog" ? "conversation" : "cutscene") + " took the screen";
        }
        if (mode == "loading")
        {
            return "an area is loading";
        }
        if (health.value("paused", false))
        {
            return "the game is paused";
        }
        return std::nullopt;
    }

    // Gives an order and waits for that command's end; a command another
    // cleared is not the end waited for. Nothing if the time ran out, or if
    // the journey was taken from it meanwhile, which `stopped_` then says.
    std::optional<std::string> Order(const Json &order, const char *type, std::string what)
    {
        const Json given = bench_.Invoke("order", order).data;
        auto since = given.value("cursor", bench_.Events().Next());
        const auto filter =
            EventFilter::FromArgs(Json{{"topic", "eluvian.command.ended"}, {"match", {{"id", who_}, {"type", type}}}});
        while (Left().count() > 0)
        {
            const auto event = bench_.Events().WaitFor(since, filter, std::min(Left(), milliseconds(2000)));
            if (!event)
            {
                // Nothing in two seconds: a fight, a pause or a conversation
                // may have taken the creature's command with it.
                if (auto reason = NotMine())
                {
                    steps_.push_back(Json{{"did", what}, {"result", *reason}});
                    stopped_ = std::move(reason);
                    return std::nullopt;
                }
                continue;
            }
            since = event->seq + 1;
            std::string result = event->payload.value("result", std::string());
            if (result == "COMMAND_FAILED_COMMAND_CLEARED")
            {
                continue;
            }
            steps_.push_back(Json{{"did", std::move(what)}, {"result", result}});
            return result;
        }
        return std::nullopt;
    }

    Json End(bool arrived, std::string reason)
    {
        answer_["arrived"] = arrived;
        answer_["reason"] = std::move(reason);
        answer_["who"] = who_;
        answer_["opened"] = doors_;
        answer_["steps"] = steps_;
        answer_["cursor"] = bench_.Events().Next();
        return answer_;
    }

    Bench &bench_;
    Clock::time_point deadline_;
    std::size_t maxDoors_;
    double within_;
    Json where_ = Json::object();
    std::string who_;
    bool byClick_ = false;
    bool wentToReached_ = false;
    std::optional<std::string> stopped_;
    Json doors_ = Json::array();
    Json steps_ = Json::array();
    Json answer_ = Json::object();
};

} // namespace

void AddTravelTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{"travel",
                 "Walk a creature to a `target` (a creature or object id) or a `point`, opening what is shut on the "
                 "way: it asks for the walk, uses the door in the way, waits, and goes on, up to `maxDoors` "
                 "(default 4) and `budgetMs` (default 60000). `id` is who walks, default the creature controlled. "
                 "Answers `arrived` (within `within` metres, default 1.5), `reason`, the `distance` left, what it "
                 "`opened`, and each step with how it ended. It stops and says why when the journey is no longer "
                 "its to make: a fight began, a conversation or cutscene took the screen, the game is paused. "
                 "Opening a door may start a fight or a plot's step; the events from `cursor` say what followed.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"id", {{"description", "Who walks: a creature id; default the one controlled"}}},
                        {"target", {{"description", "A creature or object id to walk to"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
                        {"within", {{"type", "number"}, {"description", "Metres that count as there; default 1.5"}}},
                        {"maxDoors", {{"type", "integer"}, {"minimum", 0}, {"maximum", 20}}},
                        {"budgetMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}}}}},
                 Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "travel");
            return Journey(bench, args).Run();
        });
}

} // namespace eluvian
