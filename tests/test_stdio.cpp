#include "core/Stdio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sstream>
#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

struct Fixture
{
    Bench bench;
    McpEndpoint mcp{bench, ServerInfo{"ghilana", "0.1.0", ""}};

    Fixture()
    {
        bench.Tools().Add(ToolSpec{"echo", "Echo.", Json{{"type", "object"}}, Thread::Listener, true},
                          [](const Json &args) -> ToolResult { return Json{{"args", args}}; });
        bench.Tools().Add(ToolSpec{"bytes", "Text that is not UTF-8.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { return Json{{"name", std::string("caf\xE9")}}; });
    }
};

} // namespace

TEST_CASE("a line holding a request is answered on one line", "[stdio]")
{
    Fixture f;
    const auto reply = McpLine(f.mcp, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    REQUIRE(reply);
    CHECK(reply->find('\n') == std::string::npos);
    CHECK(Json::parse(*reply)["id"] == 1);
    CHECK(Json::parse(*reply)["result"] == Json::object());
}

TEST_CASE("a blank line and a notification have no reply", "[stdio]")
{
    Fixture f;
    CHECK_FALSE(McpLine(f.mcp, ""));
    CHECK_FALSE(McpLine(f.mcp, "  \r"));
    CHECK_FALSE(McpLine(f.mcp, R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
}

TEST_CASE("a line that is not JSON is answered with the parse error", "[stdio]")
{
    Fixture f;
    const auto reply = McpLine(f.mcp, "{nope");
    REQUIRE(reply);
    const auto json = Json::parse(*reply);
    CHECK(json["error"]["code"] == -32700);
    CHECK(json["id"].is_null());
}

TEST_CASE("a line ended by a carriage return is read as without it", "[stdio]")
{
    Fixture f;
    const auto reply = McpLine(f.mcp, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\r");
    REQUIRE(reply);
    CHECK(Json::parse(*reply)["id"] == 7);
}

TEST_CASE("an answer holding text that is not UTF-8 is sent, not thrown on", "[stdio]")
{
    Fixture f;
    const auto reply =
        McpLine(f.mcp, R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"bytes","arguments":{}}})");
    REQUIRE(reply);
    CHECK_THAT(*reply, ContainsSubstring("caf"));
    const auto json = Json::parse(*reply);
    CHECK(json["result"]["isError"] == false);
}

TEST_CASE("ServeStdio answers each line in order, one reply a line, to the stream's end", "[stdio]")
{
    Fixture f;
    std::istringstream in("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
                          "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                          "\n"
                          "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
                          "\"arguments\":{\"a\":1}}}\n"
                          "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
    std::ostringstream out;
    ServeStdio(f.mcp, in, out);
    std::istringstream replies(out.str());
    std::string line;
    REQUIRE(std::getline(replies, line));
    CHECK(Json::parse(line)["result"]["serverInfo"]["name"] == "ghilana");
    REQUIRE(std::getline(replies, line));
    CHECK(Json::parse(line)["result"]["structuredContent"]["args"]["a"] == 1);
    REQUIRE(std::getline(replies, line));
    CHECK(Json::parse(line)["id"] == 3);
    CHECK_FALSE(std::getline(replies, line));
}
