#include "core/Travel.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
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

// How much shorter a move must leave the walk to have brought its walker
// nearer, in metres.
constexpr double kNoNearer = 0.25;
// How near the walker must be for the target named in its own way to count
// as reached (#46): a thing is used from a few metres.
constexpr double kNearOwnWay = 4.0;
// How much nearer a leg to the search's end must bring the walker for another (#82).
constexpr double kLegGain = 2.0;

// How near a target a walk that ended short is said to be in a use's
// reach, the use's click walking the rest (#80).
constexpr double kUseReach = 5.0;

// How long after an exit's use ended in success, with no load begun, it
// is used once more: a key-locked exit's first use unlocks it, and only
// the second goes through (den300ip_to_hospice_side, turn-based-agent,
// 2026-10-06, #96).
constexpr milliseconds kUnlockGrace{1500};
// Where the search finds no walk and names nothing, steps this long toward
// the goal, at most this many, each at least kStraightGain nearer (#53).
constexpr double kStraightStep = 0.5;
constexpr int kStraightSteps = 16;
constexpr double kStraightGain = 0.1;

// A command's end when another cleared it from the walker's queue.
constexpr const char *kCleared = "COMMAND_FAILED_COMMAND_CLEARED";
// A command that ended with no end said: a move measured again, a use made.
constexpr const char *kUnseen = "ended unseen";
// The game's pick level for a lock no one picks: the story or a key opens it.
constexpr int kCannotPick = 10000;
// A door's locked state (PLC_STATE_DOOR_LOCKED; a container's is 1 too):
// only a locked door needs its key or its pick (#45).
constexpr int kLocked = 1;
// How far back from an exit a failed use is tried again from, in metres.
constexpr double kStepBack = 4.0;
// A placeable's use that leaves the area (placeable_interactions' row 15,
// core/Placeables.h names them).
constexpr int kActionAreaTransition = 15;

