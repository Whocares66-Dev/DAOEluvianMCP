// MCP over stdio: one JSON-RPC message a line in, one reply a line out, as
// MCP's stdio transport says (a message never holds a newline; the server
// writes nothing to stdout that is not a message). What ghilana, a process
// its client starts, is served over (#128); Eluvian, in the game, is served
// over HTTP (Http.h) and its bridge carries it to stdio.
//
// Transport-free like Mcp.h: a line in, a line out, over streams the tests
// make of strings.
#pragma once

#include "core/Mcp.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace eluvian
{

// The reply to one line, as one line without its newline; nothing for a
// blank line or one that wants no reply (a notification, a response). A
// line that is not JSON is answered with JSON-RPC's parse error.
std::optional<std::string> McpLine(McpEndpoint &endpoint, std::string_view line);

// Reads lines from `in` until it ends, writing each reply and a newline to
// `out` and flushing it, so a client waiting on a reply has it.
void ServeStdio(McpEndpoint &endpoint, std::istream &in, std::ostream &out);

// JSON as one line, with text that is not UTF-8 replaced rather than
// thrown on: a resource's name or a file's string may be anything.
std::string OneLine(const Json &value);

} // namespace eluvian
