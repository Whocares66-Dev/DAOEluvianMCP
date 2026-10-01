#include "core/FrameQueue.h"

namespace eluvian
{

bool FrameQueue::OnFrameThread() const
{
    return frameThread_.load() == std::this_thread::get_id();
}

bool FrameQueue::Post(const std::shared_ptr<Task> &task)
{
    const std::lock_guard lock(mutex_);
    if (closed_)
    {
        return false;
    }
    tasks_.push_back(task);
    return true;
}

bool FrameQueue::Abandon(Task &task)
{
    auto expected = State::Queued;
    return task.state.compare_exchange_strong(expected, State::Abandoned) || expected == State::Abandoned;
}

std::size_t FrameQueue::Drain()
{
    frameThread_ = std::this_thread::get_id();
    std::deque<std::shared_ptr<Task>> tasks;
    {
        const std::lock_guard lock(mutex_);
        tasks.swap(tasks_);
    }
    std::size_t ran = 0;
    const auto now = std::chrono::steady_clock::now();
    for (const auto &task : tasks)
    {
        if (now >= task->deadline)
        {
            Abandon(*task);
            continue;
        }
        auto expected = State::Queued;
        if (task->state.compare_exchange_strong(expected, State::Running))
        {
            task->run();
            ++ran;
        }
    }
    return ran;
}

std::size_t FrameQueue::Pending() const
{
    const std::lock_guard lock(mutex_);
    return tasks_.size();
}

void FrameQueue::Close()
{
    std::deque<std::shared_ptr<Task>> tasks;
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        tasks.swap(tasks_);
    }
    for (const auto &task : tasks)
    {
        Abandon(*task);
    }
}

} // namespace eluvian
