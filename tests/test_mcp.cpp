#include "core/Mcp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

Json Request(int id, std::string method, Json params = Json::object())
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}, {"params", std::move(params)}};
}

// A bench with tools of each kind of answer, on the listener's thread so no
// frame is needed.
struct Fixture
{
    Bench bench;
    McpEndpoint mcp{bench, ServerInfo{"eluvian", "0.1.0", "Start with health."}};

    Fixture()
    {
        bench.Tools().Add(ToolSpec{"echo", "Echo.", Json{{"type", "object"}}, Thread::Listener, true},
                          [](const Json &args) -> ToolResult { return Json{{"args", args}}; });
        bench.Tools().Add(ToolSpec{"refuse", "Refuse.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { throw ToolError("refuse: not now"); });
        bench.Tools().Add(ToolSpec{"crash", "Crash.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { throw std::logic_error("a bug"); });
        bench.Tools().Add(ToolSpec{"count", "A bare number.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { return 3; });
        bench.Tools().Add(ToolSpec{"picture", "An image.", {}, Thread::Listener}, [](const Json &) {
            ToolResult result(Json{{"width", 2}});
            result.content.push_back(Json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}});
            return result;
        });
        bench.Tools().Add(ToolSpec{"frame", "Needs a frame.", {}, Thread::Frame},
                          [](const Json &) -> ToolResult { return 1; });
    }
};

} // namespace

TEST_CASE("initialize answers the version asked for, if known, with the instructions", "[mcp]")
{
    Fixture f;
    auto reply = f.mcp.Handle(Request(1, "initialize", Json{{"protocolVersion", "2025-06-18"}}));
    REQUIRE(reply);
    CHECK((*reply)["id"] == 1);
    CHECK((*reply)["result"]["protocolVersion"] == "2025-06-18");
    CHECK((*reply)["result"]["serverInfo"]["name"] == "eluvian");
    CHECK((*reply)["result"]["capabilities"].contains("tools"));
    CHECK((*reply)["result"]["instructions"] == "Start with health.");

    reply = f.mcp.Handle(Request(2, "initialize", Json{{"protocolVersion", "2099-01-01"}}));
    CHECK((*reply)["result"]["protocolVersion"].get<std::string>() == McpEndpoint::kProtocolVersions.front());
}

TEST_CASE("notifications and responses get no reply", "[mcp]")
{
    Fixture f;
    CHECK_FALSE(f.mcp.Handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    CHECK_FALSE(f.mcp.Handle(Json{{"jsonrpc", "2.0"}, {"id", 9}, {"result", Json::object()}}));
}

TEST_CASE("ping, and an unknown method", "[mcp]")
{
    Fixture f;
    const auto pong = f.mcp.Handle(Request(1, "ping"));
    CHECK((*pong)["result"].is_object());
    CHECK((*pong)["result"].empty());
    const auto reply = f.mcp.Handle(Request(2, "resources/list"));
    CHECK((*reply)["error"]["code"] == -32601);
}

TEST_CASE("malformed messages are invalid requests", "[mcp]")
{
    Fixture f;
    CHECK((*f.mcp.Handle(Json{{"id", 1}, {"method", "ping"}}))["error"]["code"] == -32600);
    CHECK((*f.mcp.Handle(Json("ping")))["error"]["code"] == -32600);
    CHECK((*f.mcp.Handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", 5}}))["error"]["code"] == -32600);
    CHECK((*f.mcp.Handle(Json::array()))["error"]["code"] == -32600);
    CHECK(McpEndpoint::ParseError("eof")["error"]["code"] == -32700);
}

TEST_CASE("tools/list is the registry's description", "[mcp]")
{
    Fixture f;
    const auto reply = f.mcp.Handle(Request(1, "tools/list"));
    const Json &tools = (*reply)["result"]["tools"];
    REQUIRE(tools.size() == 6);
    CHECK(tools[0]["name"] == "count");
    CHECK(tools[0].contains("inputSchema"));
}

TEST_CASE("tools/call returns the data as text and as structured content", "[mcp]")
{
    Fixture f;
    const auto reply = f.mcp.Handle(Request(1, "tools/call", Json{{"name", "echo"}, {"arguments", {{"id", "0x7d9"}}}}));
    const Json &result = (*reply)["result"];
    CHECK(result["isError"] == false);
    CHECK(Json::parse(result["content"][0]["text"].get<std::string>()) == Json{{"args", {{"id", "0x7d9"}}}});
    CHECK(result["structuredContent"]["args"]["id"] == "0x7d9");
}

TEST_CASE("an answer that is not an object has no structured content", "[mcp]")
{
    Fixture f;
    const auto reply = f.mcp.Handle(Request(1, "tools/call", Json{{"name", "count"}}));
    CHECK((*reply)["result"]["content"][0]["text"] == "3");
    CHECK_FALSE((*reply)["result"].contains("structuredContent"));
}

TEST_CASE("extra content follows the text", "[mcp]")
{
    Fixture f;
    const auto reply = f.mcp.Handle(Request(1, "tools/call", Json{{"name", "picture"}}));
    const Json &content = (*reply)["result"]["content"];
    REQUIRE(content.size() == 2);
    CHECK(content[1]["type"] == "image");
}

TEST_CASE("a tool's failure is a result the agent reads; an unknown tool is a protocol error", "[mcp]")
{
    Fixture f;
    auto reply = f.mcp.Handle(Request(1, "tools/call", Json{{"name", "refuse"}}));
    CHECK((*reply)["result"]["isError"] == true);
    CHECK((*reply)["result"]["content"][0]["text"] == "refuse: not now");

    reply = f.mcp.Handle(Request(2, "tools/call", Json{{"name", "crash"}}));
    CHECK((*reply)["result"]["isError"] == true);
    CHECK_THAT((*reply)["result"]["content"][0]["text"].get<std::string>(), ContainsSubstring("a bug"));

    reply = f.mcp.Handle(Request(3, "tools/call", Json{{"name", "nothing"}}));
    CHECK((*reply)["error"]["code"] == -32602);
    reply = f.mcp.Handle(Request(4, "tools/call", Json{{"arguments", Json::object()}}));
    CHECK((*reply)["error"]["code"] == -32602);
}

TEST_CASE("a frame tool with no frame says the frame is not running", "[mcp]")
{
    Bench bench(BenchOptions{std::chrono::milliseconds(10)});
    bench.Tools().Add(ToolSpec{"frame", "Needs a frame."}, [](const Json &) -> ToolResult { return 1; });
    McpEndpoint mcp(bench, ServerInfo{"eluvian", "0", ""});
    const auto reply = mcp.Handle(Request(1, "tools/call", Json{{"name", "frame"}}));
    CHECK((*reply)["result"]["isError"] == true);
    CHECK_THAT((*reply)["result"]["content"][0]["text"].get<std::string>(),
               ContainsSubstring("frame is not running") && ContainsSubstring("health"));
}

TEST_CASE("a batch is answered as a batch, without the notifications", "[mcp]")
{
    Fixture f;
    const Json batch = Json::array(
        {Request(1, "ping"), Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, Request(2, "ping")});
    const auto reply = f.mcp.Handle(batch);
    REQUIRE(reply);
    REQUIRE(reply->is_array());
    CHECK(reply->size() == 2);
    CHECK_FALSE(f.mcp.Handle(Json::array({Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}})));
}
