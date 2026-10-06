#include "core/EventLog.h"

#include <string>
#include <system_error>

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

EventLog::EventLog(const std::filesystem::path &path, std::size_t most)
    : out_(MovedAside(path), std::ios::binary | std::ios::trunc), most_(most)
{
}

void EventLog::Write(const Event &event)
{
    if (!out_.is_open() || full_)
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
    out_.flush();
    written_ += line.size();
}

} // namespace eluvian
