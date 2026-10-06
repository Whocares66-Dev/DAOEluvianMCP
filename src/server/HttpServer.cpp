#include "server/HttpServer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace eluvian
{

namespace
{

constexpr std::uintptr_t kNoSocket = INVALID_SOCKET;

// A connection idle this long is closed; clients reconnect on their own. A
// handler's own wait (wait_for, up to five minutes) is not idleness: the
// clock runs only while waiting to receive.
constexpr auto kIdle = std::chrono::seconds(120);

// How often a connection waiting to receive looks whether the server is
// stopping. Stop cannot wake a blocked recv on another thread (shutdown does
// not, on Windows), and closing the socket under it would let its handle be
// reused while the connection's thread still holds it.
constexpr long kPollMs = 200;

// True when the socket has bytes (or its end) to receive; false when the
// server stops or the connection has idled too long.
bool WaitReadable(SOCKET socket, const std::atomic<bool> &running)
{
    const auto deadline = std::chrono::steady_clock::now() + kIdle;
    while (running && std::chrono::steady_clock::now() < deadline)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval timeout{0, kPollMs * 1000};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready != 0)
        {
            return ready > 0; // readable, or an error recv will report
        }
    }
    return false;
}

// How long one send may wait for a client to take bytes. A client that
// asked for a large answer and stopped reading would otherwise hold its
// thread, and its place among the connections, for ever, and Stop with it
// (a review's finding). SO_SNDTIMEO on the socket: a send that waits this
// long fails, and the connection is closed.
constexpr DWORD kSendWaitMs = 5'000;

// Sends everything, in pieces small enough to look between them whether the
// server is stopping; false if the client stopped taking them, or it is.
bool SendAll(SOCKET socket, std::string_view bytes, const std::atomic<bool> &running)
{
    while (!bytes.empty())
    {
        if (!running)
        {
            return false;
        }
        const int chunk = static_cast<int>(std::min<std::size_t>(bytes.size(), 64 * 1024));
        const int sent =
            send(socket, bytes.data(), chunk, 0); // NOLINT(bugprone-suspicious-stringview-data-usage): sized
        if (sent <= 0)
        {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

} // namespace

struct HttpServer::Connection
{
    SOCKET socket = INVALID_SOCKET;
    std::thread thread;
    std::atomic<bool> done{false}; // Serve has closed the socket and returned
};

HttpServer::HttpServer(Handler handler) : handler_(std::move(handler)), listener_(kNoSocket)
{
}

HttpServer::~HttpServer()
{
    Stop();
}

std::uint16_t HttpServer::Start(std::uint16_t first, int tries)
{
    if (running_)
    {
        return 0;
    }
    WSADATA data{};
    if (!winsock_)
    {
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            return 0;
        }
        winsock_ = true;
    }
    for (int i = 0; i < tries; ++i)
    {
        const auto port = static_cast<std::uint16_t>(first == 0 ? 0 : first + i);
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        // INVALID_SOCKET is Winsock's macro, ~0 cast to SOCKET, read as signed by clang (#77).
        if (listener == INVALID_SOCKET) // NOLINT(modernize-use-integer-sign-comparison)
        {
            return 0;
        }
        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof exclusive);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0 &&
            listen(listener, SOMAXCONN) == 0)
        {
            int length = sizeof address;
            getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
            listener_ = listener;
            running_ = true;
            acceptor_ = std::thread([this] { Accept(); });
            return ntohs(address.sin_port);
        }
        closesocket(listener);
        if (first == 0)
        {
            break;
        }
    }
    return 0;
}

void HttpServer::Accept()
{
    const auto listener = static_cast<SOCKET>(listener_);
    while (running_)
    {
        if (!WaitReadable(listener, running_))
        {
            continue; // stopping, or no one came for a while
        }
        const SOCKET client = accept(listener, nullptr, nullptr);
        // INVALID_SOCKET is Winsock's macro, ~0 cast to SOCKET, read as signed by clang (#77).
        if (client == INVALID_SOCKET) // NOLINT(modernize-use-integer-sign-comparison)
        {
            Sleep(10); // out of resources for a moment; try again
            continue;
        }
        Reap();
        const BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof noDelay);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&kSendWaitMs), sizeof kSendWaitMs);

        const std::lock_guard lock(mutex_);
        if (connections_.size() >= kMaxConnections)
        {
            SendAll(client,
                    Serialize(HttpResponse::OfJson(503, Json{{"error", "too many connections to the bench"}}), false),
                    running_);
            closesocket(client);
            continue;
        }
        auto connection = std::make_shared<Connection>();
        connection->socket = client;
        connection->thread = std::thread([this, connection] {
            Serve(connection);
            connection->done = true;
        });
        connections_.push_back(std::move(connection));
    }
}

void HttpServer::Serve(const std::shared_ptr<Connection> &connection)
{
    HttpParser parser;
    std::string buffer(16 * 1024, '\0');
    bool open = true;
    while (open && WaitReadable(connection->socket, running_))
    {
        const int received = recv(connection->socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0)
        {
            break;
        }
        auto status = parser.Feed(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
        while (status == HttpParser::Status::Complete)
        {
            const HttpRequest request = parser.Take();
            HttpResponse response;
            try
            {
                response = handler_(request);
            }
            catch (const std::exception &e)
            {
                response = HttpResponse::OfJson(500, Json{{"error", std::string("the bench failed: ") + e.what()}});
            }
            const bool keepAlive = request.keepAlive && running_;
            if (!SendAll(connection->socket, Serialize(response, keepAlive), running_) || !keepAlive)
            {
                open = false;
                break;
            }
            status = parser.Parse();
        }
        if (status == HttpParser::Status::Error)
        {
            SendAll(connection->socket, Serialize(parser.ErrorResponse(), false), running_);
            break;
        }
    }
    shutdown(connection->socket, SD_BOTH);
    closesocket(connection->socket);
}

void HttpServer::Reap()
{
    const std::lock_guard lock(mutex_);
    std::erase_if(connections_, [](const std::shared_ptr<Connection> &connection) {
        if (!connection->done)
        {
            return false;
        }
        connection->thread.join();
        return true;
    });
}

void HttpServer::Stop()
{
    if (running_.exchange(false))
    {
        acceptor_.join(); // it polls, and sees running_ false within kPollMs
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = kNoSocket;
        std::vector<std::shared_ptr<Connection>> connections;
        {
            const std::lock_guard lock(mutex_);
            connections.swap(connections_);
        }
        // Each connection sees running_ false within kPollMs and closes its
        // own socket; a handler still running is waited for.
        for (const auto &connection : connections)
        {
            connection->thread.join();
        }
    }
    if (winsock_)
    {
        WSACleanup();
        winsock_ = false;
    }
}

} // namespace eluvian
