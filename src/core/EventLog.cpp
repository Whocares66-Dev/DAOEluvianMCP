#include "core/EventLog.h"

#include <string>

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

} // namespace

EventLog::EventLog(const std::filesystem::path &path, std::size_t most)
    : out_(path, std::ios::binary | std::ios::trunc), most_(most)
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
