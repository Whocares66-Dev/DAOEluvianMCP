// Every tool the bench serves, by name: the one list both MCP and REST
// reflect, so a tool registered once is reachable over both (devbench's
// ToolRegistry, C:\modding\devbench\src\ToolRegistry.h).
//
// A tool is one concept with a handler and its own arguments. Tools made of
// kinds (inspect's `kind`, stage's `action`), each kind a concept, one
// schema all their arguments, were split into a tool each (the user,
// 2026-10-09, #110): a tool is not a box of concepts for the sake of a
// shorter list.
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

// What to run for one call.
struct Call
{
    std::string tool;
    Thread thread = Thread::Frame;
    ToolHandler handler;
};

class ToolRegistry
{
  public:
    // Adds a tool, or replaces the one of that name. True if the name was
    // new. Throws std::invalid_argument for a tool without a handler.
    bool Add(ToolSpec spec, ToolHandler handler);

    // The call for a tool; nullopt if there is no such tool.
    [[nodiscard]] std::optional<Call> Resolve(std::string_view tool) const;

    // The arguments a call gave that its tool's schema does not list, each
    // with what to say of it: the nearest it does list, or what it takes.
    // Empty for a tool whose schema lists none, which cannot tell.
    [[nodiscard]] Json Unknown(std::string_view tool, const Json &args) const;

    // The arguments its tool's schema requires that a call left out, in the
    // schema's order. Empty for a tool that requires none.
    [[nodiscard]] std::vector<std::string> Missing(std::string_view tool, const Json &args) const;

    // MCP's tools/list `tools` array, sorted by name. Unlisted tools are left
    // out.
    [[nodiscard]] Json Describe() const;

    // Every tool's name, listed or not.
    [[nodiscard]] std::vector<std::string> Names() const;

    // Bumped by every Add: a client that listed before can tell
    // its list is stale.
    [[nodiscard]] std::uint64_t Revision() const;

  private:
    struct Entry
    {
        ToolSpec spec;
        ToolHandler handler;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> tools_;
    std::uint64_t revision_ = 0;
};

} // namespace eluvian
