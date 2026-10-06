// Every event of a run, kept in a file: one JSON object a line, as
// `events` gives them (seq, frame, ms, topic, payload). The bus's buffer
// holds the last few thousand and is gone with the game; a balance run of
// many fights, or a game that crashed, is read from here afterwards
// (turn-based-agent's runs, 2026-10-02; the bridge's game_events reads it
// while no game runs). Written anew by each game, beside the logs; the run
// before's is kept as <name>.previous.jsonl (#74).
//
// Written by a thread of its own: the bus hands each event over as it is
// emitted, in its order, and the writer serializes, writes and flushes
// them in batches. Until 2026-10-05 the emitting thread (most often the
// frame thread) did that under the bus's lock (#73). A crash loses only
// the events of its last moment, the writer being woken by each.
#pragma once

#include "core/EventBus.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace eluvian
{

class EventLog
{
  public:
    // Opens `path` empty, the file there before moved to PreviousOf(path).
    // Stops writing past `most` bytes, with a last line saying so, so a run
    // that never ends does not fill the disk.
    // `beforeBatch`, the tests' only: called on the writer before it writes
    // each batch, to hold it there.
    explicit EventLog(const std::filesystem::path &path, std::size_t most = 256U << 20,
                      std::function<void()> beforeBatch = {});
    // Writes what is queued, then stops the writer.
    ~EventLog();
    EventLog(const EventLog &) = delete;
    EventLog &operator=(const EventLog &) = delete;

    // eluvian.events.jsonl's is eluvian.events.previous.jsonl.
    [[nodiscard]] static std::filesystem::path PreviousOf(const std::filesystem::path &path);

    [[nodiscard]] bool Open() const
    {
        return open_;
    }
    // One event, queued for the writer: quick, and safe under the bus's lock.
    void Write(std::shared_ptr<const Event> event);
    // Waits until every event given so far is in the file.
    void Flush();

  private:
    void Run();
    void WriteNow(const Event &event); // the writer's: a line

    std::ofstream out_;
    const bool open_;
    std::size_t written_ = 0;
    std::size_t most_;
    bool full_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;    // the writer's: an event queued, or the end
    std::condition_variable drained_; // Flush's: a batch written
    std::deque<std::shared_ptr<const Event>> queue_;
    bool writing_ = false;
    bool stopping_ = false;
    std::function<void()> beforeBatch_;
    std::thread writer_; // last, started once the rest is made
};

} // namespace eluvian
