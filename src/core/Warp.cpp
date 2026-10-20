#include "core/Warp.h"

#include "core/Args.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr std::size_t kMostNear = 10;
// Farther than this from the waypoint asked for, the hero is said not to be at it.
constexpr double kAtWaypoint = 5.0;

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The areas whose name has the one asked in it, or, with none, those that
// begin as it does up to its first '_' (pre440ar_ for the Tower of Ishal's
// floors): a name half remembered finds its neighbours.
std::vector<std::string> NearAreas(const std::string &asked, const Json &areas)
{
    std::vector<std::string> names;
    for (const auto &area : areas)
    {
        if (area.is_string())
        {
            names.push_back(Lower(area.get<std::string>()));
        }
    }
    std::ranges::sort(names);
    std::vector<std::string> near;
    for (const auto &name : names)
    {
        if (near.size() < kMostNear && name.find(asked) != std::string::npos)
        {
            near.push_back(name);
        }
    }
    const auto cut = asked.find('_');
    if (near.empty() && cut != std::string::npos && cut > 0)
    {
        const std::string prefix = asked.substr(0, cut + 1);
        for (const auto &name : names)
        {
            if (near.size() < kMostNear && name.starts_with(prefix))
            {
                near.push_back(name);
            }
        }
    }
    return near;
}