// One journey: its arguments, its clock, and the transcript it answers with.
class Journey
{
  public:
    Journey(Bench &bench, const Args &args)
        : bench_(bench), deadline_(Clock::now() + milliseconds(args.Integer("budgetMs", 60000, 1000, 600000))),
          maxDoors_(static_cast<std::size_t>(args.Integer("maxDoors", 4, 0, 20))), within_(args.Number("within", 1.5)),
          fightNear_(args.Number("fightNear", 20))
    {
        // The bus's place as the journey begins, so that the events from it
        // are the journey's: a door's fight, a plot's step, the walk's
        // commands. It was taken at the end, and read nothing of them
        // (noticed writing the mutants, 2026-10-03, #13).
        answer_["cursor"] = bench_.Events().Next();
        const int given = (args.Has("target") ? 1 : 0) + (args.Has("point") ? 1 : 0) + (args.Has("exit") ? 1 : 0) +
                          (args.Has("area") ? 1 : 0);
        if (args.Has("to"))
        {
            to_ = args.Raw().at("to");
        }
        // `to`, a pin of the world map: alone with the map up, or with the
        // `exit` that opens it (#24).
        const bool fits = to_.is_null() ? given == 1 : given == 0 || (given == 1 && args.Has("exit"));
        if (!fits)
        {
            throw ToolError("travel: give one of `target` (a creature or object id), `point`, `exit` (an id "
                            "from inspect kind=exits) or `area` (an area's tag); or `to`, a pin of the world map "
                            "(inspect kind=worldmap), with `exit` (the area's way to the map) if the map is not up");
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
        else if (args.Has("area"))
        {
            area_ = args.String("area");
        }
        if (args.Has("id"))
        {
            who_ = args.String("id");
        }
        if (args.Has("upTo"))
        {
            if (!where_.contains("target") && !where_.contains("point"))
            {
                throw ToolError("travel: `upTo` cuts a walk to a `target` or a `point`");
            }
            upTo_ = args.Number("upTo");
            if (*upTo_ <= 0)
            {
                throw ToolError("travel: `upTo` is a length in metres, more than 0");
            }
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
        startArea_ = game.value(Json::json_pointer("/area/tag"), std::string());
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
        if (!to_.is_null() && exit_.empty())
        {
            return Map(false);
        }
        Json walked = Walk();
        const std::string why = walked.value("reason", std::string());
        if (leaving && !walked.value("arrived", false) && why.starts_with("no walk there"))
        {
            // An exit the search cannot walk to: one of the camp's two World
            // Map exits answered "no walkable ground there", the other walked,
            // and the windmill's door up from the Secret Passage answered
            // "none" twice, where an order to use it walked there at once
            // (turn-based-agent, 2026-10-03, #27). A click's use walks its
            // user there as the game's own click does, so the exit is used
            // from here.
            steps_.push_back(Json{{"did", "walk to the exit"}, {"result", why + ": used from here"}});
            return Cross();
        }
        if (!leaving || !walked.value("arrived", false))
        {
            return walked;
        }
        Json crossed = CrossAny();
        if (to_.is_null() || crossed.value("arrived", false))
        {
            return crossed;
        }
        return Map(true); // the exit opened the world map, if it did: on to the pin
    }

  private:
    // Across by the exit chosen, and, for an `area`, by the next active
    // exit to it where that one's use fails for good: of the Gnawed
    // Noble Tavern's two exits to the market, 1 m apart, one failed from
    // near and a step back while the other's click went through at once
    // (turn-based-agent, 2026-10-09, #101). Each is tried once.
    Json CrossAny()
    {
        Json crossed = Cross();
        while (!area_.empty() && !crossed.value("arrived", false) &&
               crossed.value("reason", std::string()).starts_with("the use of "))
        {
            tried_.insert(where_.at("target").get<std::string>());
            if (!ChooseExit())
            {
                return crossed;
            }
            steps_.push_back(
                Json{{"did", "another exit to " + area_}, {"result", where_.at("target").get<std::string>()}});
            lastWalk_.reset();
            legFrom_.reset();
            lastEnd_.reset();
            wentToReached_ = false;
            const Json walked = Walk();
            if (!walked.value("arrived", false) && !walked.value("reason", std::string()).starts_with("no walk there"))
            {
                return walked;
            }
            crossed = Cross();
        }
        return crossed;
    }

    // Across the world map to a pin, as a click on it: the map's script
    // sends "GameModeController.AreaTransition <where the party is> <the
    // pin's index>" (worldmap.gfx, read and tried in play by
    // turn-based-agent, 2026-10-03, #24), and the party goes, at times
    // through a random encounter on the way. Waited for until another area
    // is in place; an encounter's area is said as such.
    Json Map(bool opening)
    {
        auto map = Ask("inspect", Json{{"kind", "worldmap"}});
        // After its exit the map is waited for a little: it opens a moment
        // after the exit is used.
        const auto until = Clock::now() + std::min(Left(), milliseconds(5000));
        while (opening && map && !map->value("up", false) && Clock::now() < until)
        {
            std::this_thread::sleep_for(milliseconds(250));
            map = Ask("inspect", Json{{"kind", "worldmap"}});
        }
        if (opening && (!map || !map->value("up", false)))
        {
            return answer_; // the exit did something else, said by its crossing
        }
        if (!map)
        {
            return End(false, "the world map could not be read: " + stopped_.value_or(""));
        }
        if (!map->value("up", false))
        {
            return End(false, "no map is up, the world's or the Fade's: travel with `exit` (the area's way to "
                              "the world map, inspect kind=exits) and `to` goes there and on; in the Fade, a "
                              "Fade Pedestal's use opens its map");
        }
        std::string others;
        const Json *pin = PinNamed(*map, others);
        if (pin == nullptr)
        {
            return NoPin(*map, others);
        }
        answer_["pin"] = *pin;
        const std::string name = pin->value("name", std::string());
        const std::string state = pin->value("state", std::string());
        if (state != "active")
        {
            return End(false,
                       name + " is " + state + " on the world map, not a place to go now; the active ones: " + others);
        }
        auto since = bench_.Events().Next();
        if (auto refused = ClickPin(*map, *pin, name))
        {
            return std::move(*refused);
        }
        // The pin of the area the party is in: the game closes the map
        // and loads nothing, so no other area comes to wait for
        // (turn-based-agent, the market's own pin, 2026-10-06, #95).
        if (!startArea_.empty() && pin->value("area", std::string()) == startArea_)
        {
            answer_["already"] = true;
            return End(true, "the party is in " + name + " already: its pin only closed the map");
        }
        return Arrive(since, *pin, name);
    }

    // No pin `to` names on the map shown. In a city, where the map up may
    // be the city's and the world's the other, swapped to as the map's
    // toggle button does and looked for again there, once (#42); else said,
    // of both maps when swapped.
    Json NoPin(const Json &map, const std::string &others)
    {
        const std::string shown = map.value("shown", Json()).is_string() ? map.at("shown").get<std::string>() : "";
        const std::string other = map.value("other", Json()).is_string() ? map.at("other").get<std::string>() : "";
        if (!other.empty() && !swapped_)
        {
            swapped_ = true;
            if (!Ask("input", Json{{"message", "WorldMap.WorldMapSwapMaps"}}))
            {
                return End(false, *stopped_);
            }
            steps_.push_back(Json{{"did", "swap to the " + other + " map"}, {"result", "asked"}});
            const auto swapUntil = Clock::now() + std::min(Left(), milliseconds(3000));
            while (Clock::now() < swapUntil)
            {
                const auto again = Ask("inspect", Json{{"kind", "worldmap"}});
                if (again && again->value("shown", Json()) != map.value("shown", Json()))
                {
                    break;
                }
                std::this_thread::sleep_for(milliseconds(250));
            }
            answer_["swapped"] = true;
            return Map(false);
        }
        if (auto byCity = ByCity(map))
        {
            return std::move(*byCity);
        }
        std::string where = shown.empty() ? std::string("the world map") : "the " + shown + " map";
        if (swapped_)
        {
            where += " or the other";
        }
        return End(false, "no pin " + to_.dump() + " on " + where + "; the active ones: " + others);
    }

    // The map of the designer maps (inspect kind=maps) that holds the pin
    // `to` names: its tag and the areas its pins lead to. Nothing if none.
    std::optional<std::pair<std::string, std::set<std::string>>> MapHolding()
    {
        const auto maps = Ask("inspect", Json{{"kind", "maps"}});
        if (!maps)
        {
            return std::nullopt;
        }
        for (const Json &map : maps->value("maps", Json::array()))
        {
            const Json pins = map.value("pins", Json::array());
            if (std::ranges::none_of(pins, [this](const Json &pin) { return Names(pin); }))
            {
                continue;
            }
            std::set<std::string> areas;
            for (const Json &pin : pins)
            {
                areas.insert(pin.value("area", std::string()));
            }
            return std::make_pair(map.value("tag", std::string()), std::move(areas));
        }
        return std::nullopt;
    }

    // A pin on a map not up: a city's district, from outside the city
    // (#91). The wide map leads into a city by an area the two share: its
    // Denerim pin goes to den200ar_market, a pin of denerim.map too
    // (lib-agent, 2026-10-09). So the map holding the pin is found, and the
    // shown map's active pin into one of its areas; the party goes there,
    // uses the area's way to the map, which opens the city's, and takes the
    // pin there. Once a journey; nothing if there is no such way.
    std::optional<Json> ByCity(const Json &map)
    {
        if (cityTried_)
        {
            return std::nullopt;
        }
        cityTried_ = true;
        const auto holding = MapHolding();
        if (!holding)
        {
            return std::nullopt;
        }
        const Json pins = map.value("pins", Json::array());
        const auto entry = std::ranges::find_if(pins, [&holding](const Json &pin) {
            return pin.value("state", std::string()) == "active" &&
                   holding->second.contains(pin.value("area", std::string()));
        });
        if (entry == pins.end())
        {
            return std::nullopt;
        }
        steps_.push_back(Json{{"did", "to " + to_.dump() + " by " + entry->value("name", std::string())},
                              {"result", "a pin of the " + holding->first + " map"}});
        const Json wanted = to_;
        to_ = entry->value("tag", std::string());
        const Json there = Map(false);
        to_ = wanted;
        if (!there.value("arrived", false))
        {
            return std::make_optional(there);
        }
        return std::make_optional(OpenCityMap());
    }

    // In the city's entry area: its way to the map used, which opens the
    // city's, and the pin `to` names taken there.
    Json OpenCityMap()
    {
        const auto listed = Ask("inspect", Json{{"kind", "exits"}, {"near", who_}});
        const Json exits = listed ? listed->value("exits", Json::array()) : Json::array();
        const auto way = std::ranges::find_if(exits, [](const Json &exit) {
            return exit.value("active", true) && !exit.value("blocker", false) &&
                   exit.value(Json::json_pointer("/leadsTo/area"), std::string()) == "world_map";
        });
        if (way == exits.end())
        {
            return End(false, "in " + answer_.value(Json::json_pointer("/area/tag"), std::string()) +
                                  ", the way into the city, no exit leads to the map: travel with `exit` and `to` "
                                  "from one that does");
        }
        exitAt_ = *way;
        where_ = Json{{"target", way->value("id", std::string())}};
        answer_["exit"] = *way;
        fromArea_ = answer_.value(Json::json_pointer("/area/tag"), std::string());
        startArea_ = fromArea_;
        lastWalk_.reset();
        legFrom_.reset();
        lastEnd_.reset();
        wentToReached_ = false;
        Json walked = Walk();
        if (!walked.value("arrived", false) && !walked.value("reason", std::string()).starts_with("no walk there"))
        {
            return walked;
        }
        Cross();
        return Map(true);
    }

    // The pin `to` names, the active one where two share a name; the
    // active ones' names in `others`, for a refusal.
    const Json *PinNamed(const Json &map, std::string &others) const
    {
        static const Json kNone = Json::array();
        const Json &pins = map.contains("pins") ? map.at("pins") : kNone;
        const Json *pin = nullptr;
        std::string active;
        for (const auto &candidate : pins)
        {
            // Two pins may share a name: Redcliffe Village is pin 6 and, not
            // active, the climax's pin 14; by name the active one is meant
            // (turn-based-agent, 2026-10-03).
            const bool live = candidate.value("state", std::string()) == "active";
            if (Names(candidate) && (pin == nullptr || (live && pin->value("state", std::string()) != "active")))
            {
                pin = &candidate;
            }
            if (candidate.value("state", std::string()) == "active")
            {
                active += (active.empty() ? "" : ", ") + candidate.value("name", std::string());
            }
        }
        others = active.empty() ? std::string("none") : active;
        return pin;
    }

    // The pin clicked as its own click does: nothing, or the refusal.
    std::optional<Json> ClickPin(const Json &map, const Json &pin, const std::string &name)
    {
        // What the pin's own click does before the transition (worldmap.gfx,
        // MapPinIcon.PinClicked): the map marks itself closed by travel, and
        // the pin's index goes in the data tree. Without the mark the map
        // closed saying "not by travel", which at camp is the player backing
        // out: the followers went back to their places and the hero arrived
        // alone, four times of four from camp to the area it was entered
        // from; with it, all four arrived (turn-based-agent's reading and
        // trial, 2026-10-03, #30).
        const int index = pin.value("index", 0);
        // The map shown: the world's, or the Fade's own (fademap.gfx), whose
        // pin sets the index and sends the transition with no mark (#34).
        const std::string screen = map.value("screen", Json("WorldMap")).is_string()
                                       ? map.value("screen", std::string("WorldMap"))
                                       : std::string("WorldMap");
        if (map.value("map", std::string("world")) != "fade")
        {
            const auto marked =
                Ask("input", Json{{"screen", screen}, {"set", "WorldMap.m_bClosedByTravel"}, {"value", true}});
            steps_.push_back(
                Json{{"did", "mark the map closed by travel"},
                     {"result", marked && marked->value("done", false) ? "done" : stopped_.value_or("not done")}});
        }
        Ask("input", Json{{"screen", screen},
                          {"call", "ExternalCommands.SetValue"},
                          {"args", Json::array({"WorldMap.LastClickedPinIndex", index})}});
        const std::string click =
            "GameModeController.AreaTransition " + map.value("at", std::string()) + " " + std::to_string(index);
        const auto sent = Ask("input", Json{{"message", click}});
        if (!sent || !sent->value("sent", false))
        {
            return End(false, "the game took no " + click + (stopped_ ? ": " + *stopped_ : std::string()));
        }
        steps_.push_back(Json{{"did", "the world map's pin " + name}, {"result", "asked"}});
        return std::nullopt;
    }

    // Waited for until another area is in place: the pin's, or an
    // encounter's on the way.
    Json Arrive(std::uint64_t since, const Json &pin, const std::string &name)
    {
        const auto loaded = EventFilter::FromArgs(Json{{"topic", "eluvian.script.area_loaded"}});
        while (Left() > milliseconds(0))
        {
            if (const auto event = bench_.Events().WaitFor(since, loaded, std::min(Left(), milliseconds(500))))
            {
                since = event->seq + 1;
            }
            const Json health = bench_.Invoke("health", Json::object()).data;
            if (health.value("mode", std::string()) == "loading")
            {
                continue;
            }
            const auto read = Ask("inspect", Json{{"kind", "game"}});
            if (!read)
            {
                continue;
            }
            const Json &game = *read;
            const std::string area = game.value(Json::json_pointer("/area/tag"), std::string());
            if (area.empty() || area == startArea_ || area == fromArea_)
            {
                continue;
            }
            answer_["area"] = game.at("area");
            if (area == pin.value("area", std::string()))
            {
                return End(true, "through to " + name);
            }
            answer_["encounter"] = true;
            return End(false, "on the way to " + name + ", the party is in " +
                                  game.value(Json::json_pointer("/area/name"), area) +
                                  " (a random encounter, or the way in): travel with `to` again from its way "
                                  "to the world map, or with `area`");
        }
        return End(false, "the party did not reach " + name + " in the time given; the pin was asked");
    }

    // Whether a pin is the one `to` names: by index, or by name (any case)
    // or tag.
    bool Names(const Json &pin) const
    {
        if (to_.is_number_integer())
        {
            return pin.value("index", -1) == to_.get<int>();
        }
        if (!to_.is_string())
        {
            return false;
        }
        const std::string wanted = to_.get<std::string>();
        return Lower(pin.value("name", std::string())) == Lower(wanted) || pin.value("tag", std::string()) == wanted;
    }

    static std::string Lower(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // The exit to go by: the one named, or the nearest active one that
    // leads to the area named and was not tried yet, from inspect
    // kind=exits. False if no other is left, after the first.
    bool ChooseExit()
    {
        const Json listed = bench_.Invoke("inspect", Json{{"kind", "exits"}, {"near", who_}}).data;
        const Json exits = listed.value("exits", Json::array());
        std::string places;
        for (const auto &exit : exits)
        {
            const std::string id = exit.value("id", std::string());
            const std::string area = exit.value(Json::json_pointer("/leadsTo/area"), std::string());
            // A text blocker is never a way out (#32): asked for by id, it is
            // refused, and for an area the next exit is taken.
            const bool blocker = exit.value("blocker", false);
            if (blocker && id == exit_)
            {
                throw ToolError("travel: " + exit_ +
                                " is a text blocker: its use shows a line and leaves nothing; "
                                "inspect kind=exits lists the ways out");
            }
            if ((!exit_.empty() && id == exit_) ||
                (exit_.empty() && area == area_ && exit.value("active", true) && !blocker && !tried_.contains(id)))
            {
                exitAt_ = exit;
                where_["target"] = id;
                answer_["exit"] = exit;
                return true;
            }
            places += (places.empty() ? "" : ", ") + area;
        }
        if (!tried_.empty())
        {
            return false;
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
        // The use's own end, watched: the Windmill door's use from 1 m ended
        // COMMAND_FAILED at once and the crossing waited out its budget; from
        // 8 m back it worked (turn-based-agent, 2026-10-03, #29). A use that
        // fails is tried once more from a step back, then said.
        const auto useEnded = EventFilter::FromArgs(
            Json{{"topic", "eluvian.command.ended"}, {"match", {{"id", who_}, {"type", "UseObject"}}}});
        auto useSince = since;
        bool steppedBack = false;
        bool usedAgain = false;
        std::optional<Clock::time_point> succeeded;
        while (Left().count() > 0)
        {
            const auto event = bench_.Events().WaitFor(since, loaded, std::min(Left(), milliseconds(500)));
            if (event)
            {
                since = event->seq + 1;
            }
            if (const auto ended = bench_.Events().WaitFor(useSince, useEnded, milliseconds(0)))
            {
                useSince = ended->seq + 1;
                const std::string result = ended->payload.value("result", std::string());
                if (result.starts_with("COMMAND_FAILED") && result != kCleared)
                {
                    if (auto failed = UseAgain(exit, result, steppedBack, since, useSince))
                    {
                        return std::move(*failed);
                    }
                    continue;
                }
                if (result == "success")
                {
                    succeeded = Clock::now();
                }
            }
            if (auto end = Crossed(to))
            {
                return std::move(*end);
            }
            UseOnceMore(exit, succeeded, usedAgain);
        }
        return End(false, "the party did not leave in the time given; the exit was used");
    }

    // A key-locked exit's second use (#96): once its first use ended in
    // success and no load began within kUnlockGrace. Never once a load has
    // begun, which is the crossing under way: a second use ordered during
    // one was refused (an area is loading), and the refusal ended a
    // crossing that went through (turn-based-agent, 2026-10-09, #99). A
    // refusal is said in the steps, and the crossing waits on.
    void UseOnceMore(const std::string &exit, std::optional<Clock::time_point> &succeeded, bool &usedAgain)
    {
        if (answer_.value("mode", std::string()) == "loading")
        {
            usedAgain = true;
        }
        if (!succeeded || usedAgain || Clock::now() - *succeeded <= kUnlockGrace)
        {
            return;
        }
        usedAgain = true;
        succeeded.reset();
        if (!Ask("order", Json{{"action", "use"}, {"id", who_}, {"target", exit}}))
        {
            steps_.push_back(Json{{"did", "use " + exit + " again"}, {"result", *stopped_}});
            stopped_.reset();
            return;
        }
        steps_.push_back(Json{{"did", "use " + exit + " again"},
                              {"result", "asked: the first use ended, and no load began (a key's first use unlocks)"}});
    }

    // A use of the exit that failed: tried once more from a step back,
    // the cursors taken before it is ordered; nothing, or the end.
    std::optional<Json> UseAgain(const std::string &exit, const std::string &result, bool &steppedBack,
                                 std::uint64_t &since, std::uint64_t &useSince)
    {
        steps_.push_back(Json{{"did", "use " + exit}, {"result", result}});
        if (steppedBack || !StepBack(exit))
        {
            std::string why = "the use of " + exit;
            why += " ended ";
            why += result;
            return End(false, std::move(why));
        }
        steppedBack = true;
        useSince = bench_.Events().Next();
        since = useSince;
        if (!Ask("order", Json{{"action", "use"}, {"id", who_}, {"target", exit}}))
        {
            return End(false, *stopped_);
        }
        steps_.push_back(Json{{"did", "use " + exit + " again"}, {"result", "asked"}});
        return std::nullopt;
    }

    // What the game shows once the use is in: through to the other area,
    // a question, a fight, a conversation, another mode. Nothing while it
    // loads or stays.
    std::optional<Json> Crossed(const std::string &to)
    {
        const Json health = bench_.Invoke("health", Json::object()).data;
        const std::string mode = health.value("mode", std::string());
        answer_["mode"] = mode;
        if (mode == "loading")
        {
            return std::nullopt;
        }
        // Refused as the other area comes in: asked again.
        const auto read = Ask("inspect", Json{{"kind", "game"}});
        if (!read)
        {
            return std::nullopt;
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
        return std::nullopt;
    }

    // A step back from the exit, along the line from it through the walker:
    // a use from too close failed where one from a few metres back worked
    // (#29). False if the walker's place could not be read or the move failed.
    bool StepBack(const std::string &exit)
    {
        const auto read = Ask("inspect", Json{{"kind", "geometry"}, {"id", who_}, {"target", exit}});
        if (!read)
        {
            return false;
        }
        const auto from = Point(read->value("position", Json()));
        const auto to = Point(read->value("to", Json()));
        if (!from || !to)
        {
            return false;
        }
        double dx = (*from)[0] - (*to)[0];
        double dy = (*from)[1] - (*to)[1];
        double length = std::hypot(dx, dy);
        if (length < 0.01)
        {
            dx = 1;
            dy = 0;
            length = 1;
        }
        const Json point =
            Json::array({(*from)[0] + (dx / length * kStepBack), (*from)[1] + (dy / length * kStepBack), (*from)[2]});
        const auto result =
            Order(Json{{"action", "move"}, {"id", who_}, {"point", point}}, "MoveToLocation", "step back from " + exit);
        return result && *result == "success";
    }

    // A point as the game's tools write one, {x, y, z} or [x, y, z].
    static std::optional<std::array<double, 3>> Point(const Json &json)
    {
        if (json.is_array() && json.size() == 3)
        {
            return std::array<double, 3>{json[0].get<double>(), json[1].get<double>(), json[2].get<double>()};
        }
        if (json.is_object() && json.contains("x") && json.contains("y") && json.contains("z"))
        {
            return std::array<double, 3>{json["x"].get<double>(), json["y"].get<double>(), json["z"].get<double>()};
        }
        return std::nullopt;
    }

    // The point `length` metres along a walk from `from` through its
    // waypoints, start to end; nothing if the walk is no longer (#90).
    static std::optional<std::array<double, 3>> PointAlong(std::array<double, 3> from, const Json &waypoints,
                                                           double length)
    {
        for (const Json &waypoint : waypoints)
        {
            const auto next = Point(waypoint);
            if (!next)
            {
                continue;
            }
            const double leg = std::hypot((*next)[0] - from[0], (*next)[1] - from[1], (*next)[2] - from[2]);
            if (leg > 0 && leg >= length)
            {
                const double share = length / leg;
                return std::array<double, 3>{from[0] + (share * ((*next)[0] - from[0])),
                                             from[1] + (share * ((*next)[1] - from[1])),
                                             from[2] + (share * ((*next)[2] - from[2]))};
            }
            length -= leg;
            from = *next;
        }
        return std::nullopt;
    }

    // A walk of at most `upTo` metres: the game's own path cut that far
    // along, where the driver cut inspect geometry's waypoints itself to
    // test movement by steps (turn-based-agent's survey, 2026-10-06,
    // #90). The goal becomes that point; a walk no longer goes all the
    // way. True if the goal moved.
    bool CutWalk(const Json &geometry, const Json &walk)
    {
        cut_ = true;
        const auto from = Point(geometry.value("position", Json()));
        if (!from)
        {
            return false;
        }
        Json waypoints = walk.value("waypoints", Json::array());
        if (!waypoints.is_array() || waypoints.empty())
        {
            waypoints = Json::array({geometry.value("to", Json())}); // no path read: the line to it
        }
        const auto point = PointAlong(*from, waypoints, *upTo_);
        if (!point)
        {
            return false;
        }
        where_ = Json{{"point", *point}};
        answer_["cutAt"] = *point;
        return true;
    }

    Json Walk()
    {
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
            if (upTo_ && !cut_ && CutWalk(geometry, walk))
            {
                continue; // measured again to the point it was cut at
            }
            const double distance = geometry.value("distance", 0.0);
            answer_["distance"] = distance;
            measured_ = true;
            if (distance <= within_)
            {
                return End(true, "there");
            }
            if (auto end = Step(geometry, walk, distance))
            {
                return std::move(*end);
            }
        }
        return End(false, "too many steps without arriving");
    }

    // One step of the walk, by what the search answered (#77): the
    // journey's end, or nothing, to measure again and go on.
    std::optional<Json> Step(const Json &geometry, const Json &walk, double distance)
    {
        if (walk.value("found", false))
        {
            return AlongTheWalk(geometry, walk, distance);
        }
        const std::string blocker = walk.value(Json::json_pointer("/blocker/id"), std::string());
        if (!blocker.empty() && blocker == where_.value("target", std::string()))
        {
            return TargetInTheWay(walk, distance);
        }
        if (auto shut = ShutForGood(walk, blocker))
        {
            return shut;
        }
        // An exit lying ahead is no door on the way: its use leaves the
        // area or brings the map up. Where the search gives up, the
        // placeable just ahead is named, and beside Redcliffe castle's
        // village door that was the World Map exit, used twice before
        // travel stopped (turn-based-agent, 2026-10-04, #38). It is gone
        // round as nothing named: as far as the search got, then said.
        // A way out only if its use is the area transition, as the engine
        // says (GetPlaceableAction, #88): Kristoff's room door carried a
        // leadsTo its template took from the inn's street door, and its use
        // only opens it (turn-based-agent, 2026-10-09, #104). With no action
        // read, a leadsTo is taken at its word.
        const bool exitAhead = !blocker.empty() && walk.at("blocker").contains("leadsTo") &&
                               walk.at("blocker").value("action", kActionAreaTransition) == kActionAreaTransition;
        // So too a thing that is not interactive, which no use opens: the
        // East Brecilian Forest's heart has an Invisible Object across
        // the path, removed by a trigger's script as the party walks in
        // with the Grand Oak's branch; travel used it as a door and said
        // "did not open" five times without walking up to it
        // (turn-based-agent, 2026-10-04, #50). Gone round the same way,
        // a trigger near it may remove it on the way there.
        const bool inertAhead = !blocker.empty() && !walk.value(Json::json_pointer("/blocker/interactive"), true);
        if (!blocker.empty() && !exitAhead && !inertAhead)
        {
            return OpenOnTheWay(walk, blocker);
        }
        return NoWalk(walk, blocker, exitAhead, inertAhead);
    }

    // A walk found: a move along it.
    std::optional<Json> AlongTheWalk(const Json &geometry, const Json &walk, double distance)
    {
        // The walk's own end, not the target's place: a door or a
        // mirror stands where nothing can, the search ends as near
        // as a creature fits, and a move to the thing itself ended
        // COMMAND_FAILED_INVALID_PATH 6 m and 20 m short (2026-10-01).
        const double length = walk.value("length", distance);
        if (length < 0.5)
        {
            return AsNear("as near as the walk goes");
        }
        // A walk whose end the creature cannot stand on, under a
        // thing that fills its place or hangs overhead: each move
        // there ends where it began, and the walk asked again is as
        // long. The Elven Ruins' cocoon, 4 m up, took 22 such moves
        // until the steps ran out, where a use from below worked at
        // once (#4, 2026-10-02). A thing is used from as near as it
        // goes; a point is not reached.
        if (lastWalk_ && length > *lastWalk_ - kNoNearer)
        {
            if (where_.contains("target"))
            {
                return AsNear("as near as the walk goes: the last move brought it no nearer");
            }
            return End(false, "the walk goes no nearer: the last move brought it no nearer");
        }
        lastWalk_ = length;
        const Json waypoints = walk.value("waypoints", Json::array());
        const Json &to = waypoints.is_array() && !waypoints.empty() ? waypoints.back() : geometry.at("to");
        const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", to}}, "MoveToLocation", "move");
        if (!result)
        {
            return End(false, stopped_.value_or("the time ran out while walking"));
        }
        if (*result == "success" || *result == kUnseen)
        {
            return std::nullopt; // measured again above: there, or stopped short of it
        }
        if (*result == kCleared)
        {
            lastWalk_.reset(); // not a move made: it does not say the walk goes no nearer
            return std::nullopt;
        }
        if (*result != "COMMAND_FAILED_PATH_ACTION_REQUIRED")
        {
            // A move to a creature ends COMMAND_FAILED_INVALID_PATH
            // beside it, where the creature stands: there, then. A
            // walk to Master Ilen ended so 0.45 m from him (#12).
            Json again = where_;
            again.update(Json{{"kind", "geometry"}, {"id", who_}});
            if (const auto after = Ask("inspect", again))
            {
                answer_["distance"] = after->value("distance", 0.0);
                measured_ = true;
                if (after->value("distance", within_ + 1) <= within_)
                {
                    return End(true, "there");
                }
            }
            return End(false, "the walk ended " + *result);
        }
        return std::nullopt; // something shut on the way: asked again from where it stopped
    }

    std::optional<Json> TargetInTheWay(const Json &walk, double distance)
    {
        // The target itself is what stands in the way: it is reached,
        // not a door to open on the way (a chest in a narrow place was
        // opened as one, #21). Only near it: from afar the search names
        // the target as soon as the way ends at it, and a locked door
        // 62 m off answered "as near as the walk goes" with no step
        // taken (turn-based-agent, 2026-10-04, #46). It is walked up
        // to first, as far as the search got.
        if (distance > kNearOwnWay)
        {
            if (walk.contains("reached") && !walk.at("reached").is_null() && !wentToReached_)
            {
                wentToReached_ = true;
                lastWalk_.reset();
                const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", walk.at("reached")}},
                                          "MoveToLocation", "move as far as the search got");
                if (!result)
                {
                    return End(false, stopped_.value_or("the time ran out while walking"));
                }
                wentToReached_ = *result != kCleared;
                return std::nullopt;
            }
            // As far as the search got, and no nearer: its way can
            // end at the target from far off where the game walks
            // on. In the darkspawn tunnels under Ostagar an exit two
            // rooms away was "in the way" 69 m off, and from a little
            // further on the next travel walked there and through
            // (turn-based-agent, 2026-10-05, #67). The engine's own
            // move to it, once, before saying so.
            if (!engineTried_)
            {
                engineTried_ = true;
                if (EngineMove("the engine's own move, the search's way ending at the target"))
                {
                    lastWalk_.reset();
                    return std::nullopt;
                }
            }
            return End(false, "the target itself stands in the way of the walk, " +
                                  std::to_string(std::lround(distance)) +
                                  " m off, and the walk gets no nearer: order use on it walks up to it");
        }
        return AsNear("as near as the walk goes: the target itself stands in the way");
    }

    std::optional<Json> ShutForGood(const Json &walk, const std::string &blocker)
    {
        // A door the story keeps shut, or its key: its pick level is the
        // game's "cannot be picked", and using it does nothing. The
        // Circle Tower's stairs stand behind such doors until the story
        // opens them; travel used one three times and stood there
        // (turn-based-agent, 2026-10-03, #33). Said at once, neither used
        // nor walked towards.
        // So too a door whose key is required and not carried: Redcliffe
        // castle's Main Hall, its key "DoesNotExist" (turn-based-agent,
        // 2026-10-04, #40).
        // Only while it is locked: a door the party unlocked with its key,
        // which the game took, fell shut behind them unlocked, and travel
        // would not leave the room "the key the party does not carry"
        // (turn-based-agent, 2026-10-04, #45). Read as locked where no
        // state is read.
        const bool locked = walk.value(Json::json_pointer("/blocker/state"), kLocked) == kLocked;
        const bool keyMissing = locked && walk.value(Json::json_pointer("/blocker/lock/keyRequired"), false) &&
                                !walk.value(Json::json_pointer("/blocker/lock/keyHeld"), true);
        if (!blocker.empty() && keyMissing)
        {
            answer_["shutBy"] = walk.at("blocker");
            std::string why = "the way is shut by a door whose key the party does not carry (" +
                              walk.value(Json::json_pointer("/blocker/lock/keyTag"), std::string()) + "): " + blocker;
            why += " " + walk.value(Json::json_pointer("/blocker/name"), std::string());
            why += " (the story opens it, or its key; inspect kind=placeables says more)";
            return End(false, std::move(why));
        }
        // Its key held, it opens all the same: Kristoff's room door in the
        // Crown and Lion, at the cannot-pick level, was refused so with the
        // key in the pack (turn-based-agent, 2026-10-09, #104); a use with
        // the key unlocks it, and the second opens it (#66).
        const bool keyHeld = walk.value(Json::json_pointer("/blocker/lock/keyHeld"), false);
        if (!blocker.empty() && locked && !keyHeld &&
            walk.value(Json::json_pointer("/blocker/lock/pickLevel"), 0) >= kCannotPick)
        {
            answer_["shutBy"] = walk.at("blocker");
            std::string why = "the way is shut by a door that cannot be opened: " + blocker;
            why += " " + walk.value(Json::json_pointer("/blocker/name"), std::string());
            why += " (the story opens it, or its key; inspect kind=placeables says more)";
            return End(false, std::move(why));
        }
        return std::nullopt;
    }

    // A door on the way, used to open it.
    std::optional<Json> OpenOnTheWay(const Json &walk, const std::string &blocker)
    {
        if (doors_.size() >= maxDoors_)
        {
            return End(false, "more shut on the way than `maxDoors` allows; the next is " + blocker);
        }
        if (blocker == lastBlocker_)
        {
            // A door locked with its key carried: the key's first use
            // only unlocks it, the key taken from the pack, and a
            // second opens it; the Ruined Temple's south-east door
            // stood "did not open" after the first (turn-based-agent,
            // 2026-10-04, #47). Its state read straight after may
            // still say locked, a frame early: the key carried at the
            // first use is what says a second is due.
            const Json used = doors_.empty() ? Json::object() : doors_.back();
            const bool keyTurned = used.value(Json::json_pointer("/state"), kLocked) == kLocked &&
                                   used.value(Json::json_pointer("/lock/keyHeld"), false);
            if (!keyTurned || keyTurned_ == blocker)
            {
                return End(false, "what is in the way did not open: " + blocker);
            }
            keyTurned_ = blocker;
        }
        lastBlocker_ = blocker;
        lastWalk_.reset(); // the way is another once it is open
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
            lastBlocker_.clear(); // not a use made: it may be asked again
            return std::nullopt;
        }
        if (doors_.empty() || doors_.back().value("id", std::string()) != blocker)
        {
            doors_.push_back(walk.at("blocker")); // once, as it was at its first use
        }
        return std::nullopt;
    }

    std::optional<Json> NoWalk(const Json &walk, const std::string &blocker, bool exitAhead, bool inertAhead)
    {
        // No walk and nothing named in the way: go as far as the search
        // got and ask from there; a creature standing at a door gets the
        // pathfinder's own word for it. Again while each leg gets nearer:
        // a far goal's search runs out short of it (Ostagar's 314 m, the
        // Arl's dungeon's 114 m, where from 61 m the same goal was found),
        // and a driver walked on toward it by strides (turn-based-agent,
        // 2026-10-06, #82). A leg that gains nothing ends it as before.
        const double now = answer_.value("distance", 0.0);
        const bool nearer = !legFrom_ || now < *legFrom_ - kLegGain;
        // And to a new end: from where the last leg ended the search can
        // answer that same point again, a leg of nothing.
        const auto end = walk.contains("reached") ? Point(walk.at("reached")) : std::nullopt;
        const bool fresh =
            end && (!lastEnd_ || std::hypot((*end)[0] - (*lastEnd_)[0], (*end)[1] - (*lastEnd_)[1]) > kLegGain);
        if (end && (!wentToReached_ || (nearer && fresh)))
        {
            wentToReached_ = true;
            legFrom_ = now;
            lastEnd_ = end;
            answer_["legs"] = answer_.value("legs", 0) + 1;
            lastWalk_.reset();
            const auto result = Order(Json{{"action", "move"}, {"id", who_}, {"point", walk.at("reached")}},
                                      "MoveToLocation", "move as far as the search got");
            if (!result)
            {
                return End(false, stopped_.value_or("the time ran out while walking"));
            }
            wentToReached_ = *result != kCleared;
            return std::nullopt;
        }
        if (exitAhead)
        {
            answer_["exitAhead"] = walk.at("blocker");
            std::string why = blocker + " " + walk.value(Json::json_pointer("/blocker/name"), std::string()) +
                              ", an exit, stands in the way; it is not used on the way, as it leaves the area";
            if (where_.contains("target"))
            {
                return AsNear("as near as the walk goes: " + why + " (order use on the target walks the rest)");
            }
            return End(false, "no walk there: " + why);
        }
        if (inertAhead)
        {
            answer_["inertAhead"] = walk.at("blocker");
            std::string why = blocker + " " + walk.value(Json::json_pointer("/blocker/name"), std::string()) +
                              " stands in the way and is not interactive, so no use opens it: the story removes "
                              "it, a trigger near it perhaps (inspect kind=triggers)";
            if (where_.contains("target"))
            {
                return AsNear("as near as the walk goes: " + why);
            }
            return End(false, "no walk there: " + why);
        }
        // The search found none and named nothing: a strip on an open
        // doorway's line in the Lair of the Werewolves that it takes for
        // no ground, which the engine's own straight steps cross; the
        // walk stopped dead where a player walks through
        // (turn-based-agent, 2026-10-04, #53). Short straight steps
        // toward the goal, the walk asked again after each, once.
        // The engine's own move to the goal first: through an open door
        // at Soldier's Peak the search found none, where MoveToLocation
        // walked straight in and the straight steps got no nearer
        // (turn-based-agent, 2026-10-05, #67). The game's path is the
        // authority; the search only plans.
        if (walk.value("result", std::string()) == "none" && !engineTried_ && exit_.empty() && area_.empty())
        {
            engineTried_ = true;
            if (EngineMove("the engine's own move, the search having found none"))
            {
                lastWalk_.reset();
                return std::nullopt;
            }
        }
        if (walk.value("result", std::string()) == "none" && !straightTried_ && exit_.empty() && area_.empty())
        {
            straightTried_ = true;
            if (const auto why = StraightSteps())
            {
                return End(false, *why);
            }
            lastWalk_.reset();
            return std::nullopt;
        }
        return End(false, "no walk there: " + walk.value("result", std::string("the search found none")));
    }

    [[nodiscard]] milliseconds Left() const
    {
        return std::max(milliseconds(0), std::chrono::duration_cast<milliseconds>(deadline_ - Clock::now()));
    }

    // The fight near the walker in combat mode, if there is one: the nearest
    // hostile within `fightNear`, said by name, id and distance. Nothing if
    // none is that near; a fight said if the creatures cannot be read.
    std::optional<std::string> FightNear()
    {
        if (fightNear_ <= 0)
        {
            return "a fight began";
        }
        const auto read = Ask("inspect", Json{{"kind", "creatures"},
                                              {"side", "hostile"},
                                              {"near", who_},
                                              {"radius", fightNear_},
                                              {"includeDead", false}});
        if (!read)
        {
            return "a fight began";
        }
        const Json hostiles = read->value("creatures", Json::array());
        if (hostiles.empty())
        {
            return std::nullopt;
        }
        const Json &nearest = hostiles.at(0);
        std::string why =
            "a fight began: " + nearest.value("name", std::string()) + " " + nearest.value("id", std::string());
        why += std::format(" is {:.0f} m off", nearest.value("distance", 0.0));
        answer_["fightNear"] = nearest;
        return why;
    }

    // The engine's own move to the goal, in one: true if it was ordered,
    // the journey to go on.
    bool EngineMove(const std::string &step)
    {
        Json ask = where_;
        ask.update(Json{{"kind", "geometry"}, {"id", who_}});
        const auto read = Ask("inspect", ask);
        if (!read)
        {
            return false;
        }
        const auto to = Point(read->value("to", Json()));
        if (!to)
        {
            return false;
        }
        // Whatever it ends in, the walk is asked again from where it left
        // the walker, and the straight steps follow if it still finds none.
        return Order(Json{{"action", "move"}, {"id", who_}, {"point", Json::array({(*to)[0], (*to)[1], (*to)[2]})}},
                     "MoveToLocation", step)
            .has_value();
    }

    // Steps straight toward the goal, each kStraightStep, until the walk
    // finds a path again or the goal is within reach (nothing: the journey
    // goes on), or a step brings it no nearer or the steps run out (why).
    std::optional<std::string> StraightSteps()
    {
        Json ask = where_;
        ask.update(Json{{"kind", "geometry"}, {"id", who_}, {"walk", true}});
        for (int step = 0; step < kStraightSteps; ++step)
        {
            const auto read = Ask("inspect", ask);
            if (!read)
            {
                return stopped_.value_or("the walk could not be measured");
            }
            const double before = read->value("distance", 0.0);
            if (before <= within_ || (step > 0 && read->value(Json::json_pointer("/walk/found"), false)))
            {
                return std::nullopt;
            }
            const auto from = Point(read->value("position", Json()));
            const auto to = Point(read->value("to", Json()));
            if (!from || !to)
            {
                return "no walk there: none, and the way straight on could not be read";
            }
            const double dx = (*to)[0] - (*from)[0];
            const double dy = (*to)[1] - (*from)[1];
            const double length = std::hypot(dx, dy);
            const double reach = std::min(kStraightStep, length);
            const Json point = Json::array({(*from)[0] + (dx / length * reach), (*from)[1] + (dy / length * reach),
                                            (*from)[2] + (((*to)[2] - (*from)[2]) * reach / length)});
            const auto result =
                Order(Json{{"action", "move"}, {"id", who_}, {"point", point}}, "MoveToLocation", "straight step");
            if (!result)
            {
                return stopped_.value_or("the time ran out stepping straight on");
            }
            Json again = where_;
            again.update(Json{{"kind", "geometry"}, {"id", who_}});
            const auto after = Ask("inspect", again);
            if (!after || after->value("distance", before) > before - kStraightGain)
            {
                return "no walk there: none, and a straight step toward it got no nearer";
            }
        }
        return "no walk there: none, and straight steps did not find the way within " +
               std::to_string(static_cast<int>(kStraightStep * kStraightSteps)) + " m";
    }

    // Whether the walker still has a command current or queued; true if that
    // could not be read, so that a read refused is no reason to stop waiting.
    bool StillCommanded()
    {
        const auto read = Ask("inspect", Json{{"kind", "creature"}, {"id", who_}});
        if (!read)
        {
            return true;
        }
        const Json command = read->value("command", Json::object());
        return !command.value("current", Json()).is_null() || command.value("queued", 0) > 0;
    }

    // The game's loading mode, waited out: it shows it while it autosaves,
    // as while an area loads, and an autosave leaves the area as it was. A
    // walk in the Korcari Wilds stopped "an area is loading" for the game's
    // own autosave on the way (turn-based-agent, 2026-10-03, #15). Nothing
    // if the journey can go on: the mode back and the area the same.
    std::optional<std::string> WaitOutLoad()
    {
        while (Left().count() > 0)
        {
            const Json health = bench_.Invoke("health", Json::object()).data;
            if (health.value("mode", std::string()) != "loading")
            {
                break;
            }
            std::this_thread::sleep_for(std::min(Left(), milliseconds(250)));
        }
        if (Left().count() == 0)
        {
            return "an area is loading";
        }
        const auto game = Ask("inspect", Json{{"kind", "game"}});
        if (!game)
        {
            return "an area is loading";
        }
        const std::string area = game->value(Json::json_pointer("/area/tag"), std::string());
        if (!startArea_.empty() && area != startArea_)
        {
            return "another area loaded: " + area;
        }
        return std::nullopt;
    }

    // The text of the game's message box if one is up; nothing with none, or
    // with no read of the interface, when the pause is all that is known.
    std::optional<std::string> Box()
    {
        try
        {
            const Json gui = bench_.Invoke("inspect", Json{{"kind", "gui"}}).data;
            if (gui.contains("popup") && !gui.at("popup").is_null())
            {
                return gui.at("popup").value("text", std::string());
            }
            return std::nullopt;
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
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
            // The game keeps its combat mode while any hostile that has seen
            // the party lives, even 40 m off with no fight near, where a
            // player walks freely; travel stopped "a fight began" nine times
            // at one spot (turn-based-agent, 2026-10-03, #37). It walks on
            // while no hostile is within `fightNear` of the walker.
            if (auto near = FightNear())
            {
                return near;
            }
        }
        if (mode == "dialog" || mode == "cutscene")
        {
            return "a " + std::string(mode == "dialog" ? "conversation" : "cutscene") + " took the screen";
        }
        if (mode == "loading")
        {
            if (auto why = WaitOutLoad())
            {
                return why;
            }
            return NotMine(); // an autosave, done: the journey goes on, the rest asked again
        }
        if (health.value("paused", false))
        {
            // A message box pauses the game, and is what to answer: leaving
            // the Korcari Wilds, "Gather your party and venture forth?" read
            // as the game paused (turn-based-agent, 2026-10-03, #19).
            if (const auto box = Box())
            {
                return "the game asks: " + *box + "; game action=press answers it (inspect kind=gui reads its buttons)";
            }
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
        auto ordered = Ask("order", order);
        if (!ordered && stopped_ && stopped_->find("loading") != std::string::npos && !WaitOutLoad())
        {
            ordered = Ask("order", order); // refused as an autosave began: once more after it
        }
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
                if (!StillCommanded())
                {
                    // Gone with no end said: a walk to an exit 115 m off
                    // waited out its 240 s
                    // with the walker 62 m short and no command current or
                    // queued, a fight just over around her (turn-based-agent,
                    // 2026-10-03, #32). No longer waited for: measured again.
                    // Not a cleared command: a use that ends so is a use
                    // made, or a door the story keeps shut is used for ever
                    // (the Circle Tower's, three times, #33).
                    steps_.push_back(Json{{"did", std::move(what)}, {"result", kUnseen}});
                    return std::string(kUnseen);
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

    // A target reached as near as the walk goes counts as arrived near it
    // only: within `within`, or a use's reach (#80) for a thing that fills
    // its own place. 20 m from an archer where no path led, with `within`
    // 6, it read arrived (turn-based-agent, 2026-10-09, #100).
    Json AsNear(std::string reason)
    {
        const double distance = answer_.value("distance", 0.0);
        return End(distance <= std::max(within_, kUseReach), std::move(reason));
    }

    Json End(bool arrived, std::string reason)
    {
        if (!arrived && !measured_)
        {
            MeasureAgain();
        }
        answer_["arrived"] = arrived;
        answer_["reason"] = std::move(reason);
        // A move to a large object's point ends where its collision
        // begins, while a use's click walks to the object's own use point
        // (the high dragon's remains, a sarcophagus: turn-based-agent,
        // 2026-10-05, #80).
        if (!arrived && where_.contains("target") && exit_.empty() && area_.empty() &&
            answer_.value("distance", kUseReach + 1) <= kUseReach)
        {
            answer_["hint"] = "order use on the target walks up to it, as a click does";
        }
        answer_["who"] = who_;
        answer_["opened"] = doors_;
        answer_["steps"] = steps_;
        return answer_;
    }

    Bench &bench_;
    Clock::time_point deadline_;
    std::size_t maxDoors_;
    double within_;
    double fightNear_; // in combat mode, how near a hostile stops the journey
    Json where_ = Json::object();
    std::string exit_;      // `exit`: the exit's id
    std::string area_;      // `area`: the area to reach, by an exit that leads there
    Json exitAt_;           // the exit chosen, as inspect kind=exits gave it
    std::string keyTurned_; // the door used a second time, its key's first use having unlocked it
    bool swapped_ = false;  // whether the map up was swapped for the other once, looking for the pin
    std::string fromArea_;  // the area's tag before the crossing
    std::string startArea_; // the area's tag as the journey began
    Json to_;               // `to`: a world map pin's name, tag or index
    std::string who_;
    bool byClick_ = false;
    std::optional<double> legFrom_;                // the distance as the last leg to the search's end began (#82)
    std::optional<double> upTo_;                   // `upTo`: the most of the walk to make (#90)
    std::set<std::string> tried_;                  // the exits to the area whose use failed for good (#101)
    bool cityTried_ = false;                       // whether a pin was looked for by its city (#91)
    bool cut_ = false;                             // whether the walk was cut to `upTo` already
    std::optional<std::array<double, 3>> lastEnd_; // where that leg was sent
    bool wentToReached_ = false;
    std::string lastBlocker_;        // the door used last, to tell one that did not open
    std::optional<double> lastWalk_; // the walk's length when the last move along it was given
    bool straightTried_ = false;     // whether straight steps were tried where the walk found none (#53)
    bool engineTried_ = false;       // whether the engine's own move was tried where the walk found none (#67)
    bool measured_ = false;          // whether `distance` was measured after the last order
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
                 "`upTo` (metres) walks at most that far along the game's own path toward it, and answers where "
                 "the walk was cut (`cutAt`); a walk no longer goes all the way. "
                 "Answers `arrived` (within `within` metres, default 1.5, or as near as a creature can stand to a "
                 "thing that fills its own place or hangs overhead: a door, a chest, a cocoon; then use it from "
                 "there), `reason`, the `distance` left, what it "
                 "`opened`, and each step with how it ended. It stops and says why when the journey is no longer "
                 "its to make: a fight near (combat mode with a hostile within `fightNear`, default 20 m: a "
                 "fight far off that keeps the mode is walked on; the stop names the hostile), a conversation or "
                 "cutscene took the screen, the game is paused. "
                 "Opening a door may start a fight or a plot's step; the events from `cursor`, taken as it began, say "
                 "what came of the journey. "
                 "To another area: `exit` (an id from inspect kind=exits) or `area` (an area's tag: the nearest "
                 "active exit that leads there). The creature controlled walks to it, uses it, and the answer "
                 "comes once the other `area` is in place; it stops and says so if the game asks first (a "
                 "message box), opens the world map, or starts a fight or a conversation. Answering the box "
                 "(game action=press) lets the game go on by that exit to where it leads, a random "
                 "encounter's exit to its journey's first destination: travel again from where it arrives. "
                 "The game may autosave on the way. "
                 "Across the world map: `to`, a pin's name, tag or index (inspect kind=worldmap lists them), "
                 "alone with the map up, or with the `exit` that opens it; the pin is clicked and the answer "
                 "comes once its area is in place. A random encounter on the way is said (`encounter`, the "
                 "`area` it is in) and is not arrival: fight or talk it out, then travel `to` again.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"id", {{"description", "Who walks: a creature id; default the one controlled"}}},
                        {"target", {{"description", "A creature or object id to walk to"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
                        {"exit", {{"type", "string"}, {"description", "An exit's id, from inspect kind=exits"}}},
                        {"area",
                         {{"type", "string"},
                          {"description", "An area's tag, to go to by the nearest exit that leads there"}}},
                        {"to",
                         {{"description", "A world map pin: its name (any case), tag or index; alone with the map "
                                          "up, or with `exit`, the way to the map"}}},
                        {"within", {{"type", "number"}, {"description", "Metres that count as there; default 1.5"}}},
                        {"upTo",
                         {{"type", "number"},
                          {"description",
                           "With target or point: walk at most this many metres of the game's own path toward it"}}},
                        {"fightNear",
                         {{"type", "number"},
                          {"description", "In combat mode, walk on unless a hostile is this near (metres, default "
                                          "20; 0 stops for any fight)"}}},
                        {"maxDoors", {{"type", "integer"}, {"minimum", 0}, {"maximum", 20}}},
                        {"budgetMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}}}}},
                 Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "travel");
            return Journey(bench, args).Run();
        });
}

} // namespace eluvian
