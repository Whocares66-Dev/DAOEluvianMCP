// Every event of a run, kept in a file: one JSON object a line, as
// `events` gives them (seq, frame, ms, topic, payload). The bus's buffer
// holds the last few thousand and is gone with the game; a balance run of
// many fights, or a game that crashed, is read from here afterwards
// (turn-based-agent's runs, 2026-10-02; the bridge's game_events reads it
// while no game runs). Written anew by each game, beside the logs; the run
// before's is kept as <name>.previous.jsonl (#74).
#pragma once

#include "core/EventBus.h"

#include <cstddef>
#include <filesystem>
#include <fstream>

namespace eluvian
{

class EventLog
{
  public:
    // Opens `path` empty, the file there before moved to PreviousOf(path).
    // Stops writing past `most` bytes, with a last line saying so, so a run
    // that never ends does not fill the disk.
    explicit EventLog(const std::filesystem::path &path, std::size_t most = 256U << 20);

    // eluvian.events.jsonl's is eluvian.events.previous.jsonl.
    [[nodiscard]] static std::filesystem::path PreviousOf(const std::filesystem::path &path);

    [[nodiscard]] bool Open() const
    {
        return out_.is_open();
    }
    // One event, a line, written through at once: a crash keeps what came before it.
    void Write(const Event &event);

  private:
    std::ofstream out_;
    std::size_t written_ = 0;
    std::size_t most_;
    bool full_ = false;
};

} // namespace eluvian
