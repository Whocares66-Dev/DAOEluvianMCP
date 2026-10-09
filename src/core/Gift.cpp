#include "core/Gift.h"

#include "core/Args.h"

#include <string>
#include <thread>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// The state read back, or nothing if the game refused the read (an area
// loading): asked again.
std::optional<Json> State(Bench &bench, const Json &which)
{
    try
    {
        return bench.Invoke("gift.state", which).data;
    }
    catch (const ToolError &)
    {
        return std::nullopt;
    }
    catch (const FrameTimeout &)
    {
        return std::nullopt;
    }
}

Json Give(Bench &bench, const Json &raw, milliseconds poll)
{
    const Args args(raw, "gift");
    const auto budget = milliseconds(args.Integer("budgetMs", 3000, 100, 30000));
    const Json which = {{"to", args.String("to")}, {"item", args.String("item")}};
    const auto started = Clock::now();
    Json answer = bench.Invoke("gift.give", which).data; // its refusal is the tool's
    const Json before = answer.value("approvalBefore", Json());
    bool taken = false;
    Json state = Json::object();
    while (!taken)
    {
        if (const auto read = State(bench, which))
        {
            state = *read;
            taken = !state.value("held", true) || state.value("approval", Json()) != before;
        }
        if (taken || Clock::now() - started >= budget)
        {
            break;
        }
        std::this_thread::sleep_for(poll);
    }
    answer["approvalAfter"] = state.value("approval", before);
    answer["given"] = !state.value("held", true);
    answer["waitedMs"] = std::chrono::duration_cast<milliseconds>(Clock::now() - started).count();
    const std::string mode = bench.Invoke("health", Json::object()).data.value("mode", std::string());
    answer["mode"] = mode;
    if (!taken)
    {
        answer["note"] = "the game had not taken the gift within budgetMs: the item is still in the pack and the "
                         "approval as it was. inspect kind=character (follower.approval) and kind=inventory read "
                         "them again";
    }
    else if (mode == "dialog")
    {
        answer["note"] = "the gift started a conversation: dialogue answers it";
    }
    return answer;
}

} // namespace

void AddGiftTool(Bench &bench, milliseconds poll)
{
    bench.Tools().Add(
        ToolSpec{"gift",
                 "Give a follower a gift from the pack, as the inventory's Give does: the game's own gift event, "
                 "so its scripts change the approval and move the item, and a special gift may start a "
                 "conversation. `to`, a follower in the party; `item`, an item of base type Gift from inspect "
                 "kind=inventory. Answers once the game has taken it (the item gone from the pack, or the "
                 "approval moved; `budgetMs`, default 3000, at most): the approval before and after "
                 "(`approvalBefore`, `approvalAfter`), whether the item left the pack (`given`), the game's "
                 "`mode` after (dialog: a conversation began), and `cursor` for the events that followed.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"to", {{"type", "string"}, {"description", "The follower's id"}}},
                        {"item", {{"type", "string"}, {"description", "The gift's id"}}},
                        {"budgetMs", {{"type", "integer"}, {"minimum", 100}, {"maximum", 30000}}}}},
                      {"required", {"to", "item"}}},
                 Thread::Listener},
        [&bench, poll](const Json &raw) -> ToolResult { return Give(bench, raw, poll); });
}

} // namespace eluvian
