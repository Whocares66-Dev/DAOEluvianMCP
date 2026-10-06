#include "core/EventLog.h"

#include <string>
#include <system_error>
#include <utility>

namespace eluvian
{

namespace
{

// An event's line, as Event::ToJson().dump() gives it, but with the payload
// serialized where it is: ToJson() copied the whole payload only to dump
// it, on the emitting thread (often the frame thread) and under the bus's
// lock, and a hang's sample caught the frame thread in such a copy under
// the debug heap (lib-agent, 2026-10-05, #70).
std::string Line(const Event &event)
{
    const auto dump = [](const Json &value) { return value.dump(-1, ' ', false, Json::error_handler_t::replace); };
    std::string line = "{\"seq\":" + dump(Json(event.seq));
    line += ",\"frame\":" + dump(Json(event.frame));
    line += ",\"ms\":" + dump(Json(event.ms));
    line += ",\"topic\":" + dump(Json(event.topic));
    line += ",\"payload\":" + dump(event.payload);
    line += '}';
    return line;
}

// The run before's file, moved aside before this run's is opened: a crash
// is followed by a launch, and the truncation lost the events wanted most
// (the darkspawn tunnels' travel, turn-based-agent, 2026-10-05, #74). One
// is kept. A file that cannot be moved is written over, as before.
std::filesystem::path MovedAside(const std::filesystem::path &path)
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) > 0 && !error)
    {
        const auto previous = EventLog::PreviousOf(path);
        std::filesystem::remove(previous, error);
        std::filesystem::rename(path, previous, error);
    }
    return path;
}

} // namespace

std::filesystem::path EventLog::PreviousOf(const std::filesystem::path &path)
{
    return path.parent_path() / (path.stem().string() + ".previous" + path.extension().string());
}

EventLog::EventLog(const std::filesystem::path &path, std::size_t most, std::function<void()> beforeBatch)
    : out_(MovedAside(path), std::ios::binary | std::ios::trunc), open_(out_.is_open()), most_(most),
      beforeBatch_(std::move(beforeBatch))
{
    if (open_)
    {
        writer_ = std::thread([this] { Run(); });
    }
}

EventLog::~EventLog()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable())
    {
        writer_.join();
    }
}

void EventLog::Write(std::shared_ptr<const Event> event)
{
    if (!open_ || !event)
    {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(std::move(event));
    }
    wake_.notify_one();
}

void EventLog::Flush()
{
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

void EventLog::Run()
{
    std::unique_lock lock(mutex_);
    while (true)
    {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty())
        {
            return; // stopping, and everything given written
        }
        std::deque<std::shared_ptr<const Event>> batch;
        batch.swap(queue_);
        writing_ = true;
        lock.unlock();
        if (beforeBatch_)
        {
            beforeBatch_();
        }
        for (const auto &event : batch)
        {
            WriteNow(*event);
        }
        // Through to the system once a batch: what a crash after it keeps.
        out_.flush();
        batch.clear();
        lock.lock();
        writing_ = false;
        drained_.notify_all();
    }
}

void EventLog::WriteNow(const Event &event)
{
    if (full_)
    {
        return;
    }
    std::string line = Line(event);
    line += '\n';
    if (written_ + line.size() > most_)
    {
        full_ = true;
        line = Json{{"seq", event.seq},
                    {"topic", "eluvian.events.full"},
                    {"payload", {{"note", "the file reached its most; later events were not kept"}}}}
                   .dump() +
               '\n';
    }
    out_ << line;
    written_ += line.size();
}

} // namespace eluvian
