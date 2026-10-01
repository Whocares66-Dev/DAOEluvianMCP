#include "core/Rest.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

struct Fixture
{
    Bench bench{BenchOptions{std::chrono::milliseconds(10)}};
    RestEndpoint rest{bench};

    Fixture()
    {
        bench.Tools().Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true},
                          [](const Json &) -> ToolResult { return Json{{"ok", true}}; });
        bench.Tools().Add(ToolSpec{"echo", "Echo.", {}, Thread::Listener},
                          [](const Json &args) -> ToolResult { return Json{{"args", args}}; });
        bench.Tools().Add(ToolSpec{"refuse", "Refuse.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { throw ToolError("not now"); });
        bench.Tools().Add(ToolSpec{"crash", "Crash.", {}, Thread::Listener},
                          [](const Json &) -> ToolResult { throw std::logic_error("a bug"); });
        bench.Tools().Add(ToolSpec{"frame", "Needs a frame."}, [](const Json &) -> ToolResult { return 1; });
        AddEventTools(bench);
    }
};

} // namespace

TEST_CASE("GET /api/health is the health tool", "[rest]")
{
    Fixture f;
    const auto response = f.rest.Handle("GET", "/api/health", {}, "");
    CHECK(response.status == 200);
    CHECK(response.body == Json{{"ok", true}});
}

TEST_CASE("GET /api/tools lists the tools and the revision", "[rest]")
{
    Fixture f;
    const auto response = f.rest.Handle("GET", "/api/tools", {}, "");
    CHECK(response.status == 200);
    CHECK(response.body["tools"].size() == 7);
    CHECK(response.body["revision"] == f.bench.Tools().Revision());
}

TEST_CASE("POST /api/tool/<name> calls the tool with the body as its arguments", "[rest]")
{
    Fixture f;
    auto response = f.rest.Handle("POST", "/api/tool/echo", {}, R"({"id":"0x7d9"})");
    CHECK(response.status == 200);
    CHECK(response.body["args"]["id"] == "0x7d9");

    response = f.rest.Handle("POST", "/api/tool/echo", {}, "  \r\n");
    CHECK(response.body["args"] == Json::object());
}

TEST_CASE("each failure has its status and a sentence", "[rest]")
{
    Fixture f;
    CHECK(f.rest.Handle("POST", "/api/tool/echo", {}, "{not json").status == 400);
    CHECK(f.rest.Handle("POST", "/api/tool/refuse", {}, "").body == Json{{"error", "not now"}});
    CHECK(f.rest.Handle("POST", "/api/tool/refuse", {}, "").status == 400);
    CHECK(f.rest.Handle("POST", "/api/tool/nothing", {}, "").status == 404);
    CHECK(f.rest.Handle("POST", "/api/tool/crash", {}, "").status == 500);
    const auto timeout = f.rest.Handle("POST", "/api/tool/frame", {}, "");
    CHECK(timeout.status == 504);
    CHECK_THAT(timeout.body["error"].get<std::string>(), ContainsSubstring("/api/health"));
    const auto unrouted = f.rest.Handle("DELETE", "/api/tool/echo", {}, "");
    CHECK(unrouted.status == 404);
    CHECK_THAT(unrouted.body["error"].get<std::string>(), ContainsSubstring("GET /api/tools"));
}

TEST_CASE("GET /api/events reads the bus from the query", "[rest]")
{
    Fixture f;
    f.bench.Events().Emit("a", Json::object());
    f.bench.Events().Emit("b", Json::object());
    f.bench.Events().Emit("a", Json::object());
    auto response = f.rest.Handle("GET", "/api/events", Query{{"since", "2"}, {"topic", "a"}}, "");
    CHECK(response.status == 200);
    REQUIRE(response.body["events"].size() == 1);
    CHECK(response.body["events"][0]["seq"] == 3);
    CHECK(response.body["next"] == 4);

    response = f.rest.Handle("GET", "/api/events", Query{{"since", "two"}}, "");
    CHECK(response.status == 400);
}
