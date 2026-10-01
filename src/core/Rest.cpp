#include "core/Rest.h"

#include "core/Args.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>

namespace eluvian
{

namespace
{

RestResponse Error(int status, std::string message)
{
    return RestResponse{status, Json{{"error", std::move(message)}}};
}

} // namespace

RestEndpoint::RestEndpoint(Bench &bench) : bench_(bench)
{
}

RestResponse RestEndpoint::Call(std::string_view tool, const Json &args)
{
    try
    {
        ToolResult answer = bench_.Invoke(tool, args);
        if (!answer.content.empty())
        {
            return RestResponse{200, Json{{"data", std::move(answer.data)}, {"content", std::move(answer.content)}}};
        }
        return RestResponse{200, std::move(answer.data)};
    }
    catch (const UnknownTool &e)
    {
        return Error(404, e.what());
    }
    catch (const ToolError &e)
    {
        return Error(400, e.what());
    }
    catch (const FrameTimeout &e)
    {
        return Error(504, std::string(tool) + ": " + e.what() + "; GET /api/health says why");
    }
    catch (const std::exception &e)
    {
        return Error(500, std::string(tool) + " failed inside the bench: " + e.what());
    }
}

RestResponse RestEndpoint::Handle(std::string_view method, std::string_view path, const Query &query,
                                  std::string_view body)
{
    constexpr std::string_view kToolPrefix = "/api/tool/";
    if (method == "GET" && path == "/api/health")
    {
        return Call("health", Json::object());
    }
    if (method == "GET" && path == "/api/tools")
    {
        return RestResponse{200, Json{{"revision", bench_.Tools().Revision()}, {"tools", bench_.Tools().Describe()}}};
    }
    if (method == "GET" && path == "/api/events")
    {
        Json args = Json::object();
        for (const auto &[key, value] : query)
        {
            if (key == "since" || key == "limit")
            {
                std::int64_t n = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
                if (error != std::errc{} || end != value.data() + value.size())
                {
                    return Error(400, "`" + key + "` must be a whole number");
                }
                args[key] = n;
            }
            else if (key == "topic")
            {
                args[key] = value;
            }
        }
        return Call("events", args);
    }
    if (method == "POST" && path.starts_with(kToolPrefix))
    {
        const auto tool = path.substr(kToolPrefix.size());
        Json args = Json::object();
        if (body.find_first_not_of(" \t\r\n") != std::string_view::npos)
        {
            args = Json::parse(body, nullptr, false);
            if (args.is_discarded())
            {
                return Error(400, "the body is not JSON: it should be the tool's arguments, an object");
            }
        }
        return Call(tool, args);
    }
    return Error(404, "no route " + std::string(method) + " " + std::string(path) +
                          "; GET /api/tools lists the tools, POST /api/tool/<name> calls one");
}

} // namespace eluvian
