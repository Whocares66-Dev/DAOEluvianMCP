// The bench's events: what happened in the game and in the plugins, in the
// order it happened, each with a sequence number an agent reads from.
//
// The play test of 2026-09-30 waited a guessed 4 to 12 s for each turn to
// end, then read the log; too short, and the next click landed in someone
// else's turn. An agent here waits for the event instead (WaitFor), from a
// cursor it was handed before it acted, so an event that came between its
// order and its wait is not missed.
//
// One ring buffer for every source, so the engine's events and the mods'
// share one order and one clock. An event older than the buffer is gone; a
// read from a cursor before the oldest says how many it missed.
//
// Thread-safe: the frame thread and plugins emit, listener threads read and
// wait.
#pragma once

#include "core/Tool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eluvian
{

struct Event
{
    std::uint64_t seq = 0;
    std::uint64_t frame = 0; // the game's frame count when it was emitted
    double ms = 0;           // real milliseconds since the bus was made
    std::string topic;
    Json payload;

    [[nodiscard]] Json ToJson() const;
};

// Which events a read wants. A topic is a name ("eluvian.command.ended"),
// a prefix ending in ".*" ("mymod.*"), or "*"; none means all. Every field
// of `match` must equal the payload's field of that name (a nested object
// matches as a subset too).
struct EventFilter
{
    std::vector<std::string> topics;
    Json match = Json::object();

    [[nodiscard]] bool Accepts(const Event &event) const;
    // From a tool's arguments: `topic` (a string or an array) and `match`.
    // Throws ToolError for a wrong type.
    static EventFilter FromArgs(const Json &args);
};

class EventBus
{
  public:
    using FrameSource = std::function<std::uint64_t()>;

    explicit EventBus(std::size_t capacity = 4096, FrameSource frame = {});

    // Records an event; returns its sequence number. Wakes every waiter.
    std::uint64_t Emit(std::string topic, Json payload);

    // The sequence number the next event will have: a cursor for "from now".
    [[nodiscard]] std::uint64_t Next() const;

    struct Page
    {
        std::vector<Event> events;
        std::uint64_t next = 0;   // the cursor to read from next time
        std::uint64_t missed = 0; // events at or after the cursor already gone from the buffer
        bool truncated = false;   // more matching events than `limit`
    };
    // The events from `cursor` on that pass the filter, at most `limit`.
    [[nodiscard]] Page Since(std::uint64_t cursor, const EventFilter &filter, std::size_t limit) const;

    // The first event from `cursor` on that passes the filter, waiting up
    // to `timeout` for it; nullopt on the timeout, on Close, or when
    // `keepWaiting` turns false.
    [[nodiscard]] std::optional<Event> WaitFor(std::uint64_t cursor, const EventFilter &filter,
                                               std::chrono::milliseconds timeout,
                                               const std::atomic<bool> *keepWaiting = nullptr) const;

    // Wakes every waiter for good: the plugin is going away.
    void Close();

  private:
    [[nodiscard]] std::optional<Event> FirstMatch(std::uint64_t cursor, const EventFilter &filter) const;

    const std::size_t capacity_;
    FrameSource frame_;
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<Event> events_;
    std::uint64_t next_ = 1; // 0 is never a sequence number, so 0 reads "from the oldest"
    bool closed_ = false;
};

} // namespace eluvian
