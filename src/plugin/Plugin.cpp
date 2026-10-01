// eluvian.dll: Eluvian MCP, a test bench serving the running game to agents
// (MCP, POST /mcp) and scripts (REST, /api/) on 127.0.0.1. A DAOLib plugin.
//
// At load it makes the bench (core/Bench.h), adds its tools, starts the
// listener (server/HttpServer.h) and adds a frame callback. The callback
// runs on the scripts' thread, paused or not; it notes the frame
// (game/Health.h) and runs the calls the listener queued for the game
// (core/FrameQueue.h), so every read and order happens where the game
// allows it. Other plugins add their tools through src/api/EluvianAPI.h.
//
// dev/DESIGN.md is the design; README.md how to connect.
#include "core/Bench.h"
#include "core/Http.h"
#include "core/Mcp.h"
#include "core/Rest.h"
#include "core/Scenario.h"
#include "game/GameTools.h"
#include "game/Health.h"
#include "plugin/HostApi.h"
#include "server/HttpServer.h"

#include "dao/Frame.h"
#include "dao/Log.h"
#include "dao/Plugin.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

using namespace eluvian;

// 8930: beside devbench's 8920 and 8921 for Skyrim; the Ghidra MCP has 8090.
constexpr std::uint16_t kDefaultPort = 8930;
constexpr int kPortTries = 10;

constexpr std::string_view kInstructions =
    "Eluvian serves a running Dragon Age: Origins. Start with `health` (is the game up, in which mode, and "
    "`owner`: who started it; a game that is not yours is another's test or the player's own, to read but not to "
    "drive), then "
    "`inspect kind=game`. Creatures are named by id strings like \"0x7d9\", as the logs write them; `inspect "
    "kind=creatures` lists them with positions, sides and health, `inspect kind=creature` gives one with its "
    "effects. Act, then wait: `order` answers with a `cursor`, and `wait_for` from that cursor returns the event "
    "you expect (eluvian.command.ended, eluvian.creature.died, or a mod's own) instead of a guessed sleep. "
    "Mods that register with Eluvian add tools, inspect kinds and events of their own; `health` lists them. "
    "`log` reads a log from a cursor (`since: \"end\"` marks the end, to read from after acting). `script` calls any "
    "script command by name when no tool fits. A conversation or cutscene on the screen (health's mode dialog or "
    "cutscene) is read with `inspect kind=dialogue` and answered with `dialogue`. Calls run on the game's frame: while "
    "an area loads they are "
    "refused, and if frames stop they time out; `health` says which.";

// Who started this game, as the launcher said in ELUVIAN_OWNER: a test's
// agent, by the name it goes by. Several agents share one game and one
// port, and each had to ask the others whose a running game was; one
// acted on another's by mistake (2026-10-01). Empty when the game was
// started by hand: the player's own, and not a test's to drive or close.
std::string OwnerSetting()
{
    char *text = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&text, &size, "ELUVIAN_OWNER") != 0 || text == nullptr)
    {
        return {};
    }
    std::string owner(text);
    std::free(text);
    std::erase_if(owner, [](unsigned char c) { return c < 0x20 || c > 0x7E; });
    owner.resize(std::min<std::size_t>(owner.size(), 64));
    return owner;
}

// A whole number from the environment, within [low, high]; `fallback` when
// the variable is not set, or is not such a number (said in the log).
unsigned Setting(const char *name, unsigned fallback, unsigned low, unsigned high)
{
    char *text = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&text, &size, name) != 0 || text == nullptr)
    {
        return fallback;
    }
    const std::string_view value(text);
    unsigned number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    const bool good = error == std::errc{} && end == value.data() + value.size() && number >= low && number <= high;
    std::free(text);
    if (!good)
    {
        spdlog::warn("{} is not a number from {} to {}; using {}", name, low, high, fallback);
        return fallback;
    }
    return number;
}

std::uint16_t PortSetting()
{
    return static_cast<std::uint16_t>(Setting("ELUVIAN_PORT", kDefaultPort, 1, 65535));
}

// How many events the bus keeps. A session of eight turn-based fights made
// 12,700 in 66 minutes (2026-09-30), which the first 16,384 would have begun
// to drop before an hour and a half; 32,768 is a few megabytes of the
// game's 32-bit space. ELUVIAN_EVENTS chooses another.
std::size_t EventsSetting()
{
    return Setting("ELUVIAN_EVENTS", 32768, 1024, 262144);
}

