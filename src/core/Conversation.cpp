#include "core/Conversation.h"

#include "core/Args.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// The game ignores an answer in the moment after its replies appear (seen
// 2026-10-01): a choice with no eluvian.dialogue.chosen after this is asked
// again, this many times.
constexpr milliseconds kIgnored{1000};
constexpr int kMostTries = 3;
// With `skipLines`, a line spoken and nothing after it for this long is cut
// short, as the skip key cuts it.
constexpr milliseconds kQuiet{800};

// The indices of a list of replies, to tell the replies answered from new.
std::vector<int> Indices(const Json &replies)
{
    std::vector<int> indices;
    if (replies.is_array())
    {
        for (const auto &reply : replies)
        {
            indices.push_back(reply.value("index", -1));
        }
    }
    return indices;
}

// Whether replies are a real choice: a first reply with no text is the
// game's "continue", which the skip key goes through.
bool RealChoice(const Json &replies)
{
    return replies.is_array() && !replies.empty() && !replies.at(0).value("text", std::string()).empty();
}

// One conversation run on: its arguments, its clock, and the answer.
class Talk
{
  public:
    Talk(Bench &bench, const Args &args)
        : bench_(bench), action_(args.Choice("action", {"choose", "continue", "skip_cutscene"})),
          untilChoice_(args.Choice("until", {"step", "choice"}, "step") == "choice"),
          skipLines_(args.Bool("skipLines", false)),
          deadline_(Clock::now() + milliseconds(args.Integer("budgetMs", 60000, 1000, 300000)))
    {
        step_ = Json{{"action", action_}};
        if (args.Has("place"))
        {
            step_["place"] = args.Raw().at("place");
        }
    }

    Json Run()
    {
        if (!untilChoice_)
        {
            return bench_.Invoke("dialogue.step", step_).data;
        }
        // The replies a choice answers: the same ones read after it are not
        // the next choice. Not a continue's: replies already up before one
        // are the choice it runs on to (seen in play, 2026-10-03, the
        // Quartermaster's: the call waited out its budget on them).
        if (action_ == "choose")
        {
            const Json before = bench_.Invoke("inspect", Json{{"kind", "dialogue"}}).data;
            answered_ = Indices(before.value("replies", Json::array()));
        }
        cursor_ = bench_.Events().Next();
        answer_["cursor"] = cursor_;
        answer_["did"] = bench_.Invoke("dialogue.step", step_).data;
        if (action_ == "choose" && !Chosen())
        {
            return End(false, "the game took no choice, asked " + std::to_string(kMostTries) +
                                  " times; inspect kind=dialogue says where the conversation is");
        }
        return Follow();
    }

  private:
    // Waits for the choice's eluvian.dialogue.chosen, asking again if the
    // game ignored it.
    bool Chosen()
    {
        const auto chosen = EventFilter::FromArgs(Json{{"topic", "eluvian.dialogue.chosen"}});
        for (int tries = 1; tries <= kMostTries; ++tries)
        {
            if (bench_.Events().WaitFor(cursor_, chosen, std::min(Left(), kIgnored)))
            {
                return true;
            }
            if (Left().count() == 0 || tries == kMostTries || !Ask("dialogue.step", step_))
            {
                return false;
            }
        }
        return false;
    }

    // On through the lines: each kept, a reply with no text gone through,
    // until a real choice, the conversation's end, or the time.
    Json Follow()
    {
        const auto filter = EventFilter::FromArgs(
            Json{{"topic", {"eluvian.dialogue.line", "eluvian.dialogue.replies", "eluvian.mode.changed"}}});
        auto quietSince = Clock::now();
        while (Left().count() > 0)
        {
            if (const auto event = bench_.Events().WaitFor(cursor_, filter, std::min(Left(), milliseconds(250))))
            {
                cursor_ = event->seq + 1;
                quietSince = Clock::now();
                if (auto end = OnEvent(*event))
                {
                    return std::move(*end);
                }
                continue;
            }
            // Nothing in a quarter second: the screen read.
            if (auto end = OnScreen(quietSince))
            {
                return std::move(*end);
            }
        }
        return End(false, "the time ran out before the next choice; inspect kind=dialogue says where it is");
    }

    // An event's part: a line kept, a choice answered or gone through, the
    // conversation's end. Nothing while it goes on.
    std::optional<Json> OnEvent(const Event &event)
    {
        if (event.topic == "eluvian.dialogue.line")
        {
            if (const auto text = event.payload.value("text", std::string()); !text.empty())
            {
                lines_.push_back(Json{{"speaker", event.payload.value("speaker", std::string())},
                                      {"by", event.payload.value("by", Json())},
                                      {"to", event.payload.value("to", Json())},
                                      {"text", text}});
            }
            return std::nullopt;
        }
        if (event.topic == "eluvian.dialogue.replies")
        {
            const Json replies = event.payload.value("replies", Json::array());
            if (RealChoice(replies))
            {
                return Choice(replies);
            }
            // No replies is the old ones taken away, not a continue to
            // go through: one gone through there, as the Quartermaster's
            // talk ended into his Store, was refused (2026-10-03).
            if (!replies.empty() && !GoOn())
            {
                return Stopped();
            }
            return std::nullopt;
        }
        const std::string to = event.payload.value("to", std::string());
        if (to != "dialog")
        {
            return Ended(to);
        }
        return std::nullopt;
    }

