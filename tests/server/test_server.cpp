#include "server/HttpServer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace eluvian;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A bare HTTP client: one connection, requests written by hand, a response
// read to its Content-Length.
class Client
{
  public:
    explicit Client(std::uint16_t port)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            return;
        }
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        connected_ = connect(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0;
        const DWORD timeout = 5000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
    }
    ~Client()
    {
        closesocket(socket_);
        WSACleanup();
    }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    [[nodiscard]] bool Connected() const
    {
        return connected_;
    }

    // Sends a request and returns the whole response, head and body; empty
    // if the server closed first.
    std::string Exchange(const std::string &method, const std::string &path, const std::string &body = {},
                         const std::string &extraHeaders = {})
    {
        const std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extraHeaders +
                                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        SendRaw(request);
        return ReadResponse();
    }

    void SendRaw(const std::string &bytes)
    {
        send(socket_, bytes.data(), static_cast<int>(bytes.size()), 0);
    }

    std::string ReadResponse()
    {
        while (true)
        {
            const auto headEnd = pending_.find("\r\n\r\n");
            if (headEnd != std::string::npos)
            {
                const auto at = pending_.find("Content-Length: ");
                const auto length = std::stoul(pending_.substr(at + 16));
                if (pending_.size() >= headEnd + 4 + length)
                {
                    std::string response = pending_.substr(0, headEnd + 4 + length);
                    pending_.erase(0, headEnd + 4 + length);
                    return response;
                }
            }
            char buffer[4096];
            const int received = recv(socket_, buffer, sizeof buffer, 0);
            if (received <= 0)
            {
                return {};
            }
            pending_.append(buffer, static_cast<std::size_t>(received));
        }
    }

  private:
    SOCKET socket_ = INVALID_SOCKET;
    bool connected_ = false;
    std::string pending_;
};

std::string BodyOf(const std::string &response)
{
    return response.substr(response.find("\r\n\r\n") + 4);
}

struct Fixture
{
    Bench bench;
    McpEndpoint mcp{bench, ServerInfo{"eluvian", "0.1.0", "Start with health."}};
    RestEndpoint rest{bench};
    HttpServer server{[this](const HttpRequest &request) { return Route(request, mcp, rest); }};
    std::uint16_t port = 0;

    Fixture()
    {
        bench.Tools().Add(ToolSpec{"health", "Alive?", {}, Thread::Listener, true},
                          [](const Json &) -> ToolResult { return Json{{"ok", true}}; });
        AddEventTools(bench);
        port = server.Start(0, 1);
    }
};

} // namespace

TEST_CASE("an MCP client initializes and lists the tools over one connection", "[server]")
{
    Fixture f;
    REQUIRE(f.port != 0);
    Client client(f.port);
    REQUIRE(client.Connected());

    auto response = client.Exchange(
        "POST", "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
        "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n");
    REQUIRE_THAT(response, ContainsSubstring("HTTP/1.1 200 OK"));
    CHECK_THAT(response, ContainsSubstring("Connection: keep-alive"));
    CHECK(Json::parse(BodyOf(response))["result"]["serverInfo"]["name"] == "eluvian");

    response = client.Exchange("POST", "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK_THAT(response, ContainsSubstring("HTTP/1.1 202 Accepted"));

    response = client.Exchange("POST", "/mcp", R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    CHECK(Json::parse(BodyOf(response))["result"]["tools"].size() == 3);
}

TEST_CASE("REST answers over the same port", "[server]")
{
    Fixture f;
    Client client(f.port);
    const auto response = client.Exchange("GET", "/api/health");
    CHECK(Json::parse(BodyOf(response)) == Json{{"ok", true}});
}

TEST_CASE("a wait_for on one connection sees an event emitted meanwhile", "[server]")
{
    Fixture f;
    Client client(f.port);
    std::thread emitter([&f] {
        std::this_thread::sleep_for(50ms);
        f.bench.Events().Emit("turnbased.turn.started", Json{{"id", "0x82"}});
    });
    const auto response = client.Exchange("POST", "/api/tool/wait_for", R"({"topic":"turnbased.*","timeoutMs":5000})");
    emitter.join();
    CHECK(Json::parse(BodyOf(response))["event"]["payload"]["id"] == "0x82");
}

TEST_CASE("a malformed request is answered and the connection closed", "[server]")
{
    Fixture f;
    Client client(f.port);
    client.SendRaw("NONSENSE\r\n\r\n");
    const auto response = client.ReadResponse();
    CHECK_THAT(response, ContainsSubstring("HTTP/1.1 400 Bad Request"));
    CHECK_THAT(response, ContainsSubstring("Connection: close"));
    CHECK(client.ReadResponse().empty());
}

TEST_CASE("a port in use moves to the next", "[server]")
{
    Fixture f;
    HttpServer second([](const HttpRequest &) { return HttpResponse::OfJson(200, Json::object()); });
    const auto port = second.Start(f.port, 5);
    CHECK(port != 0);
    CHECK(port != f.port);
    HttpServer third([](const HttpRequest &) { return HttpResponse::OfJson(200, Json::object()); });
    CHECK(third.Start(f.port, 1) == 0);
}

TEST_CASE("a client that asks for a large answer and stops reading does not hold Stop", "[server]")
{
    // More than the socket's buffers hold, so the server's send must wait on
    // a client that never takes it.
    const std::string large(48 * 1024 * 1024, 'x');
    HttpServer server([&large](const HttpRequest &) { return HttpResponse{200, "text/plain", large, {}}; });
    const auto port = server.Start(0, 1);
    REQUIRE(port != 0);
    Client deaf(port);
    REQUIRE(deaf.Connected());
    deaf.SendRaw("GET /large HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    std::this_thread::sleep_for(300ms); // the server is sending by now, into full buffers

    const auto start = std::chrono::steady_clock::now();
    server.Stop();
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("Stop returns with a client still connected", "[server]")
{
    Fixture f;
    Client idle(f.port);
    REQUIRE(idle.Connected());
    idle.Exchange("GET", "/api/health");
    const auto start = std::chrono::steady_clock::now();
    f.server.Stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(idle.ReadResponse().empty()); // the server closed it
}
