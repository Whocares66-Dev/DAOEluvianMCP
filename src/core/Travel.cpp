#include "core/Travel.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// How much shorter a move must leave the walk to have brought its walker
// nearer, in metres.
constexpr double kNoNearer = 0.25;

// A command's end when another cleared it from the walker's queue.
constexpr const char *kCleared = "COMMAND_FAILED_COMMAND_CLEARED";

// One journey: its arguments, its clock, and the transcript it answers with.
class Journey
{
  public:
    Journey(Bench &bench, const Args &args)
        : bench_(bench), deadline_(Clock::now() + milliseconds(args.Integer("budgetMs", 60000, 1000, 600000))),
          maxDoors_(static_cast<std::size_t>(args.Integer("maxDoors", 4, 0, 20))), within_(args.Number("within", 1.5))
    {
        const int given = (args.Has("target") ? 1 : 0) + (args.Has("point") ? 1 : 0) + (args.Has("exit") ? 1 : 0) +
                          (args.Has("area") ? 1 : 0);
        if (given != 1)
        {
            throw ToolError("travel: give one of `target` (a creature or object id), `point`, `exit` (an id "
                            "from inspect kind=exits) or `area` (an area's tag)");
        }
        if (args.Has("target"))
        {
            where_["target"] = args.Raw().at("target");
        }
        else if (args.Has("point"))
        {
            where_["point"] = args.Raw().at("point");
        }
        else if (args.Has("exit"))
        {
            exit_ = args.String("exit");
        }
        else
        {
            area_ = args.String("area");
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
        const bool leaving = !exit_.empty() || !area_.empty();
        if (leaving)
        {
            if (!byClick_)
            {
                throw ToolError("travel: only the creature controlled takes the party to another area; leave out "
                                "`id`, or control that creature first");
            }
            ChooseExit();
            fromArea_ = game.value(Json::json_pointer("/area/tag"), std::string());
        }
        Json walked = Walk();
        if (!leaving || !walked.value("arrived", false))
        {
            return walked;
        }
        return Cross();
    }

  private:
    // The exit to go by: the one named, or the nearest active one that
    // leads to the area named, from inspect kind=exits.
    void ChooseExit()
    {
        const Json listed = bench_.Invoke("inspect", Json{{"kind", "exits"}, {"near", who_}}).data;
        const Json exits = listed.value("exits", Json::array());
        std::string places;
        for (const auto &exit : exits)
        {
            const std::string id = exit.value("id", std::string());
            const std::string area = exit.value(Json::json_pointer("/leadsTo/area"), std::string());
            if ((!exit_.empty() && id == exit_) || (exit_.empty() && area == area_ && exit.value("active", true)))
            {
                exitAt_ = exit;
                where_["target"] = id;
                answer_["exit"] = exit;
                return;
            }
            places += (places.empty() ? "" : ", ") + area;
        }
        if (!exit_.empty())
        {
            throw ToolError("travel: " + exit_ + " is not one of this area's exits; inspect kind=exits lists them");
        }
        throw ToolError("travel: no active exit here leads to " + area_ + "; this area's lead to " +
                        (places.empty() ? std::string("nowhere") : places));
    }

    // Through the exit: used, then waited on until the other area is in
    // place (the scripts' area-loaded event, or the game's area changed).
    // Not the command's end, which a party leaving its area need not send.
    Json Cross()
    {
        const std::string exit = where_.at("target").get<std::string>();
        const std::string to = exitAt_.value(Json::json_pointer("/leadsTo/area"), std::string());
        auto since = bench_.Events().Next();
        measured_ = false;
        if (!Ask("order", Json{{"action", "use"}, {"id", who_}, {"target", exit}}))
        {
            return End(false, *stopped_);
        }
        steps_.push_back(Json{{"did", "use " + exit}, {"result", "asked"}});
        const auto loaded = EventFilter::FromArgs(Json{{"topic", "eluvian.script.area_loaded"}});
        while (Left().count() > 0)
        {
            const auto event = bench_.Events().WaitFor(since, loaded, std::min(Left(), milliseconds(2000)));
            if (event)
            {
                since = event->seq + 1;
            }
            const Json health = bench_.Invoke("health", Json::object()).data;
            const std::string mode = health.value("mode", std::string());
            answer_["mode"] = mode;
            if (mode == "loading")
            {
                continue;
            }
            // Refused as the other area comes in: asked again.
            const auto read = Ask("inspect", Json{{"kind", "game"}});
            if (!read)
            {
                continue;
            }
            const Json &game = *read;
            const std::string area = game.value(Json::json_pointer("/area/tag"), std::string());
            if (!area.empty() && area != fromArea_)
            {
                answer_["area"] = game.at("area");
                steps_.push_back(Json{{"did", "wait for " + area}, {"result", "loaded"}});
                return End(true, "through to " + game.value(Json::json_pointer("/area/name"), area) +
                                     (area == to || to.empty() ? "" : " (the exit named " + to + ")"));
            }
            if (game.contains("popup") && !game.at("popup").is_null())
            {
                return End(false, "the game asks before leaving: " + game.at("popup").dump() +
                                      "; inspect kind=gui reads it and game action=press answers it");
            }
            if (mode == "combat")
            {
                return End(false, "a fight began before the party left");
            }
            if (mode == "dialog" || mode == "cutscene")
            {
                return End(false, "a " + std::string(mode == "dialog" ? "conversation" : "cutscene") +
                                      " took the screen before the party left");
            }
            if (mode != "explore" && !mode.empty())
            {
                return End(false, "the game is in mode " + mode +
                                      " and the party has not left (an exit to the "
                                      "world map opens it)");
            }
        }
        return End(false, "the party did not leave in the time given; the exit was used");
    }

    Json Walk()
    {
        std::string lastBlocker;
        std::optional<double> lastWalk; // the walk's length when the last move along it was given
        for (int step = 0; step < 4 * (static_cast<int>(maxDoors_) + 2); ++step)
        {
            if (const auto reason = NotMine())
            {
                return End(false, *reason);
            }
            Json ask = where_;
            ask.update(Json{{"kind", "geometry"}, {"id", who_}, {"walk", true}, {"waypoints", true}});
            const auto measured = Ask("inspect", ask);
            if (!measured)
            {
                return End(false, *stopped_);
            }
            const Json &geometry = *measured;
            const Json walk = geometry.value("walk", Json::object());
            const double distance = geometry.value("distance", 0.0);
            answer_["distance"] = distance;
            measured_ = true;
            if (distance <= within_)
            {
                return End(true, "there");
            }
            if (walk.value("found", false))
            {
                // The walk's own end, not the target's place: a door or a
                // mirror stands where nothing can, the search ends as near
                // as a creature fits, and a move to the thing itself ended
                // COMMAND_FAILED_INVALID_PATH 6 m and 20 m short (2026-10-01).
                const double length = walk.value("length", distance);
                if (length < 0.5)
                {
                    return End(true, "as near as the walk goes");
                }
                // A walk whose end the creature cannot stand on, under a
                // thing that fills its place or hangs overhead: each move
                // there ends where it began, and the walk asked again is as
                // long. The Elven Ruins' cocoon, 4 m up, took 22 such moves
                // until the steps ran out, where a use from below worked at
                // once (#4, 2026-10-02). A thing is used from as near as it
                // goes; a point is not reached.
                if (lastWalk && length > *lastWalk - kNoNearer)
                {
                    if (where_.contains("target"))
                    {
                        return End(true, "as near as the walk goes: the last move brought it no nearer");
                    }
                    return End(false, "the walk goes no nearer: the last move brought it no nearer");
                }
                lastWalk = length;
                const Json waypoints = walk.value("waypoints", Json::array());
                const Json &to = waypoints.is_array() && !waypoints.empty() ? waypoints.back() : geometry.at("to");
                const auto result =
                    Order(Json{{"action", "move"}, {"id", who_}, {"point", to}}, "MoveToLocation", "move");
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out while walking"));
                }
                if (*result == "success")
                {
                    continue; // measured again above: there, or stopped short of it
                }
                if (*result == kCleared)
                {
                    lastWalk.reset(); // not a move made: it does not say the walk goes no nearer
                    continue;
                }
                if (*result != "COMMAND_FAILED_PATH_ACTION_REQUIRED")
                {
                    return End(false, "the walk ended " + *result);
                }
                continue; // something shut on the way: asked again from where it stopped
            }
            const std::string blocker = walk.value(Json::json_pointer("/blocker/id"), std::string());
            if (!blocker.empty() && blocker == where_.value("target", std::string()))
            {
                // The target itself is what stands in the way: it is reached,
                // not a door to open on the way (a chest in a narrow place was
                // opened as one, #21).
                return End(true, "as near as the walk goes: the target itself stands in the way");
            }
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
                lastWalk.reset(); // the way is another once it is open
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
                if (*result == kCleared)
                {
                    lastBlocker.clear(); // not a use made: it may be asked again
                    continue;
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
                lastWalk.reset();
                const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", walk.at("reached")}},
                                          "MoveToLocation", "move as far as the search got");
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out while walking"));
                }
                wentToReached_ = *result != kCleared;
                continue;
            }
            return End(false, "no walk there: " + walk.value("result", std::string("the search found none")));
        }
        return End(false, "too many steps without arriving");
    }

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

    // Gives an order and waits for that command's end. A command cleared is
    // not that end if another of the walker's follows, as the click's use is
    // cleared and given again as a container opens; one that nothing follows
    // for two seconds is, and is answered kCleared: the game's settling after
    // a load cleared a move and gave it no other, and the wait ran out the
    // whole budget (#8, 2026-10-02). Nothing if the time ran out, or if the
    // journey was taken from it meanwhile, which `stopped_` then says.
    std::optional<std::string> Order(const Json &order, const char *type, std::string what)
    {
        measured_ = false;
        const auto ordered = Ask("order", order);
        if (!ordered)
        {
            steps_.push_back(Json{{"did", what}, {"result", *stopped_}});
            return std::nullopt;
        }
        const Json &given = *ordered;
        auto since = given.value("cursor", bench_.Events().Next());
        const auto filter =
            EventFilter::FromArgs(Json{{"topic", "eluvian.command.ended"}, {"match", {{"id", who_}, {"type", type}}}});
        bool cleared = false;
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
                if (cleared)
                {
                    steps_.push_back(Json{{"did", std::move(what)}, {"result", kCleared}});
                    return std::string(kCleared);
                }
                continue;
            }
            since = event->seq + 1;
            std::string result = event->payload.value("result", std::string());
            if (result == kCleared)
            {
                cleared = true;
                continue;
            }
            steps_.push_back(Json{{"did", std::move(what)}, {"result", result}});
            return result;
        }
        return std::nullopt;
    }

    // A call the game refuses for its state mid-journey stops the journey
    // with the refusal for its reason, the steps and the doors kept: a door
    // whose plot put the game in its loading mode a frame after its use
    // ended had the next move refused, and the refusal thrown up lost them
    // (#7, 2026-10-02). Nothing if refused, and `stopped_` says why.
    std::optional<Json> Ask(std::string_view tool, const Json &args)
    {
        try
        {
            return bench_.Invoke(tool, args).data;
        }
        catch (const ToolError &e)
        {
            stopped_ = e.what();
        }
        catch (const FrameTimeout &e)
        {
            stopped_ = std::string("the game did not answer ") + std::string(tool) + ": " + e.what();
        }
        return std::nullopt;
    }

    // Where a journey stops after an order that nothing has measured since,
    // the distance is measured again: a fight that stopped a walk part way
    // answered the measure from before the move, 43.78 m with about 8.3 m
    // left (#3, 2026-10-02). With no frame to measure in (an area loading)
    // no distance is said rather than a stale one.
    void MeasureAgain()
    {
        try
        {
            Json ask = where_;
            ask.update(Json{{"kind", "geometry"}, {"id", who_}});
            answer_["distance"] = bench_.Invoke("inspect", ask).data.at("distance");
        }
        catch (const std::exception &)
        {
            answer_.erase("distance");
        }
    }

    Json End(bool arrived, std::string reason)
    {
        if (!arrived && !measured_)
        {
            MeasureAgain();
        }
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
    std::string exit_;     // `exit`: the exit's id
    std::string area_;     // `area`: the area to reach, by an exit that leads there
    Json exitAt_;          // the exit chosen, as inspect kind=exits gave it
    std::string fromArea_; // the area's tag before the crossing
    std::string who_;
    bool byClick_ = false;
    bool wentToReached_ = false;
    bool measured_ = false; // whether `distance` was measured after the last order
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
                 "Answers `arrived` (within `within` metres, default 1.5, or as near as a creature can stand to a "
                 "thing that fills its own place or hangs overhead: a door, a chest, a cocoon; then use it from "
                 "there), `reason`, the `distance` left, what it "
                 "`opened`, and each step with how it ended. It stops and says why when the journey is no longer "
                 "its to make: a fight began, a conversation or cutscene took the screen, the game is paused. "
                 "Opening a door may start a fight or a plot's step; the events from `cursor` say what followed. "
                 "To another area: `exit` (an id from inspect kind=exits) or `area` (an area's tag: the nearest "
                 "active exit that leads there). The creature controlled walks to it, uses it, and the answer "
                 "comes once the other `area` is in place; it stops and says so if the game asks first (a "
                 "message box), opens the world map, or starts a fight or a conversation. The game may "
                 "autosave on the way.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"id", {{"description", "Who walks: a creature id; default the one controlled"}}},
                        {"target", {{"description", "A creature or object id to walk to"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
                        {"exit", {{"type", "string"}, {"description", "An exit's id, from inspect kind=exits"}}},
                        {"area",
                         {{"type", "string"},
                          {"description", "An area's tag, to go to by the nearest exit that leads there"}}},
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
