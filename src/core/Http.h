// HTTP/1.1 as the bench speaks it: requests parsed from bytes, responses
// written to bytes, and the routes from a request to MCP or REST. The socket
// loop (src/server) only moves bytes, so everything here is tested without
// one.
//
// Why not a library: cpp-httplib, the obvious one, refuses 32-bit Windows
// (vcpkg: "only supported on '!x86 & !arm32'", 2026-09-30), and the game is
// 32-bit. What the bench needs is small: requests with a Content-Length or
// chunked body, keep-alive, and JSON answers, from local clients.
#pragma once

#include "core/Mcp.h"
#include "core/Rest.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eluvian
{

struct HttpRequest
{
    std::string method;
    std::string path; // decoded, without the query
    Query query;      // in the order written; a vector, which moves without allocating as a map would not
    std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
    std::string body;
    bool keepAlive = true;

    // The first header of that name (lower-case), or empty.
    [[nodiscard]] std::string_view Header(std::string_view name) const;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    static HttpResponse OfJson(int status, const Json &body);
};

// The response as bytes, with Content-Length and Connection.
std::string Serialize(const HttpResponse &response, bool keepAlive);

// Percent-decoding; '+' is a space only in a query.
std::string UrlDecode(std::string_view text, bool query);

// Requests from a connection's bytes, one at a time.
class HttpParser
{
  public:
    enum class Status
    {
        NeedMore,
        Complete,
        Error,
    };

    static constexpr std::size_t kMaxHead = 64 * 1024;
    static constexpr std::size_t kMaxBody = 16 * 1024 * 1024;

    // Adds bytes and tries to finish a request. After Complete, Take() it;
    // bytes past it stay for the next. After Error, the connection is
    // answered with ErrorResponse() and closed.
    Status Feed(std::string_view bytes);
    // Tries again on what is buffered (after a Take, a pipelined request).
    Status Parse();
    HttpRequest Take();
    [[nodiscard]] HttpResponse ErrorResponse() const;

  private:
    Status Fail(int status, std::string message);
    Status ParseChunked(std::size_t bodyStart);

    std::string buffer_;
    HttpRequest request_;
    int errorStatus_ = 0;
    std::string errorMessage_;
};

// Where a request goes: POST /mcp to MCP, /api/* to REST, after the Origin
// check (core/Origin.h).
HttpResponse Route(const HttpRequest &request, McpEndpoint &mcp, RestEndpoint &rest);

} // namespace eluvian
