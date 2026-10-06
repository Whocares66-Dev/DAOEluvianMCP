#include "core/Loot.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <optional>
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

// How long the panel may take to come after the use's command has ended:
// the click's use is cleared and given again as the container opens (seen
// in the game's events, 2026-10-02), and the panel follows a frame or more
// after.
constexpr milliseconds kPanelGrace{1500};

// How long a use may take to start: the creature stands at the container
// by then. On empty remains it never starts, and no panel comes: three of
// five spiders' remains in the Elven Ruins (turn-based-agent, 2026-10-02).
constexpr milliseconds kNoUse{3000};

// One looting: its arguments, its clock, and the answer it builds.
class Looting
{
  public:
    Looting(Bench &bench, const Args &args, milliseconds show)
        : bench_(bench), deadline_(Clock::now() + milliseconds(args.Integer("budgetMs", 60000, 1000, 600000))),
          show_(milliseconds(args.Integer("showMs", show.count(), 0, 10000))),
          target_(args.Has("target") ? args.String("target") : std::string())
    {
        answer_["cursor"] = bench_.Events().Next();
    }

    Json Run()
    {
        // A panel up already is taken from, if it is the container meant:
        // another's stands in the way of this one's opening.
        const std::string open = PanelOf(Gui());
        if (!open.empty())
        {
            if (!target_.empty() && open != target_)
            {
                throw ToolError("loot: the loot panel of " + open + " is up, not " + target_ +
                                "'s; loot with no `target` takes from it, or game action=press button=Close "
                                "closes it");
            }
            return TakeAll();
        }
        if (target_.empty())
        {
            throw ToolError("loot: no loot panel is up; give `target`, a container's id (inspect kind=placeables "
                            "lists chests, bodies' remains and the like)");
        }
        const Json walked =
            bench_.Invoke("travel", Json{{"target", target_}, {"budgetMs", std::max<long long>(1000, Left().count())}})
                .data;
        const std::string reason = walked.value("reason", std::string());
        answer_["walk"] = Json{{"arrived", walked.value("arrived", false)},
                               {"reason", reason},
                               {"distance", walked.value("distance", Json())},
                               {"opened", walked.value("opened", Json::array())}};
        // The walk may have opened the target itself, as a thing in its way:
        // a locked chest picked, crates in the path, their panel up and the
        // game paused, the walk then stopped or the use after it refused
        // (turn-based-agent, Tower of Ishal, 2026-10-03, #21). Its panel up
        // is the panel waited for.
        if (const auto gui = Ask("inspect", Json{{"kind", "gui"}}); gui && PanelOf(*gui) == target_)
        {
            return TakeAll();
        }
        if (!walked.value("arrived", false))
        {
            return End(false, "the walk there stopped: " + reason);
        }
        return Open();
    }

  private:
    // Uses the container as the player's click does, and waits for its
    // panel: what else came instead is said.
    Json Open()
    {
        const auto taken = Ask("order", Json{{"action", "use"}, {"target", target_}});
        if (!taken)
        {
            return End(false, "the use was refused: " + refused_);
        }
        const Json &given = *taken;
        const auto ordered = Clock::now();
        const std::string who = given.value(Json::json_pointer("/creature/id"), std::string());
        auto since = given.value("cursor", bench_.Events().Next());
        const auto use = EventFilter::FromArgs(Json{{"topic", {"eluvian.command.started", "eluvian.command.ended"}},
                                                    {"match", {{"id", who}, {"type", "UseObject"}}}});
        bool started = false;
        std::optional<Clock::time_point> endedAt;
        std::string result;
        while (Left().count() > 0)
        {
            if (const auto event = bench_.Events().WaitFor(since, use, std::min(Left(), milliseconds(250))))
            {
                since = event->seq + 1;
                started = true;
                if (event->topic == "eluvian.command.ended")
                {
                    result = event->payload.value("result", std::string());
                    endedAt = Clock::now();
                }
            }
            const auto read = Ask("inspect", Json{{"kind", "gui"}});
            if (!read)
            {
                return End(false, "as it opened, the game stopped answering: " + refused_ +
                                      "; wait_for eluvian.mode.changed, then inspect kind=gui says what came");
            }
            const Json &gui = *read;
            const std::string open = PanelOf(gui);
            if (open == target_)
            {
                return TakeAll();
            }
            if (!open.empty())
            {
                return End(false, "the loot panel of " + open + " opened, not " + target_ + "'s");
            }
            if (gui.contains("popup") && !gui.at("popup").is_null())
            {
                return End(false, "the game asks: " + gui.at("popup").value("text", std::string()) +
                                      "; inspect kind=gui reads it and game action=press answers it");
            }
            const std::string mode = gui.value("mode", std::string());
            if (mode == "combat")
            {
                return End(false, "a fight began");
            }
            if (mode == "dialog" || mode == "cutscene")
            {
                return End(false,
                           "a " + std::string(mode == "dialog" ? "conversation" : "cutscene") + " took the screen");
            }
            if (!started && Clock::now() - ordered > kNoUse)
            {
                return Untouched("the use started nothing");
            }
            if (endedAt && Clock::now() - *endedAt > kPanelGrace)
            {
                return Untouched("the use ended " + result);
            }
        }
        return End(false, "no loot panel came in the time given");
    }

