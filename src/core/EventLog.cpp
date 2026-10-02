#include "core/EventLog.h"

#include <string>

namespace eluvian
{

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
    std::string line = event.ToJson().dump(-1, ' ', false, Json::error_handler_t::replace);
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
