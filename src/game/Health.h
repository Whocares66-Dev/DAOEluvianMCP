// Whether the game is alive, answered without waiting on its frame: the one
// question that must still be answerable when the frame has stopped (an
// area loading, a modal dialog, a hang, a crash on the way).
//
// The frame callback records what it sees each frame (Watch::OnFrame); the
// `health` tool reads that from the listener's thread. It also turns what
// changed between frames into events: the game mode (eluvian.mode.changed)
// and the pause (eluvian.paused).
#pragma once

#include "core/Bench.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eluvian::game
{

std::string ModeName(std::int32_t mode);

class Watch
{
  public:
    explicit Watch(Bench &bench);

    // On the frame thread, first thing each frame.
    void OnFrame();

    [[nodiscard]] std::uint64_t Frames() const
    {
        return frames_.load();
    }
    [[nodiscard]] std::int32_t Mode() const
    {
        return mode_.load();
    }

  private:
    friend void AddHealthTool(Bench &bench, const Watch &watch, std::function<Json()> extra);

    Bench &bench_;
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::int64_t> lastFrameMs_{-1}; // since start_
    std::atomic<std::int32_t> mode_{-1};        // GameMode, -1 before the application is up
    std::atomic<std::int32_t> paused_{-1};      // 0 or 1, -1 unknown
};

// `health`. `extra` adds what the plugin knows (port, plugins registered).
void AddHealthTool(Bench &bench, const Watch &watch, std::function<Json()> extra);

} // namespace eluvian::game
