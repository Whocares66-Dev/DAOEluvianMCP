#include "core/Loot.h"

#include "core/Args.h"
#include "core/Placeables.h"

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

// How short of a container a walk may end and the use still be given: a
// move to a large object's point ends where its collision begins, while
// the use's click walks its user to the object's own use point. 1.9 m
// from the high dragon's remains the walk failed and the click opened
// them (turn-based-agent, 2026-10-05, #80).
constexpr double kUseReach = 5.0;

// How long a sweep waits for the world between two containers: the panel
// of the one before takes a moment to go (the driver's lootall waited
// up to 4 s, 2026-10-06).
constexpr milliseconds kSettle{4000};

// One looting: its arguments, its clock, and the answer it builds.
class Looting
{
  public:
    Looting(Bench &bench, const Args &args, milliseconds show, bool autoloot = false)
        : bench_(bench),
          deadline_(Clock::now() +
                    milliseconds(args.Integer("budgetMs", args.Has("radius") ? 300000 : 60000, 1000, 600000))),
          show_(milliseconds(args.Integer("showMs", show.count(), 0, 10000))),
          target_(args.Has("target") ? args.String("target") : std::string()),
          who_(args.Has("id") ? args.String("id") : std::string()),
          radius_(args.Has("radius") ? std::optional<double>(args.Number("radius")) : std::nullopt), autoloot_(autoloot)
    {
        answer_["cursor"] = bench_.Events().Next();
    }

    // Another party member loots with control handed to it for the
    // walk and the use, and given back as the looting ends, however it
    // ends: the driver did both by hand (turn-based-agent's survey,
    // 2026-10-06, #86).
    Json Run()
    {
        if (radius_ && !target_.empty())
        {
            throw ToolError("loot: `radius` loots everything around, `target` one container: give one");
        }
        if (who_.empty())
        {
            return Act();
        }
        const Json control = bench_.Invoke("order", Json{{"action", "control"}, {"id", who_}}).data;
        if (!control.value("taken", false))
        {
            throw ToolError("loot: " + who_ + " would not take control; nothing was looted");
        }
        before_ = control.value(Json::json_pointer("/before/id"), std::string());
        Json answer;
        try
        {
            answer = Act();
        }
        catch (...)
        {
            GiveBack();
            throw;
        }
        if (const auto back = GiveBack())
        {
            answer["controlled"] = *back;
        }
        return answer;
    }

    // Control given back to whom had it, if another: whom the game says
    // is controlled after, or nothing if it was not asked or refused.
    std::optional<Json> GiveBack()
    {
        if (before_.empty() || before_ == who_)
        {
            return std::nullopt;
        }
        const auto back = Ask("order", Json{{"action", "control"}, {"id", before_}});
        if (!back)
        {
            return Json{{"backTo", before_}, {"refused", refused_}};
        }
        return back->value("controlled", Json());
    }

    Json Act()
    {
        return radius_ ? Sweep() : Loot();
    }

    // Every placeable within the radius whose use is a loot, nearest
    // first, each looted as one loot does; one whose use is an unlock is
    // tried too, the looter perhaps able to pick it, as the driver's
    // lootall did. The engine's own action says which (GetPlaceableAction,
    // lib-agent, 2026-10-06), where the driver chose by words in tags
    // (#88). The sweep stops where the game turns from the world (a fight,
    // a conversation, a question) or the pack is full; a locked or empty
    // one is said and passed.
    Json Sweep()
    {
        Json near = Json{{"kind", "placeables"}, {"radius", *radius_}};
        if (!who_.empty())
        {
            near["near"] = who_;
        }
        const Json listed = bench_.Invoke("inspect", near).data;
        Json containers = Json::array();
        Json skipped = Json::array();
        int looted = 0;
        std::optional<std::string> stopped;
        for (const Json &placeable : listed.value("placeables", Json::array()))
        {
            const int action = placeable.value("action", 0);
            if (!placeable.value("interactive", false) ||
                (action != kPlaceableActionLoot && action != kPlaceableActionUnlock))
            {
                skipped.push_back(Json{{"id", placeable.value("id", std::string())},
                                       {"name", placeable.value("name", std::string())},
                                       {"action", placeable.value("actionName", std::string())}});
                continue;
            }
            if (!stopped)
            {
                stopped = Unsettled();
            }
            if (stopped)
            {
                continue;
            }
            Json report = LootOne(placeable);
            looted += report.value("looted", false) ? 1 : 0;
            if (report.value("full", false))
            {
                stopped = "the backpack is full";
            }
            containers.push_back(std::move(report));
        }
        const auto tried = containers.size();
        answer_["radius"] = *radius_;
        answer_["containers"] = std::move(containers);
        answer_["skipped"] = std::move(skipped);
        answer_["stopped"] = stopped ? Json(*stopped) : Json(nullptr);
        answer_["count"] = looted;
        return End(looted > 0, (stopped ? "stopped: " + *stopped + "; " : std::string()) + std::to_string(looted) +
                                   " of " + std::to_string(tried) + " containers looted");
    }

    // One container of a sweep, looted as loot with its `target` does.
    Json LootOne(const Json &placeable)
    {
        const std::string id = placeable.value("id", std::string());
        // The scripts' Loot action moves all of a container whose tag has
        // _autoloot (herbs, flowers) to the looter, with no panel
        // (placeable_h.nss, PLACEABLE_ACTION_OPEN_INVENTORY; lib-agent).
        const bool autoloot = placeable.value("tag", std::string()).find("_autoloot") != std::string::npos;
        Json report;
        try
        {
            const Args one(Json{{"target", id},
                                {"budgetMs", std::max<long long>(1000, Left().count())},
                                {"showMs", show_.count()}},
                           "loot");
            report = Looting(bench_, one, show_, autoloot).Run();
        }
        catch (const ToolError &e)
        {
            report = Json{{"looted", false}, {"reason", e.what()}};
        }
        report.erase("cursor");
        report["target"] = Json{{"id", id}, {"name", placeable.value("name", std::string())}};
        return report;
    }

