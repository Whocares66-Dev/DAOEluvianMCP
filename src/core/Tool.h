// What a tool is, to every layer: its description (which is both MCP's
// tools/list entry and the REST API's documentation), the thread it runs on,
// and its handler, JSON in and JSON out.
//
// A handler answers the question asked: a read returns what was read, an
// order returns what the game did with it (devbench's "return data, not
// ok"). A handler that cannot do what it was asked throws ToolError with a
// sentence saying what was wrong and what to do instead; the agent reads it.
#pragma once

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace eluvian
{

// Keys in the order they were written: an answer reads id, name, then the
// rest, as a person would write it.
using Json = nlohmann::ordered_json;

// Where a handler runs. Frame: on the scripts' thread, from dao::OnFrame,
// the only place the game may be read or driven; the listener waits for it.
// Listener: on the HTTP listener's thread, for what needs no game (health,
// the event bus, logs) and must answer while the frame is stalled.
enum class Thread
{
    Frame,
    Listener,
};

// A tool's failure the agent can act on: isError in MCP, a 4xx in REST.
class ToolError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// A handler's answer: data (the JSON the agent reads; MCP's structured
// content when it is an object) and, rarely, content of another kind beside
// it (an image, as MCP content items).
struct ToolResult
{
    Json data;
    std::vector<Json> content;

    ToolResult() = default;
    // Implicit, so a handler returns its JSON, or anything JSON is made
    // from, as it is.
    template <class T>
        requires(!std::same_as<std::remove_cvref_t<T>, ToolResult> && std::constructible_from<Json, T>)
    ToolResult(T &&value) : data(std::forward<T>(value))
    {
    }
};

using ToolHandler = std::function<ToolResult(const Json &args)>;

struct Budget
{
    std::string argument;   // the argument that bounds a call, in milliseconds
    std::int64_t defaultMs; // its value where a call does not give it
};

struct ToolSpec
{
    std::string name;
    std::string description;
    Json inputSchema = Json{{"type", "object"}};
    Thread thread = Thread::Frame;
    bool readOnly = false; // MCP's readOnlyHint: changes nothing in the game
    // Whether tools/list shows it. An unlisted tool is another's step: a
    // frame thread's single act that a listener tool of the same job calls
    // and waits over (`dialogue.step` under `dialogue`, #17), so that the
    // agent meets one tool for the job, not two.
    bool listed = true;
    // The argument that bounds how long a call may take, and its default:
    // what a caller across the network waits for before it gives up. The
    // bridge named the tools it waited longer for itself, and cut off a
    // sweep of loot at 60 s (the user, 2026-10-09: "there should be a
    // single source of truth", #108). In tools/list as the tool's
    // _meta["eluvian/budget"]. `= {}` is not redundant: ToolSpec is built as an
    // aggregate leaving it out, and clang's missing-field-initializers wants
    // a field so left to have one.
    std::optional<Budget> budget = {}; // NOLINT(readability-redundant-member-init): above
};

// A tool's spec with its budget declared: `argument` (budgetMs,
// timeoutMs) bounds a call, `defaultMs` where it is not given.
inline ToolSpec Budgeted(ToolSpec spec, std::string argument, std::int64_t defaultMs)
{
    spec.budget = Budget{std::move(argument), defaultMs};
    return spec;
}

} // namespace eluvian