    // A use that started nothing: what the container holds says why.
    // No panel came, the use having started nothing or ended without one:
    // what the container holds says why. Empty remains do either, and were
    // told "a locked one is unlocked first", which misled for a body
    // (turn-based-agent, 2026-10-03, #10); the lock is named only for one
    // that holds something.
    Json Untouched(const std::string &use)
    {
        const Json inventory = Ask("inspect", Json{{"kind", "inventory"}, {"id", target_}}).value_or(Json::object());
        const auto count = inventory.value("count", 0);
        answer_["empty"] = count == 0;
        if (count == 0)
        {
            return End(false, "nothing there: " + use +
                                  ", no loot panel came, and it holds no items (empty "
                                  "remains open none)");
        }
        return End(false, "it did not open: " + use + " and no loot panel came, though it holds " +
                              std::to_string(count) + (count == 1 ? " item" : " items") +
                              ". A locked one is unlocked first (order action=use objectAction=unlock); inspect "
                              "kind=placeables says its state and lock");
    }

    // Take All, as the panel's own button: the panel closes after, unless
    // the backpack is full with items left.
    Json TakeAll()
    {
        // The panel up with its items a moment, for whoever watches: in one
        // call it opened and closed within a frame or two, never seen (the
        // user, 2026-10-03, #14). The frames go on meanwhile.
        std::this_thread::sleep_for(std::min(show_, Left()));
        const auto press = Ask("game", Json{{"action", "press"}, {"button", "Take All"}});
        if (!press)
        {
            return End(false, "Take All was refused: " + refused_);
        }
        const Json &pressed = *press;
        for (const char *key : {"container", "taken", "left", "full", "backpack", "sent"})
        {
            if (pressed.contains(key))
            {
                answer_[key] = pressed.at(key);
            }
        }
        const bool closed = !pressed.contains("panel") || pressed.at("panel").is_null();
        answer_["closed"] = closed;
        if (pressed.value("full", false))
        {
            // Nothing more can be taken, and the panel up holds the game in
            // gui mode for the next verb: closed here, as its Close button
            // does (turn-based-agent, 2026-10-04, #55).
            const auto shut = Ask("game", Json{{"action", "press"}, {"button", "Close"}});
            answer_["closed"] = shut.has_value() && (!shut->contains("panel") || shut->at("panel").is_null());
            if (answer_["closed"].get<bool>())
            {
                return End(true, "the backpack is full: what is `left` stays in the container; its panel closed");
            }
            return End(true, "the backpack is full: what is `left` stays in the container, and its panel stays up; "
                             "game action=press button=Close closes it");
        }
        return End(true, closed ? "taken, and the panel closed" : "taken; the panel is still up");
    }

    Json Gui()
    {
        return bench_.Invoke("inspect", Json{{"kind", "gui"}}).data;
    }

    // A call the game refuses for its state (an area loading, the frame
    // stalled), once looting is under way: the looting ends with the
    // refusal said, not thrown, as travel's journeys do (#7). Nothing if
    // refused, and `refused_` says why.
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

    static std::string PanelOf(const Json &gui)
    {
        return gui.value(Json::json_pointer("/loot/container/id"), std::string());
    }

    [[nodiscard]] milliseconds Left() const
    {
        return std::max(milliseconds(0), std::chrono::duration_cast<milliseconds>(deadline_ - Clock::now()));
    }

    Json End(bool looted, std::string reason)
    {
        answer_["looted"] = looted;
        answer_["reason"] = std::move(reason);
        return answer_;
    }

    Bench &bench_;
    Clock::time_point deadline_;
    milliseconds show_;
    std::string target_;
    std::string refused_; // why the last call Ask made was refused
    Json answer_ = Json::object();
};

} // namespace

void AddLootTool(Bench &bench, milliseconds show)
{
    bench.Tools().Add(
        ToolSpec{"loot",
                 "Take what a container holds (a chest, a body's remains, a pile of bones: an id from inspect "
                 "kind=placeables), in one call: the creature controlled walks to `target` (as travel does, "
                 "opening doors on the way), uses it, which opens the game's loot panel, and takes all as the "
                 "panel's Take All does, which closes it. With no `target`, takes from the loot panel up now. "
                 "Answers `looted`, `reason`, the `container`, what was `taken` and what is `left` (each item "
                 "in brief), whether the backpack was `full` (then the rest stays and the panel stays up), "
                 "whether the panel `closed`, and the `walk`. It stops and says why when the container does not "
                 "open: locked, a fight, a conversation, a message box, or nothing there (`empty`: empty remains "
                 "open no panel). A container's treasure may be chosen "
                 "only as it opens, so one that read empty may not be. The events from `cursor` say what "
                 "followed (an opening may start a fight or a plot's step). `budgetMs` (default 60000) is for "
                 "the whole. `showMs` (default 1000) is how long it lingers on the panel before Take All, so "
                 "whoever watches sees it; 0 for none.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"target", {{"type", "string"}, {"description", "The container's id; none: the panel up now"}}},
                        {"budgetMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}},
                        {"showMs", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}}},
                 Thread::Listener},
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "loot");
            return Looting(bench, args, show).Run();
        });
}

} // namespace eluvian
