// Whether a request's Origin header may reach the bench.
//
// The bench has no auth and drives the game, so it answers only on
// 127.0.0.1; but a web page in the user's browser can still send requests to
// 127.0.0.1, and DNS rebinding can give a hostile page a local-looking host.
// MCP's streamable HTTP transport requires servers to check Origin for that
// reason. Clients that are not browsers (Claude Code, curl, scripts) send no
// Origin at all, and pass.
#pragma once

#include <string_view>

namespace eluvian
{

// True for no Origin, or one whose host is localhost or 127.0.0.1 (any port,
// http or https).
bool IsLocalOrigin(std::string_view origin);

} // namespace eluvian
