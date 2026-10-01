// MCP over the bench: JSON-RPC 2.0 messages in, replies out, as the
// streamable HTTP transport carries them (POST /mcp). Transport-free, so the
// whole protocol is tested without a socket; src/server only moves bytes.
//
// What is served: initialize (version negotiation, the server's
// instructions), ping, tools/list and tools/call. No resources, prompts or
// server-initiated stream: every request is answered in its own POST's
// body, which the transport allows, and a client that lists tools again
// sees tools registered since (ToolRegistry::Revision).
//
// A tool's failure is a result with isError, so the agent reads it and can
// correct itself; only a malformed request or an unknown tool is a JSON-RPC
// error, as the specification says.
#pragma once

#include "core/Bench.h"
#include "core/Tool.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace eluvian
{

struct ServerInfo
{
    std::string name;
    std::string version;
    std::string instructions; // initialize's `instructions`: how to use the bench, for the agent
};

class McpEndpoint
{
  public:
    // Newest first; a client asking for one of these gets it, and any other
    // gets the first.
    static constexpr std::array<std::string_view, 4> kProtocolVersions{"2025-11-25", "2025-06-18", "2025-03-26",
                                                                       "2024-11-05"};

    McpEndpoint(Bench &bench, ServerInfo info);

    // One POST body: a message, or a batch of them (protocol versions before
    // 2025-06-18). The reply to send, or nullopt when there is none (only
    // notifications and responses came: HTTP 202).
    std::optional<Json> Handle(const Json &body);

    // The reply to a body that is not JSON at all.
    static Json ParseError(std::string_view what);

  private:
    std::optional<Json> HandleOne(const Json &message);
    Json Initialize(const Json &params) const;
    Json CallTool(const Json &params);

    Bench &bench_;
    ServerInfo info_;
};

} // namespace eluvian