    // The screen's part, read when no event came: its replies answered or
    // gone through, a spoken line cut short, the conversation's end.
    // `quietSince` starts again when it goes on.
    std::optional<Json> OnScreen(Clock::time_point &quietSince)
    {
        const auto read = Ask("inspect", Json{{"kind", "dialogue"}});
        if (!read)
        {
            return End(false, refused_);
        }
        if (!read->value("active", false))
        {
            return Ended(read->value("cutscene", false) ? "cutscene" : Mode());
        }
        const Json replies = read->value("replies", Json::array());
        if (!replies.empty() && Indices(replies) != answered_)
        {
            if (RealChoice(replies))
            {
                return Choice(replies);
            }
            if (!GoOn())
            {
                return Stopped();
            }
            quietSince = Clock::now();
            return std::nullopt;
        }
        if (skipLines_ && replies.empty() && Clock::now() - quietSince > kQuiet)
        {
            // A line being spoken: cut short, as the skip key does.
            if (!GoOn())
            {
                return Stopped();
            }
            quietSince = Clock::now();
        }
        return std::nullopt;
    }

    bool GoOn()
    {
        return Ask("dialogue.step", Json{{"action", "continue"}}).has_value();
    }

    // A continue refused: the conversation may have ended meanwhile (into a
    // store, a cutscene), which is the run's end, not a failure.
    Json Stopped()
    {
        const auto read = Ask("inspect", Json{{"kind", "dialogue"}});
        if (read && !read->value("active", true))
        {
            return Ended(read->value("cutscene", false) ? "cutscene" : Mode());
        }
        return End(false, refused_);
    }

    Json Choice(const Json &replies)
    {
        answer_["replies"] = replies;
        return End(true, "the next choice is up: choose a reply's `place`");
    }

    Json Ended(const std::string &mode)
    {
        answer_["ended"] = true;
        answer_["mode"] = mode;
        if (mode == "cutscene")
        {
            return End(true, "the conversation went on into a cutscene: dialogue action=skip_cutscene skips it");
        }
        return End(true, "the conversation ended; the game is in mode " + mode);
    }

    std::string Mode()
    {
        const auto health = Ask("health", Json::object());
        return health ? health->value("mode", std::string()) : std::string();
    }

    // A call refused for the game's state once the conversation is under
    // way: the run ends with the refusal said, as travel's journeys do.
    std::optional<Json> Ask(std::string_view tool, const Json &args)
    {
        try
        {
            return bench_.Invoke(tool, args).data;
        }
        catch (const ToolError &e)
        {
            refused_ = e.what();
        }
        catch (const FrameTimeout &e)
        {
            refused_ = std::string("no answer from ") + std::string(tool) + ": " + e.what();
        }
        return std::nullopt;
    }

    [[nodiscard]] milliseconds Left() const
    {
        return std::max(milliseconds(0), std::chrono::duration_cast<milliseconds>(deadline_ - Clock::now()));
    }

    Json End(bool done, std::string reason)
    {
        answer_["done"] = done;
        answer_["reason"] = std::move(reason);
        answer_["lines"] = lines_;
        if (!answer_.contains("replies"))
        {
            answer_["replies"] = nullptr;
        }
        return answer_;
    }

    Bench &bench_;
    std::string action_;
    bool untilChoice_;
    bool skipLines_;
    Clock::time_point deadline_;
    Json step_;
    std::uint64_t cursor_ = 0;
    std::vector<int> answered_;
    std::string refused_;
    Json lines_ = Json::array();
    Json answer_ = Json::object();
};

} // namespace

void AddDialogueTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{"dialogue",
                 "Answer the conversation on the screen, or skip a cutscene. `action`: choose, the reply at "
                 "`place` among those inspect kind=dialogue lists, as a click on it; continue, as the skip key: "
                 "the line being spoken is cut short and its replies offered, and a reply with no text is gone "
                 "through; skip_cutscene, as Escape in a cutscene or a pre-rendered movie. With `until`: "
                 "\"choice\", it goes on from there to the next real choice in one call: an answer the game "
                 "ignored is asked again, a reply with no text gone through, and the lines play as they would "
                 "for a player (`skipLines`: true cuts each short); it answers every line spoken on the way "
                 "(`lines`: speaker as the file writes it, `by` and `to` the creatures, and text), the `replies` of "
                 "the next choice, or that it `ended` and the "
                 "`mode` after (explore, a cutscene, a fight, an area loading). Without `until` it answers that "
                 "the game was asked, and `cursor`: wait_for eluvian.dialogue.chosen, .line or "
                 "eluvian.mode.changed from it. `budgetMs` (default 60000) bounds an `until`.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"choose", "continue", "skip_cutscene"}}}},
                        {"place",
                         {{"type", "integer"},
                          {"minimum", 0},
                          {"description", "action=choose: which reply, from 0, as inspect kind=dialogue lists"}}},
                        {"until",
                         {{"type", "string"},
                          {"enum", {"step", "choice"}},
                          {"description", "choice: go on to the next real choice or the conversation's end"}}},
                        {"skipLines",
                         {{"type", "boolean"}, {"description", "with until: cut each line short; default false"}}},
                        {"budgetMs", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 300000}}}}},
                      {"required", {"action"}}},
                 Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "dialogue");
            return Talk(bench, args).Run();
        });
}

} // namespace eluvian