// What the bench was asked to do, in eluvian.log: each call with its
// arguments, how it ended and how long it took, so a session can be read
// afterwards (the events say what happened in the game, not what an agent
// did to it). The reads an agent repeats all session (health, events,
// wait_for, log) are at debug, the rest at info.
void LogCall(const CallRecord &call)
{
    std::string args = call.args.dump();
    if (args.size() > 300)
    {
        args.resize(300);
        args += "...";
    }
    const bool routine =
        call.tool == "health" || call.tool == "events" || call.tool == "wait_for" || call.tool == "log";
    const auto level = routine ? spdlog::level::debug : spdlog::level::info;
    if (call.error.empty())
    {
        spdlog::log(level, "call {} {}: answered in {:.0f} ms", call.tool, args, call.ms);
    }
    else
    {
        spdlog::log(level, "call {} {}: refused after {:.0f} ms: {}", call.tool, args, call.ms, call.error);
    }
}

struct State
{
    // Events are stamped with the frame from the watch's count, which is
    // atomic; DAOLib's own is read only on the frame thread.
    Bench bench{
        BenchOptions{std::chrono::milliseconds(5000), EventsSetting(), [this] { return watch.Frames(); }, &LogCall}};
    game::Watch watch{bench};
    McpEndpoint mcp{bench, ServerInfo{"eluvian", EL_VERSION_STRING, std::string(kInstructions)}};
    RestEndpoint rest{bench};
    HttpServer server{[this](const HttpRequest &request) { return Route(request, mcp, rest); }};
    std::uint16_t port = 0;
};

// Made at load and never freed: the listener's threads may still be running
// when the process ends, and the OS takes everything then.
State *g = nullptr;

// Where scripts and the bridge find the port: eluvian.json beside the logs.
// A game that found the first port taken is a second game, and writes
// eluvian.<pid>.json instead, as DAOLib names a second game's logs: on
// 2026-09-30 a second game wrote its port over the first's, and the bridge
// then looked for the game that was gone (turn-based-agent).
void WriteRuntime(std::uint16_t port)
{
    const auto name =
        port == PortSetting() ? std::string("eluvian.json") : std::format("eluvian.{}.json", GetCurrentProcessId());
    const auto file = dao::LogDirectory() / name;
    std::ofstream out(file, std::ios::trunc);
    out << Json{{"pid", GetCurrentProcessId()},
                {"port", port},
                {"mcp", std::format("http://127.0.0.1:{}/mcp", port)},
                {"rest", std::format("http://127.0.0.1:{}/api/", port)},
                {"version", EL_VERSION_STRING}}
               .dump(2);
}

} // namespace

extern "C" __declspec(dllexport) bool DAOLib_Load(const dao::LoaderInfo *info)
{
    dao::OpenLog("eluvian");
    spdlog::info("eluvian {}", EL_VERSION_STRING);
    // The loader's contract first, before anything of LoaderInfo's is read:
    // a loader of another API, or one whose struct ends before the fields
    // used here, is not this plugin's (as the other plugins check).
    if (info == nullptr || info->apiVersion != dao::kPluginApiVersion ||
        info->structSize < offsetof(dao::LoaderInfo, expectedExecutable) + sizeof(info->expectedExecutable))
    {
        spdlog::error("the loader's plugin API is {}, not the {} Eluvian was built for; Eluvian stays off",
                      info != nullptr ? info->apiVersion : 0, dao::kPluginApiVersion);
        return false;
    }
    if (!info->expectedExecutable)
    {
        spdlog::error("not the DAOrigins.exe DAOLib was read from; Eluvian stays off");
        return false;
    }
    g = new State();
    AddEventTools(g->bench);
    AddLogTool(g->bench, dao::LogDirectory());
    AddScenarioTool(g->bench);
    game::AddHealthTool(g->bench, g->watch, [] {
        static const std::string owner = OwnerSetting();
        return Json{{"port", g->port},
                    {"version", EL_VERSION_STRING},
                    {"owner", owner.empty() ? Json(nullptr) : Json(owner)},
                    {"plugins", plugin::RegisteredByPlugins()}};
    });
    game::AddGameTools(g->bench, g->watch);
    game::AddEngineEvents(g->bench);
    plugin::BindHostApi(g->bench);

    dao::OnFrame([] {
        g->watch.OnFrame();
        g->bench.Frames().Drain();
    });

    g->port = g->server.Start(PortSetting(), kPortTries);
    if (g->port == 0)
    {
        spdlog::error("no free port from {} to {} on 127.0.0.1: the bench is not served", PortSetting(),
                      PortSetting() + kPortTries - 1);
        return true; // the game plays on; the plugins that register still can
    }
    WriteRuntime(g->port);
    spdlog::info("serving MCP at http://127.0.0.1:{}/mcp and REST at http://127.0.0.1:{}/api/", g->port, g->port);
    return true;
}
