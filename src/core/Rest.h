// Plain HTTP over the bench, for scripts and anything that is not an MCP
// client: the same tools as MCP, reflected from the same registry
// (devbench's RestAdapter).
//
//   GET  /api/health                    the `health` tool
//   GET  /api/tools                     every tool's description and schema
//   POST /api/tool/<name>               call a tool; the body is its arguments
//   GET  /api/events?since=&topic=&limit=   the `events` tool
//
// A tool's answer is the body, as JSON; a failure is {"error": "..."} with a
// status saying what kind: 400 the arguments, 404 no such tool or route,
// 504 no frame took the call, 500 the bench itself.
#pragma once

#include "core/Bench.h"
#include "core/Tool.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eluvian
{

// A request's query, its fields in the order written.
using Query = std::vector<std::pair<std::string, std::string>>;

struct RestResponse
{
    int status = 200;
    Json body;
};

class RestEndpoint
{
  public:
    explicit RestEndpoint(Bench &bench);

    RestResponse Handle(std::string_view method, std::string_view path, const Query &query, std::string_view body);

  private:
    RestResponse Call(std::string_view tool, const Json &args);

    Bench &bench_;
};

} // namespace eluvian
