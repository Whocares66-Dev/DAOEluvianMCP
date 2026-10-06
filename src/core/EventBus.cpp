#include "core/EventBus.h"

#include <algorithm>
#include <utility>

namespace eluvian
{

namespace
{

// Every field of `want` is in `have` and equal; objects compare as subsets
// all the way down, so {"creature": {"id": "0x7d9"}} matches a payload whose
// creature has more fields than that.
bool Subset(const Json &want, const Json &have)
{
    if (!want.is_object())
    {
        return want == have;
    }
    if (!have.is_object())
    {
        return false;
    }
    return std::ranges::all_of(want.items(), [&have](const auto &field) {
        const auto it = have.find(field.key());
        return it != have.end() && Subset(field.value(), *it);
    });
}

bool TopicMatches(std::string_view pattern, std::string_view topic)
{
    if (pattern == "*")
    {
        return true;
    }
    if (pattern.ends_with(".*"))
    {
        const auto prefix = pattern.substr(0, pattern.size() - 1); // keeps the dot
        return topic.starts_with(prefix);
    }
    return pattern == topic;
}

} // namespace

Json Event::ToJson() const
{
    return Json{{"seq", seq}, {"frame", frame}, {"ms", ms}, {"topic", topic}, {"payload", payload}};
}

bool EventFilter::AcceptsTopic(std::string_view topic) const
{
    return topics.empty() ||
           std::ranges::any_of(topics, [topic](const std::string &pattern) { return TopicMatches(pattern, topic); });
}

bool EventFilter::Accepts(const Event &event) const
{
    return AcceptsTopic(event.topic) && (match.empty() || Subset(match, event.payload));
}

bool EventCatalog::Describe(TopicSpec spec)
{
    const std::lock_guard lock(mutex_);
    const bool added = !topics_.contains(spec.topic);
    std::string topic = spec.topic;
    topics_.insert_or_assign(std::move(topic), std::move(spec));
    return added;
}

Json EventCatalog::List(const EventFilter &filter) const
{
    const std::lock_guard lock(mutex_);
    Json list = Json::array();
    for (const auto &[topic, spec] : topics_)
    {
        if (filter.AcceptsTopic(topic))
        {
            list.push_back(Json{
                {"topic", topic}, {"description", spec.description}, {"fields", spec.fields}, {"from", spec.from}});
        }
    }
    return list;
}

EventFilter EventFilter::FromArgs(const Json &args)
{
    EventFilter filter;
    if (const auto topic = args.find("topic"); topic != args.end() && !topic->is_null())
    {
        if (topic->is_string())
        {
            filter.topics.push_back(topic->get<std::string>());
        }
        else if (topic->is_array() && std::ranges::all_of(*topic, [](const Json &each) { return each.is_string(); }))
        {
            for (const auto &each : *topic)
            {
                filter.topics.push_back(each.get<std::string>());
            }
        }
        else
        {
            throw ToolError("`topic` is a topic name, a prefix like \"mymod.*\", or an array of them");
        }
    }
    if (const auto match = args.find("match"); match != args.end() && !match->is_null())
    {
        if (!match->is_object())
        {
            throw ToolError(R"(`match` is an object of payload fields to equal, like {"id": "0x7d9"})");
        }
        filter.match = *match;
    }
    return filter;
}

EventBus::EventBus(std::size_t capacity, FrameSource frame)
    : capacity_(std::max<std::size_t>(capacity, 1)), frame_(std::move(frame))
{
}

std::uint64_t EventBus::Emit(std::string topic, Json payload)
{
    Event event;
    event.frame = frame_ ? frame_() : 0;
    event.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    event.topic = std::move(topic);
    event.payload = std::move(payload);
    std::uint64_t seq = 0;
    {
        const std::lock_guard lock(mutex_);
        seq = next_++;
        event.seq = seq;
        if (recorder_)
        {
            recorder_(event); // under the lock, so the record keeps the bus's order
        }
        events_.push_back(std::move(event));
        if (events_.size() > capacity_)
        {
            events_.pop_front();
        }
    }
    changed_.notify_all();
    return seq;
}

void EventBus::SetRecorder(Recorder recorder)
{
    const std::lock_guard lock(mutex_);
    recorder_ = std::move(recorder);
}

std::uint64_t EventBus::Next() const
{
    const std::lock_guard lock(mutex_);
    return next_;
}

EventBus::Page EventBus::Since(std::uint64_t cursor, const EventFilter &filter, std::size_t limit) const
{
    const std::lock_guard lock(mutex_);
    Page page;
    page.next = std::max(cursor, events_.empty() ? next_ : events_.front().seq);
    if (!events_.empty() && cursor != 0 && cursor < events_.front().seq)
    {
        page.missed = events_.front().seq - cursor;
    }
    for (const Event &event : events_)
    {
        if (event.seq < cursor)
        {
            continue;
        }
        if (filter.Accepts(event))
        {
            if (page.events.size() == limit)
            {
                page.truncated = true;
                break;
            }
            page.events.push_back(event);
        }
        page.next = event.seq + 1;
    }
    if (!page.truncated)
    {
        page.next = std::max(page.next, next_);
    }
    return page;
}

std::optional<Event> EventBus::FirstMatch(std::uint64_t cursor, const EventFilter &filter) const
{
    for (const Event &event : events_)
    {
        if (event.seq >= cursor && filter.Accepts(event))
        {
            return event;
        }
    }
    return std::nullopt;
}

std::optional<Event> EventBus::WaitFor(std::uint64_t cursor, const EventFilter &filter,
                                       std::chrono::milliseconds timeout, const std::atomic<bool> *keepWaiting) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    while (true)
    {
        if (auto found = FirstMatch(cursor, filter))
        {
            return found;
        }
        if (closed_ || (keepWaiting != nullptr && !keepWaiting->load()))
        {
            return std::nullopt;
        }
        // Everything up to here has been looked at: only newer events need it.
        cursor = std::max(cursor, next_);
        // Woken by an event or Close; and every 100 ms, to see keepWaiting.
        const auto wake = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
        if (changed_.wait_until(lock, wake) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
        {
            return FirstMatch(cursor, filter);
        }
    }
}

void EventBus::Close()
{
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

} // namespace eluvian
