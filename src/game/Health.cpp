#include "game/Health.h"

#include "dao/ClientExoApp.h"
#include "dao/GameModes.h"

#include <Windows.h>

namespace eluvian::game
{

std::string ModeName(std::int32_t mode)
{
    switch (static_cast<dao::GameMode>(mode))
    {
    case dao::GameMode::Explore:
        return "explore";
    case dao::GameMode::Combat:
        return "combat";
    case dao::GameMode::Dead:
        return "dead";
    case dao::GameMode::Gui:
        return "gui";
    case dao::GameMode::Dialog:
        return "dialog";
    case dao::GameMode::Cutscene:
        return "cutscene";
    case dao::GameMode::FlyCam:
        return "flycam";
    case dao::GameMode::Fixed:
        return "fixed";
    case dao::GameMode::Pregame:
        return "pregame";
    case dao::GameMode::Loading:
        return "loading";
    case dao::GameMode::Movie:
        return "movie";
    case dao::GameMode::CharGen:
        return "chargen"; // the main menu runs here too
    case dao::GameMode::PartyPicker:
        return "partypicker";
    }
    return mode < 0 ? "none" : "unknown";
}

Watch::Watch(Bench &bench) : bench_(bench)
{
}

void Watch::OnFrame()
{
    ++frames_;
    lastFrameMs_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();

    const auto mode = dao::CClientExoApp::CurrentMode();
    const std::int32_t now = mode ? static_cast<std::int32_t>(*mode) : -1;
    if (const auto before = mode_.exchange(now); before != now)
    {
        bench_.Events().Emit("eluvian.mode.changed", Json{{"from", ModeName(before)}, {"to", ModeName(now)}});
    }
    const auto *app = dao::CClientExoApp::Get();
    const std::int32_t paused = app == nullptr ? -1 : (app->IsPaused() ? 1 : 0);
    if (const auto before = paused_.exchange(paused); before != paused && paused >= 0)
    {
        bench_.Events().Emit("eluvian.paused", Json{{"paused", paused == 1}});
    }
}

void AddHealthTool(Bench &bench, const Watch &watch, std::function<Json()> extra)
{
    bench.Tools().Add(
        ToolSpec{"health",
                 "Is the game alive, and in what state: answered without waiting for the game's frame, so it works "
                 "while an area loads or the game hangs. `state` is running (frames coming), stalled (none for a "
                 "second: loading, a modal dialog, a hang) or starting (none yet). Also the game mode, the pause, "
                 "calls waiting for a frame, the event cursor (for wait_for's `since`), and which plugins have "
                 "registered tools. Call it first.",
                 Json{{"type", "object"}}, Thread::Listener, true},
        [&bench, &watch, extra = std::move(extra)](const Json &) -> ToolResult {
            const auto nowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watch.start_)
                    .count();
            const auto last = watch.lastFrameMs_.load();
            const auto sinceFrame = last < 0 ? -1 : nowMs - last;
            const char *state = last < 0 ? "starting" : (sinceFrame > 1000 ? "stalled" : "running");
            const auto paused = watch.paused_.load();
            Json answer = {
                {"state", state},
                {"mode", ModeName(watch.mode_.load())},
                {"paused", paused < 0 ? Json(nullptr) : Json(paused == 1)},
                {"frames", watch.frames_.load()},
                {"msSinceFrame", sinceFrame},
                {"callsWaiting", bench.Frames().Pending()},
                {"cursor", bench.Events().Next()},
                {"pid", GetCurrentProcessId()},
                {"toolsRevision", bench.Tools().Revision()},
            };
            if (extra)
            {
                answer.update(extra());
            }
            return answer;
        });
}

} // namespace eluvian::game
