// The bench's listener: Winsock on 127.0.0.1, a thread per connection, the
// bytes handed to core/Http.h's parser and its answers sent back. Nothing
// here reads the game; a handler that wants it goes through the frame queue.
//
// 127.0.0.1 only, and with SO_EXCLUSIVEADDRUSE, so no other process can
// take the port from under it. A port in use (a second game running, which
// on 2026-09-30 truncated the first's logs) moves to the next of a few.
#pragma once

#include "core/Http.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace eluvian
{

class HttpServer
{
  public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    static constexpr int kMaxConnections = 32;

    explicit HttpServer(Handler handler);
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // Binds 127.0.0.1 on the first free port from `first`, trying `tries`
    // ports, and starts accepting. The port bound, or 0 if none was free
    // (or Winsock would not start). Port 0 asks the system for any.
    std::uint16_t Start(std::uint16_t first, int tries);

    // Stops accepting, closes every connection, and joins every thread. A
    // handler still running is waited for.
    void Stop();

  private:
    struct Connection;

    void Accept();
    void Serve(const std::shared_ptr<Connection> &connection);
    void Reap();

    Handler handler_;
    std::uintptr_t listener_;
    std::atomic<bool> running_{false};
    bool winsock_ = false;
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
};

} // namespace eluvian
