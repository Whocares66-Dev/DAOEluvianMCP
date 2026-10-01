// The bench: its tools, its events, the queue to the frame thread, and how a
// call reaches its handler on the thread it runs on. MCP (Mcp.h) and REST
// (Rest.h) are two ways in to the one Invoke.
//
// Besides, the tools that need no game: `events` and `wait_for` over the
// event bus, and `log` over DAOLib's log folder.
#pragma once

#include "core/EventBus.h"
#include "core/FrameQueue.h"
#include "core/Tool.h"
#include "core/ToolRegistry.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace eluvian
{

class UnknownTool : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// One call of a tool, as it ended: for a record of what the bench was
// asked to do, which the events alone do not show.
struct CallRecord
{
    std::string_view tool;
    const Json &args;
    Thread thread = Thread::Frame;
    double ms = 0;          // from the call to its answer
    std::string_view error; // empty when it answered
};

struct BenchOptions
{
    // How long a frame tool's call may wait for a frame to take it.
    std::chrono::milliseconds frameTimeout{5000};
    std::size_t eventCapacity = 4096;
    EventBus::FrameSource frame = {}; // stamps each event with the frame it came in
    // Told of every call as it ends, on the thread that made it (a listener's).
    std::function<void(const CallRecord &)> onCall = {};
};

class Bench
{
  public:
    explicit Bench(BenchOptions options = {});

    ToolRegistry &Tools()
    {
        return tools_;
    }
    [[nodiscard]] const ToolRegistry &Tools() const
    {
        return tools_;
    }
    EventBus &Events()
    {
        return events_;
    }
    EventCatalog &Topics()
    {
        return topics_;
    }
    FrameQueue &Frames()
    {
        return frames_;
    }

    // Resolves the tool (and its kind) and runs its handler on its thread.
    // Throws UnknownTool, ToolError, FrameTimeout, or what the handler threw.
    ToolResult Invoke(std::string_view tool, const Json &args);

    // Wakes every waiter and refuses every frame call: the plugin is going.
    void Close();

  private:
    BenchOptions options_;
    ToolRegistry tools_;
    EventBus events_;
    EventCatalog topics_;
    FrameQueue frames_;
};

// `events` and `wait_for`.
// `whyNothing`, if given, is asked when a wait_for's time runs out: what the
// host knows of why nothing happened (the game is paused), as an object
// merged into the answer. On a listener's thread.
void AddEventTools(Bench &bench, std::function<Json()> whyNothing = {});

// `log`: the *.log files in `directory` (DAOLib's log folder), read from a
// cursor.
void AddLogTool(Bench &bench, std::filesystem::path directory);

} // namespace eluvian
