// Work handed from the listener to the frame thread, and its answer handed
// back: devbench's MainThread::RunAndWait (C:\modding\devbench\src\
// MainThreadTask.h), over dao::OnFrame instead of SKSE's task interface.
//
// The game may be read and driven only on the scripts' thread, which is the
// one dao::OnFrame calls back on (DAOLib's Frame.h). A listener thread posts
// a task and waits; the frame callback drains the queue; the task's value or
// exception comes back to the listener.
//
// A task not started by its deadline is abandoned and never runs: an order
// that arrives after the agent gave up on it would act on a game the agent
// no longer expects. A task already running is waited for to the end, since
// its work cannot be taken back.
//
// Frames stop while an area loads, at a crash, and when the game is closed;
// the deadline is what turns those into an answer.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace eluvian
{

// The frame did not take the task in time.
class FrameTimeout : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class FrameQueue
{
  public:
    // Runs `fn` on the frame thread and returns its value, or rethrows what
    // it threw. Throws FrameTimeout if no frame takes it within `timeout`,
    // or if the queue is closed. Called on the frame thread itself, it runs
    // `fn` at once, since waiting there would wait forever.
    template <class Fn> auto RunAndWait(Fn fn, std::chrono::milliseconds timeout) -> decltype(fn())
    {
        using R = decltype(fn());
        if (OnFrameThread())
        {
            return fn();
        }
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        auto task = std::make_shared<Task>();
        // Shared, so the function holds two pointers and moving it cannot
        // throw, whatever `fn` captured.
        auto shared = std::make_shared<Fn>(std::move(fn));
        task->run = [promise, shared] {
            try
            {
                if constexpr (std::is_void_v<R>)
                {
                    (*shared)();
                    promise->set_value();
                }
                else
                {
                    promise->set_value((*shared)());
                }
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
        };
        task->deadline = std::chrono::steady_clock::now() + timeout;
        if (!Post(task))
        {
            throw FrameTimeout("the bench is shutting down");
        }
        if (future.wait_for(timeout) == std::future_status::ready)
        {
            return future.get();
        }
        if (Abandon(*task))
        {
            throw FrameTimeout("no frame took the call within " + std::to_string(timeout.count()) + " ms");
        }
        return future.get(); // running now: wait for it to end
    }

    // On the frame thread: runs every task queued, in order, and abandons
    // those past their deadline. Returns how many ran.
    std::size_t Drain();

    // Tasks waiting for a frame.
    [[nodiscard]] std::size_t Pending() const;

    // Abandons what is queued and refuses what comes: the plugin is going.
    void Close();

  private:
    enum class State : std::uint8_t
    {
        Queued,
        Running,
        Abandoned,
    };
    struct Task
    {
        std::function<void()> run;
        std::chrono::steady_clock::time_point deadline;
        std::atomic<State> state{State::Queued};
    };

    [[nodiscard]] bool OnFrameThread() const;
    bool Post(const std::shared_ptr<Task> &task); // false once closed
    static bool Abandon(Task &task);

    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Task>> tasks_;
    std::atomic<std::thread::id> frameThread_{};
    bool closed_ = false;
};

} // namespace eluvian