    // Why the sweep cannot go on, if it cannot: the world not back within
    // kSettle (a fight, a conversation, a question, a panel that stays),
    // or the time gone.
    std::optional<std::string> Unsettled()
    {
        const auto until = Clock::now() + kSettle;
        std::string why = "the time ran out";
        while (Left().count() > 0)
        {
            const auto read = Ask("inspect", Json{{"kind", "gui"}});
            if (!read)
            {
                return "the game stopped answering: " + refused_;
            }
            if (auto instead = Instead(*read, std::string()))
            {
                return instead;
            }
            const std::string mode = read->value("mode", std::string());
            if (mode == "explore")
            {
                return std::nullopt;
            }
            why = "the game is in mode " + mode + ", not the world";
            if (Clock::now() > until)
            {
                return why;
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        return why;
    }

    Json Loot()
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
            // Short of it, near enough, and nothing else afoot: the use
            // walks the rest. A fight or a conversation that stopped the
            // walk is said, as before.
            const double distance = walked.value("distance", kUseReach + 1);
            const std::string mode = walked.value("mode", std::string("explore"));
            if (distance > kUseReach || mode != "explore")
            {
                return End(false, "the walk there stopped: " + reason);
            }
            answer_["walk"]["note"] = "the walk ended short; the use's click walks the rest";
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
            if (auto instead = Instead(gui, open))
            {
                return End(false, std::move(*instead));
            }
            if (!started && Clock::now() - ordered > kNoUse)
            {
                return Untouched("the use started nothing");
            }
            if (endedAt && Clock::now() - *endedAt > kPanelGrace)
            {
                if (autoloot_ && result == "success")
                {
                    answer_["picked"] = true;
                    return End(true, "picked: the game gives what an _autoloot container holds to the "
                                     "looter, with no panel");
                }
                return Untouched("the use ended " + result);
            }
        }
        return End(false, "no loot panel came in the time given");
    }

    // What came instead of the container's panel, if anything: another's
    // panel, a question, a fight, a conversation or a cutscene.
    std::optional<std::string> Instead(const Json &gui, const std::string &open) const
    {
        if (!open.empty())
        {
            return "the loot panel of " + open + " opened, not " + target_ + "'s";
        }
        if (gui.contains("popup") && !gui.at("popup").is_null())
        {
            return "the game asks: " + gui.at("popup").value("text", std::string()) +
                   "; inspect kind=gui reads it and game action=press answers it";
        }
        const std::string mode = gui.value("mode", std::string());
        if (mode == "combat")
        {
            return "a fight began";
        }
        if (mode == "dialog" || mode == "cutscene")
        {
            return "a " + std::string(mode == "dialog" ? "conversation" : "cutscene") + " took the screen";
        }
        return std::nullopt;
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
    std::string who_;              // the party member looting, if not the one controlled
    std::string before_;           // whom control is given back to
    std::optional<double> radius_; // `radius`: everything around, not one target
    bool autoloot_;                // a container the game empties into the looter with no panel
    std::string refused_;          // why the last call Ask made was refused
    Json answer_ = Json::object();
};

} // namespace

void AddLootTool(Bench &bench, milliseconds show)
{
    bench.Tools().Add(
        Budgeted(
            ToolSpec{
                "loot",
                "Take what a container holds (a chest, a body's remains, a pile of bones: an id from inspect "
                "kind=placeables), in one call: the creature controlled, or `id`, another party member, "
                "given control for the looting and control given back after (`controlled` says to whom), "
                "walks to `target` (as travel does, "
                "opening doors on the way), uses it, which opens the game's loot panel, and takes all as the "
                "panel's Take All does, which closes it. With no `target`, takes from the loot panel up now. "
                "Answers `looted`, `reason`, the `container`, what was `taken` and what is `left` (each item "
                "in brief), whether the backpack was `full` (then the rest stays and the panel stays up), "
                "whether the panel `closed`, and the `walk`. It stops and says why when the container does not "
                "open: locked, a fight, a conversation, a message box, or nothing there (`empty`: empty remains "
                "open no panel). With `radius` (metres) instead of `target`, every placeable within it whose use "
                "is a loot or an unlock (inspect kind=placeables' `action`), nearest first: one report "
                "each in `containers`, `count` looted, what else was there in `skipped`; it stops "
                "(`stopped`) at a fight, a conversation, a question or a full pack, and passes a "
                "locked or empty one. A container's treasure may be chosen "
                "only as it opens, so one that read empty may not be. The events from `cursor` say what "
                "followed (an opening may start a fight or a plot's step). `budgetMs` (default 60000) is for "
                "the whole. `showMs` (default 1000) is how long it lingers on the panel before Take All, so "
                "whoever watches sees it; 0 for none.",
                Json{{"type", "object"},
                     {"properties",
                      {{"target", {{"type", "string"}, {"description", "The container's id; none: the panel up now"}}},
                       {"id",
                        {{"type", "string"},
                         {"description", "The party member who loots (default: the one controlled)"}}},
                       {"radius",
                        {{"type", "number"},
                         {"minimum", 0},
                         {"description", "Every container within this many metres"}}},
                       {"budgetMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 600000}}},
                       {"showMs", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}}},
                Thread::Listener},
            "budgetMs", 300000),
        [&bench, show](const Json &raw) -> ToolResult {
            const Args args(raw, "loot");
            return Looting(bench, args, show).Run();
        });
}

} // namespace eluvian
