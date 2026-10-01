#include "core/Http.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;
using Status = HttpParser::Status;

TEST_CASE("a POST with a Content-Length body parses whole", "[http]")
{
    HttpParser parser;
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    REQUIRE(parser.Feed("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:8930\r\nContent-Type: application/json\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body) == Status::Complete);
    const auto request = parser.Take();
    CHECK(request.method == "POST");
    CHECK(request.path == "/mcp");
    CHECK(request.Header("content-type") == "application/json");
    CHECK(request.Header("missing").empty());
    CHECK(request.body == body);
    CHECK(request.keepAlive);
}

TEST_CASE("bytes arriving in pieces wait for the rest", "[http]")
{
    HttpParser parser;
    CHECK(parser.Feed("POST /api/tool/echo HTTP/1.1\r\nContent-Len") == Status::NeedMore);
    CHECK(parser.Feed("gth: 4\r\n\r\n{}") == Status::NeedMore);
    CHECK(parser.Feed("  ") == Status::Complete);
    CHECK(parser.Take().body == "{}  ");
}

TEST_CASE("two requests in one read come out one after the other", "[http]")
{
    HttpParser parser;
    REQUIRE(parser.Feed("GET /api/health HTTP/1.1\r\n\r\nGET /api/tools HTTP/1.1\r\nConnection: close\r\n\r\n") ==
            Status::Complete);
    CHECK(parser.Take().path == "/api/health");
    REQUIRE(parser.Parse() == Status::Complete);
    const auto second = parser.Take();
    CHECK(second.path == "/api/tools");
    CHECK_FALSE(second.keepAlive);
    CHECK(parser.Parse() == Status::NeedMore);
}

TEST_CASE("the query and path are decoded", "[http]")
{
    HttpParser parser;
    REQUIRE(parser.Feed("GET /api/events?since=12&topic=turnbased.%2A&flag&x=a+b HTTP/1.1\r\n\r\n") ==
            Status::Complete);
    const auto request = parser.Take();
    CHECK(request.path == "/api/events");
    CHECK(request.query == Query{{"since", "12"}, {"topic", "turnbased.*"}, {"flag", ""}, {"x", "a b"}});
    CHECK(UrlDecode("a+b%20c%zz%4", false) == "a+b c%zz%4");
}

TEST_CASE("a chunked body is put together", "[http]")
{
    HttpParser parser;
    CHECK(parser.Feed("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"a\"\r\n") == Status::NeedMore);
    CHECK(parser.Feed("3;ext=1\r\n:1}\r\n0\r\n\r\n") == Status::Complete);
    CHECK(parser.Take().body == R"({"a":1})");
}

TEST_CASE("a chunked body may end with trailers", "[http]")
{
    HttpParser parser;
    REQUIRE(parser.Feed("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\nX-T: 1\r\n\r\nGET "
                        "/api/health HTTP/1.1\r\n\r\n") == Status::Complete);
    CHECK(parser.Take().body == "{}");
    REQUIRE(parser.Parse() == Status::Complete);
    CHECK(parser.Take().path == "/api/health");
}

TEST_CASE("HTTP/1.0 closes unless asked to keep alive", "[http]")
{
    HttpParser parser;
    REQUIRE(parser.Feed("GET / HTTP/1.0\r\n\r\n") == Status::Complete);
    CHECK_FALSE(parser.Take().keepAlive);
    REQUIRE(parser.Feed("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == Status::Complete);
    CHECK(parser.Take().keepAlive);
}

TEST_CASE("malformed requests are errors with a status", "[http]")
{
    const auto statusOf = [](const std::string &bytes) {
        HttpParser parser;
        return parser.Feed(bytes) == Status::Error ? parser.ErrorResponse().status : 0;
    };
    CHECK(statusOf("NONSENSE\r\n\r\n") == 400);
    CHECK(statusOf("GET /x HTTP/2\r\n\r\n") == 400);
    CHECK(statusOf("GET x HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("GET /x HTTP/1.1\r\nno colon\r\n\r\n") == 400);
    CHECK(statusOf("POST /x HTTP/1.1\r\nContent-Length: ten\r\n\r\n") == 400);
    CHECK(statusOf("POST /x HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n") == 413);
    CHECK(statusOf("POST /x HTTP/1.1\r\nContent-Length: 999999999999999999999999\r\n\r\n") == 413);
    CHECK(statusOf("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n") == 400);
    CHECK(statusOf("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabc\r\n") == 400);
    CHECK(statusOf("GET /x HTTP/1.1\r\n" + std::string(HttpParser::kMaxHead + 1, 'a')) == 431);
}

TEST_CASE("a response is written with its length and connection", "[http]")
{
    const auto bytes = Serialize(HttpResponse::OfJson(200, Json{{"ok", true}}), true);
    CHECK(bytes == "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: "
                   "keep-alive\r\n\r\n{\"ok\":true}");
    HttpResponse accepted{202, "application/json", {}, {{"X-A", "b"}}};
    CHECK(Serialize(accepted, false) ==
          "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\nX-A: b\r\n\r\n");
}

namespace
{

struct Routes
{
    Bench bench;
    McpEndpoint mcp{bench, ServerInfo{"eluvian", "0.1.0", ""}};
    RestEndpoint rest{bench};

    Routes()
    {
        bench.Tools().Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true},
                          [](const Json &) -> ToolResult { return Json{{"ok", true}}; });
    }

    HttpResponse Send(std::string method, std::string path, std::string body = {}, std::string origin = {})
    {
        HttpRequest request;
        request.method = std::move(method);
        request.path = std::move(path);
        request.body = std::move(body);
        if (!origin.empty())
        {
            request.headers.emplace_back("origin", std::move(origin));
        }
        return Route(request, mcp, rest);
    }
};

} // namespace

TEST_CASE("POST /mcp is MCP; a notification is 202 with no body", "[http]")
{
    Routes routes;
    auto response = routes.Send("POST", "/mcp", R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    CHECK(response.status == 200);
    CHECK(Json::parse(response.body)["id"] == 7);

    response = routes.Send("POST", "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(response.status == 202);
    CHECK(response.body.empty());

    response = routes.Send("POST", "/mcp", "{oops");
    CHECK(response.status == 400);
    CHECK(Json::parse(response.body)["error"]["code"] == -32700);
}

TEST_CASE("GET /mcp has no stream, and says so", "[http]")
{
    Routes routes;
    const auto response = routes.Send("GET", "/mcp");
    CHECK(response.status == 405);
    CHECK(response.headers.at(0) == std::pair<std::string, std::string>{"Allow", "POST"});
}

TEST_CASE("REST is under /api/ and anything else is a 404 that says where to go", "[http]")
{
    Routes routes;
    CHECK(Json::parse(routes.Send("GET", "/api/health").body) == Json{{"ok", true}});
    const auto response = routes.Send("GET", "/");
    CHECK(response.status == 404);
    CHECK_THAT(response.body, ContainsSubstring("POST /mcp"));
}

TEST_CASE("a page from elsewhere is refused", "[http]")
{
    Routes routes;
    CHECK(routes.Send("POST", "/mcp", "{}", "https://example.com").status == 403);
    CHECK(routes.Send("GET", "/api/health", {}, "http://localhost:5173").status == 200);
}
