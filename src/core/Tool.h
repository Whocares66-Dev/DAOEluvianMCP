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
#include <functional>
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

struct ToolSpec
{
    std::string name;
    std::string description;
    Json inputSchema = Json{{"type", "object"}};
    Thread thread = Thread::Frame;
    bool readOnly = false; // MCP's readOnlyHint: changes nothing in the game
    // For a tool made of kinds (one without a handler of its own): the
    // argument that chooses the kind, `kind` for inspect, `action` for stage.
    std::string kindProperty = "kind";
};

} // namespace eluvian
