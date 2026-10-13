#include "core/Mcp.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace eluvian
{

namespace
{

// JSON-RPC 2.0's error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

// An exception that becomes a JSON-RPC error rather than a tool result.
struct RpcError
{
    int code;
    std::string message;
};

Json ErrorReply(const Json &id, int code, std::string_view message)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json TextResult(std::string text, bool isError)
{
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", std::move(text)}}})}, {"isError", isError}};
}

} // namespace

McpEndpoint::McpEndpoint(Bench &bench, ServerInfo info) : bench_(bench), info_(std::move(info))
{
}

Json McpEndpoint::ParseError(std::string_view what)
{
    return ErrorReply(nullptr, kParseError, std::string("Parse error: ") + std::string(what));
}

std::optional<Json> McpEndpoint::Handle(const Json &body)
{
    if (!body.is_array())
    {
        return HandleOne(body);
    }
    if (body.empty())
    {
        return ErrorReply(nullptr, kInvalidRequest, "Invalid Request: an empty batch");
    }
    Json replies = Json::array();
    for (const auto &message : body)
    {
        if (auto reply = HandleOne(message))
        {
            replies.push_back(std::move(*reply));
        }
    }
    if (replies.empty())
    {
        return std::nullopt;
    }
    return replies;
}

std::optional<Json> McpEndpoint::HandleOne(const Json &message)
{
    if (!message.is_object() || message.value("jsonrpc", "") != "2.0")
    {
        return ErrorReply(nullptr, kInvalidRequest, "Invalid Request: not a JSON-RPC 2.0 message");
    }
    const auto method = message.find("method");
    const auto id = message.find("id");
    if (method == message.end())
    {
        // A response to a request of ours; we make none, so there is nothing to do.
        return std::nullopt;
    }
    if (!method->is_string())
    {
        return ErrorReply(id == message.end() ? Json(nullptr) : *id, kInvalidRequest, "Invalid Request: no method");
    }
    if (id == message.end())
    {
        return std::nullopt; // a notification: notifications/initialized, notifications/cancelled
    }
    const auto &name = method->get_ref<const std::string &>();
    const Json params = message.value("params", Json::object());
    try
    {
        Json result;
        if (name == "initialize")
        {
            result = Initialize(params);
        }
        else if (name == "ping")
        {
            result = Json::object();
        }
        else if (name == "tools/list")
        {
            result = Json{{"tools", bench_.Tools().Describe()}};
        }
        else if (name == "tools/call")
        {
            result = CallTool(params);
        }
        else
        {
            return ErrorReply(*id, kMethodNotFound, "Method not found: " + name);
        }
        return Json{{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(result)}};
    }
    catch (const RpcError &error)
    {
        return ErrorReply(*id, error.code, error.message);
    }
}

Json McpEndpoint::Initialize(const Json &params) const
{
    const std::string asked = params.value("protocolVersion", "");
    const auto known = std::ranges::find(kProtocolVersions, asked);
    const std::string_view version = known != kProtocolVersions.end() ? *known : kProtocolVersions.front();
    Json result = {
        {"protocolVersion", version},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", info_.name}, {"version", info_.version}}},
    };
    if (!info_.instructions.empty())
    {
        result["instructions"] = info_.instructions;
    }
    return result;
}

Json McpEndpoint::CallTool(const Json &params)
{
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
    {
        throw RpcError{kInvalidParams, "Invalid params: tools/call needs a tool `name`"};
    }
    const auto &name = params["name"].get_ref<const std::string &>();
    const Json args = params.value("arguments", Json::object());
    try
    {
        ToolResult answer = bench_.Invoke(name, args);
        // Replaced, not thrown on: text that is not UTF-8 (a file's string,
        // read by ghilana) would fail the whole answer (#128).
        Json result = TextResult(answer.data.dump(-1, ' ', false, Json::error_handler_t::replace), false);
        for (auto &item : answer.content)
        {
            result["content"].push_back(std::move(item));
        }
        if (answer.data.is_object())
        {
            result["structuredContent"] = std::move(answer.data);
        }
        return result;
    }
    catch (const UnknownTool &e)
    {
        throw RpcError{kInvalidParams, e.what()};
    }
    catch (const ToolError &e)
    {
        return TextResult(e.what(), true);
    }
    catch (const FrameTimeout &e)
    {
        return TextResult(name + ": " + e.what() +
                              ". The game's frame is not running: an area loading, a modal dialog, a crash, or the "
                              "game closed. `health` says which.",
                          true);
    }
    catch (const std::exception &e)
    {
        return TextResult(name + " failed inside the bench: " + e.what(), true);
    }
}

} // namespace eluvian
