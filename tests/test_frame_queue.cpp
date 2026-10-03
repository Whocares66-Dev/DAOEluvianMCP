#include "core/FrameQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace eluvian;
using namespace std::chrono_literals;

namespace
{

// A stand-in for dao::OnFrame: drains the queue every few milliseconds on
// its own thread until stopped.
class FrameThread
{
  public:
    explicit FrameThread(FrameQueue &queue)
        : thread_([this, &queue] {
              while (running_)
              {
                  queue.Drain();
                  std::this_thread::sleep_for(2ms);
              }
          })
    {
    }
    ~FrameThread()
    {
        running_ = false;
        thread_.join();
    }
    FrameThread(const FrameThread &) = delete;
    FrameThread &operator=(const FrameThread &) = delete;

  private:
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace

TEST_CASE("a call runs on the frame thread and its value comes back", "[queue]")
{
    FrameQueue queue;
    const FrameThread frames(queue);
    std::thread::id ranOn;
    const int answer = queue.RunAndWait(
        [&ranOn] {
            ranOn = std::this_thread::get_id();
            return 42;
        },
        2s);
    CHECK(answer == 42);
    CHECK(ranOn != std::this_thread::get_id());
}

TEST_CASE("what the call threw is thrown to the caller", "[queue]")
{
    FrameQueue queue;
    const FrameThread frames(queue);
    CHECK_THROWS_AS(queue.RunAndWait([]() -> int { throw std::logic_error("no"); }, 2s), std::logic_error);
}

TEST_CASE("with no frame the call times out and never runs after", "[queue]")
{
    FrameQueue queue;
    std::atomic<bool> ran{false};
    CHECK_THROWS_AS(queue.RunAndWait([&ran] { ran = true; }, 20ms), FrameTimeout);
    CHECK(queue.Drain() == 0); // the late frame finds it abandoned
    CHECK_FALSE(ran);
    CHECK(queue.Pending() == 0);
}

TEST_CASE("a call already running is waited for past the timeout", "[queue]")
{
    FrameQueue queue;
    const FrameThread frames(queue);
    // The frame has a fifth of a second to take it: with 20 ms, one run in
    // three failed with every core busy, as the mutants keep them
    // (tools/mutants.py, 2026-10-03).
    const int answer = queue.RunAndWait(
        [] {
            std::this_thread::sleep_for(600ms);
            return 7;
        },
        200ms);
    CHECK(answer == 7);
}

TEST_CASE("on the frame thread a call runs at once", "[queue]")
{
    FrameQueue queue;
    queue.Drain(); // this thread is now the frame's
    CHECK(queue.RunAndWait([] { return 3; }, 0ms) == 3);
}

TEST_CASE("a closed queue refuses calls at once", "[queue]")
{
    FrameQueue queue;
    queue.Close();
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(queue.RunAndWait([] { return 1; }, 5s), FrameTimeout);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE("a call given up on while the frame runs another is not run after it", "[queue]")
{
    FrameQueue queue;
    std::atomic<bool> ran{false};
    std::atomic<bool> refused{false};
    std::thread first([&queue] {
        (void)queue.RunAndWait(
            [] {
                std::this_thread::sleep_for(1000ms);
                return 1;
            },
            5s);
    });
    while (queue.Pending() < 1)
    {
        std::this_thread::sleep_for(1ms);
    }
    std::thread second([&queue, &ran, &refused] {
        try
        {
            queue.RunAndWait([&ran] { ran = true; }, 150ms);
        }
        catch (const FrameTimeout &)
        {
            refused = true;
        }
    });
    while (queue.Pending() < 2)
    {
        std::this_thread::sleep_for(1ms);
    }
    // This thread is the frame: the first call runs past the second's
    // timeout, and the second, given up on meanwhile, is not run after.
    CHECK(queue.Drain() == 1);
    first.join();
    second.join();
    CHECK(refused);
    CHECK_FALSE(ran);
}

TEST_CASE("a call still queued when the queue closes is refused, not waited for for ever", "[queue]")
{
    FrameQueue queue;
    std::promise<bool> refused;
    auto answer = refused.get_future();
    std::thread caller([&queue, &refused] {
        try
        {
            queue.RunAndWait([] { return 1; }, 500ms);
            refused.set_value(false);
        }
        catch (const FrameTimeout &)
        {
            refused.set_value(true);
        }
        catch (...)
        {
            refused.set_value(false);
        }
    });
    while (queue.Pending() == 0)
    {
        std::this_thread::sleep_for(1ms);
    }
    queue.Close(); // abandons it, before its caller's timeout does
    const bool came = answer.wait_for(5s) == std::future_status::ready;
    if (!came)
    {
        caller.detach(); // waiting for ever: the test fails rather than hangs
    }
    REQUIRE(came);
    CHECK(answer.get());
    caller.join();
}

TEST_CASE("calls from many threads all come back", "[queue]")
{
    FrameQueue queue;
    const FrameThread frames(queue);
    std::atomic<int> sum{0};
    std::vector<std::thread> callers;
    for (int i = 1; i <= 8; ++i)
    {
        callers.emplace_back([&queue, &sum, i] { sum += queue.RunAndWait([i] { return i; }, 5s); });
    }
    for (auto &caller : callers)
    {
        caller.join();
    }
    CHECK(sum == 36);
}

TEST_CASE("a call with no value runs on the frame thread too", "[queue]")
{
    FrameQueue queue;
    const FrameThread frames(queue);
    std::atomic<bool> ran{false};
    queue.RunAndWait([&ran] { ran = true; }, 2s);
    CHECK(ran);
}