std::string Joined(const std::vector<std::string> &names)
{
    std::string text;
    for (const auto &name : names)
    {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

// The waypoint of a tag, without case; null for none.
const Json *WaypointOf(const Json &waypoints, const std::string &tag)
{
    for (const auto &waypoint : waypoints)
    {
        if (Lower(waypoint.value("tag", std::string())) == Lower(tag))
        {
            return &waypoint;
        }
    }
    return nullptr;
}

std::vector<std::string> TagsOf(const Json &waypoints)
{
    std::vector<std::string> tags;
    for (const auto &waypoint : waypoints)
    {
        const std::string tag = waypoint.value("tag", std::string());
        if (std::ranges::find(tags, tag) == tags.end())
        {
            tags.push_back(tag);
        }
    }
    return tags;
}

// A read the game may refuse while an area loads: nothing then, asked again.
std::optional<Json> Read(Bench &bench, const char *tool, const Json &args = Json::object())
{
    try
    {
        return bench.Invoke(tool, args).data;
    }
    catch (const ToolError &)
    {
        return std::nullopt;
    }
    catch (const FrameTimeout &)
    {
        return std::nullopt;
    }
}

double Distance(const Json &a, const Json &b)
{
    const double dx = a.value("x", 0.0) - b.value("x", 0.0);
    const double dy = a.value("y", 0.0) - b.value("y", 0.0);
    const double dz = a.value("z", 0.0) - b.value("z", 0.0);
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

class Warp
{
  public:
    Warp(Bench &bench, const Json &raw, milliseconds poll, milliseconds settle)
        : bench_(bench), args_(raw, "warp"), poll_(poll), settle_(settle), started_(Clock::now())
    {
    }

    Json Run()
    {
        const std::string asked = Lower(args_.String("area"));
        const auto budget = milliseconds(args_.Integer("budgetMs", 60000, 5000, 180000));
        const Json file = bench_.Invoke("warp.area", Json{{"area", asked}}).data;
        if (!file.value("found", false))
        {
            const auto near = NearAreas(asked, file.value("areas", Json::array()));
            throw ToolError(std::format("warp: no area \"{}\" in the game's archives{}. An area is named by its "
                                        "tag, as state's `area` gives it, and `resources` with type \"are\" lists "
                                        "them; nothing was sent",
                                        asked, near.empty() ? "" : "; near it: " + Joined(near)));
        }
        const std::string tag = file.value("tag", asked);
        const Json waypoints = file.value("waypoints", Json::array());
        const Json module = file.value("module", Json());
        const bool readable = file.value("readable", true);
        if (!readable && (args_.Bool("waypoints", false) || args_.Has("waypoint")))
        {
            throw ToolError(std::format("warp: {}'s file is in {}'s archive, which is enciphered, so its waypoints "
                                        "are not known; leave `waypoint` out for the area's own arrival; nothing "
                                        "was sent",
                                        tag, module.is_string() ? module.get<std::string>() : "an add-in"));
        }
        if (args_.Bool("waypoints", false))
        {
            return Json{{"area", tag}, {"module", module}, {"count", waypoints.size()}, {"waypoints", waypoints}};
        }
        const Json *waypoint = nullptr;
        if (args_.Has("waypoint"))
        {
            waypoint = WaypointOf(waypoints, args_.String("waypoint"));
            if (waypoint == nullptr)
            {
                throw ToolError(std::format("warp: {} has no waypoint \"{}\", and the game would put the party at "
                                            "(0, 0, 0); its waypoints: {}. Leave `waypoint` out for the area's own "
                                            "arrival; nothing was sent",
                                            tag, args_.String("waypoint"), Joined(TagsOf(waypoints))));
            }
        }
        const Json health = bench_.Invoke("health", Json::object()).data;
        const std::string mode = health.value("mode", std::string());
        if (mode != "explore" && mode != "combat")
        {
            throw ToolError(std::format("warp: the game is in {} mode; warp jumps from explore or combat, with a "
                                        "game loaded (health says the mode); nothing was sent",
                                        mode.empty() ? "no" : mode));
        }
        Json answer = Json::object();
        answer["module"] = module;
        answer["cursor"] = health.value("cursor", Json());
        const std::string to = waypoint != nullptr ? waypoint->value("tag", std::string()) : std::string();
        const Json sent =
            bench_
                .Invoke("script",
                        Json{{"action", "call"}, {"command", "DoAreaTransition"}, {"args", Json::array({tag, to})}})
                .data;
        if (sent.value("value", 1) == 0)
        {
            // An add-in's area, from the campaign: Stone Prisoner's hut from
            // the Dalish origin answered 0, and the game stayed (2026-10-19).
            throw ToolError(std::format("warp: the game refused the transition to {} (DoAreaTransition answered 0), "
                                        "and the party is where it was: the area is {}'s, and the module running "
                                        "does not load it",
                                        tag, module.is_string() ? module.get<std::string>() : "another module"));
        }
        const auto arrived = WaitArrival(tag, budget);
        answer["arrived"] = arrived.has_value();
        answer["waitedMs"] = std::chrono::duration_cast<milliseconds>(Clock::now() - started_).count();
        if (!arrived)
        {
            answer["area"] = tag;
            answer["note"] = std::format("the game had not put the party in {} within budgetMs; health and state "
                                         "say where it is now",
                                         tag);
            return answer;
        }
        answer["area"] = arrived->value("area", Json());
        answer["party"] = arrived->value("party", Json::array());
        std::this_thread::sleep_for(settle_);
        const std::string after = Read(bench_, "health").value_or(Json::object()).value("mode", std::string());
        answer["mode"] = after;
        if (waypoint != nullptr)
        {
            answer["waypoint"] = *waypoint;
            Where(answer, *waypoint);
        }
        if (after == "cutscene")
        {
            answer["note"] = "the area's scripts started a cutscene: dialogue's skip_cutscene skips it";
        }
        else if (after == "dialog")
        {
            answer["note"] = "the area's scripts started a conversation: dialogue answers it";
        }
        else if (after == "combat")
        {
            answer["note"] = "the area's scripts started a fight";
        }
        return answer;
    }

  private:
    // The state once the party is in the area asked for, out of the load;
    // nothing if the budget ran out first.
    std::optional<Json> WaitArrival(const std::string &tag, milliseconds budget)
    {
        while (Clock::now() - started_ < budget)
        {
            const auto health = Read(bench_, "health");
            if (health && health->value("mode", std::string()) != "loading")
            {
                const auto state = Read(bench_, "state");
                if (state && Lower(state->value("area", Json::object()).value("tag", std::string())) == Lower(tag))
                {
                    return state;
                }
            }
            std::this_thread::sleep_for(poll_);
        }
        return std::nullopt;
    }

    // Where the hero stands, and how far that is from the waypoint asked for.
    void Where(Json &answer, const Json &waypoint)
    {
        const Json party = answer.value("party", Json::array());
        if (party.empty() || !party.front().contains("id"))
        {
            return;
        }
        const auto hero = Read(bench_, "creature", Json{{"id", party.front()["id"]}});
        if (!hero || !hero->contains("position"))
        {
            return;
        }
        const double off = Distance(hero->at("position"), waypoint.value("position", Json::object()));
        answer["at"] = hero->at("position");
        answer["fromWaypoint"] = std::round(off * 100.0) / 100.0;
        if (off > kAtWaypoint)
        {
            answer["note"] = std::format("the hero stands {:.1f} m from the waypoint: the area's scripts may have "
                                         "moved the party on arriving",
                                         off);
        }
    }

    Bench &bench_;
    Args args_;
    milliseconds poll_;
    milliseconds settle_;
    Clock::time_point started_;
};

} // namespace

void AddWarpTool(Bench &bench, milliseconds poll, milliseconds settle)
{
    bench.Tools().Add(
        Budgeted(
            ToolSpec{
                "warp",
                "Puts the party in an area, at one of its waypoints, by the game's own area transition "
                "(DoAreaTransition), with no save there: the area loads and its scripts run as on arriving "
                "by its doors, so a cutscene or a fight it starts plays. Use a test save: the area's plot may "
                "not be where its scripts expect, and plot sets flags first.\n\n"
                "Pass `area`, its tag (state's `area` gives the one the party is in; `resources` with type "
                "\"are\" lists them). `waypoint`, one of the area's, is where the party arrives; without it, "
                "the area's own arrival. `waypoints: true` lists the area's waypoints (tag and position) and "
                "jumps nowhere. Both are checked against the area's file before anything is sent: the game "
                "itself puts the party at (0, 0, 0) for a waypoint the area lacks.\n\n"
                "It jumps from explore or combat mode, and answers once the party is in the area, or after "
                "`budgetMs`: the `area`, the `party`, the `mode` it arrived in (cutscene, dialog or combat if "
                "the area's scripts started one), with a waypoint where the hero stands (`at`) and how far "
                "from it (`fromWaypoint`, metres), `arrived`, and a `cursor` for the events of the load.",
                Json{{"type", "object"},
                     {"properties",
                      {{"area",
                        {{"type", "string"},
                         {"description", "The area's tag, as pre440ar_tower_level_4 (the Tower of Ishal's top "
                                         "floor). Without case."}}},
                       {"waypoint",
                        {{"type", "string"},
                         {"description", "One of the area's waypoints, by tag. Default: the area's own arrival."}}},
                       {"waypoints",
                        {{"type", "boolean"},
                         {"description", "List the area's waypoints and jump nowhere. Default false."}}},
                       {"budgetMs",
                        {{"type", "integer"},
                         {"minimum", 5000},
                         {"maximum", 180000},
                         {"description", "The longest to wait for the area to load, in milliseconds. Default "
                                         "60000."}}}}},
                     {"required", {"area"}}},
                Thread::Listener},
            "budgetMs", 60000),
        [&bench, poll, settle](const Json &raw) -> ToolResult { return Warp(bench, raw, poll, settle).Run(); });
}

} // namespace eluvian
