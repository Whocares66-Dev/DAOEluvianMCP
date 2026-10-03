// Every tool the bench serves, by name: the one list both MCP and REST
// reflect, so a tool registered once is reachable over both (devbench's
// ToolRegistry, C:\modding\devbench\src\ToolRegistry.h).
//
// A tool is either one handler, or a base whose work is split into kinds
// chosen by one argument (`kind` for inspect, `action` for stage): `inspect kind="creature"`, and kinds other
// plugins add (`inspect kind="mymod.state"`). A kind keeps the list of
// tools short while staying discoverable: adding one rewrites the base's
// schema (the kind's name joins the `kind` enum) and its description (a line
// per kind), so an agent reading tools/list sees it without a second call.
// devbench's README has the A/B test behind the rule that a kind must mean
// what its base tool's name says, or a cold agent never looks for it there.
//
// Thread-safe: plugins register from the frame thread while the listener
// lists and resolves.
#pragma once

#include "core/Tool.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eluvian
{

struct KindSpec
{
    std::string kind;
    std::string description;
    Thread thread = Thread::Frame;
};

// What to run for one call: the tool's handler, or its kind's.
struct Call
{
    std::string tool;
    std::string kind; // empty for a tool without kinds
    Thread thread = Thread::Frame;
    ToolHandler handler;
};

class ToolRegistry
{
  public:
    // Adds a tool, or replaces the one of that name (and its kinds). A tool
    // with no handler is a base for kinds. True if the name was new.
    bool Add(ToolSpec spec, ToolHandler handler = {});

    // Adds a kind to a base tool, or replaces the kind of that name. True if
    // the kind was new. Throws std::invalid_argument if there is no such
    // tool or it has a handler of its own.
    bool AddKind(std::string_view tool, KindSpec spec, ToolHandler handler);

    // The call for a tool and its arguments; nullopt if there is no such
    // tool. Throws ToolError for a base tool's missing or unknown kind,
    // naming the kinds there are.
    [[nodiscard]] std::optional<Call> Resolve(std::string_view tool, const Json &args) const;

    // MCP's tools/list `tools` array, sorted by name, with every base tool's
    // kinds written into its schema and description. Unlisted tools are left
    // out.
    [[nodiscard]] Json Describe() const;

    // Every tool's name, listed or not.
    [[nodiscard]] std::vector<std::string> Names() const;

    // Bumped by every Add and AddKind: a client that listed before can tell
    // its list is stale.
    [[nodiscard]] std::uint64_t Revision() const;

  private:
    struct Kind
    {
        KindSpec spec;
        ToolHandler handler;
    };
    struct Entry
    {
        ToolSpec spec;
        ToolHandler handler;
        std::map<std::string, Kind, std::less<>> kinds;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> tools_;
    std::uint64_t revision_ = 0;
};

} // namespace eluvian
