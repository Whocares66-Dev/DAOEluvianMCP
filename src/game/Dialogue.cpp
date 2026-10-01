// `dialogue`: the conversation on the screen answered, and a cutscene
// skipped. inspect kind=dialogue (Inspect.cpp) reads it.
#include "game/Shared.h"

namespace eluvian::game
{

// Conversations and cutscenes stand between a test and the world it means
// to act in: tonight's first test with no computer use could not leave the
// clearing it loaded into (2026-10-01). DAOLib's dao::dialogue sends what
// the screen's own clicks and keys send (3d6c2ea).
void AddDialogue(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"dialogue",
                 "Answer the conversation on the screen, or skip a cutscene. `action`: choose, the reply at "
                 "`place` among those inspect kind=dialogue lists, as a click on it; continue, as the skip key: "
                 "the line being spoken is cut short and its replies offered, and a reply with no text is gone "
                 "through; skip_cutscene, as Escape in a cutscene or a pre-rendered movie. A loop that works: read "
                 "inspect kind=dialogue; with "
                 "no replies, or a first reply with no text, continue; otherwise choose. Each answers that the "
                 "game was asked, and `cursor`: wait_for eluvian.dialogue.chosen, eluvian.dialogue.line or "
                 "eluvian.mode.changed from it. The game ignores an answer in the moment after replies appear: "
                 "if nothing follows in a second, ask again.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"choose", "continue", "skip_cutscene"}}}},
                        {"place",
                         {{"type", "integer"},
                          {"minimum", 0},
                          {"description", "action=choose: which reply, from 0, as inspect kind=dialogue lists"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "dialogue");
            const Args args(raw, "dialogue");
            const auto action = args.Choice("action", {"choose", "continue", "skip_cutscene"});
            Json answer = {{"action", action}, {"asked", true}, {"cursor", bench.Events().Next()}};
            if (action == "skip_cutscene")
            {
                // The game has two kinds, each with its own mode and its own
                // skip (DAOLib 4757f0e): a cutscene played in the world, and a
                // pre-rendered movie. Esc skips either; so does this.
                if (dao::dialogue::SkipCutscene())
                {
                    answer["skipped"] = "cutscene";
                }
                else if (dao::dialogue::SkipMovie())
                {
                    answer["skipped"] = "movie";
                }
                else
                {
                    throw ToolError("dialogue: no cutscene or movie is playing (health's mode is cutscene or "
                                    "movie while one is)");
                }
                return answer;
            }
            if (dao::dialogue::Current() == nullptr)
            {
                throw ToolError(dao::dialogue::InCutscene()
                                    ? "dialogue: a cutscene is playing, not a conversation: action skip_cutscene"
                                    : "dialogue: no conversation is on the screen (health's mode is dialog "
                                      "while one is)");
            }
            if (action == "continue")
            {
                if (!dao::dialogue::Continue())
                {
                    throw ToolError("dialogue: the game took no continue; inspect kind=dialogue says where "
                                    "the conversation is");
                }
                return answer;
            }
            const auto replies = dao::dialogue::Replies();
            if (replies.empty())
            {
                throw ToolError("dialogue: no replies are offered yet, the other is speaking: action continue "
                                "cuts the line short, or wait_for eluvian.dialogue.replies");
            }
            if (!args.Has("place"))
            {
                throw ToolError(std::format("dialogue: choose needs `place`, 0 to {}; inspect kind=dialogue "
                                            "lists the replies",
                                            replies.size() - 1));
            }
            const auto place =
                static_cast<std::size_t>(args.Integer("place", 0, 0, static_cast<std::int64_t>(replies.size()) - 1));
            const bool female = HeroIsFemale();
            if (replies[place].text == 0xFFFFFFFF)
            {
                throw ToolError("dialogue: that reply has no text, which the game goes through on action "
                                "continue, not choose");
            }
            answer["reply"] = LineJson(replies[place], female);
            if (!dao::dialogue::Choose(place))
            {
                throw ToolError("dialogue: the game took no choice; inspect kind=dialogue says where the "
                                "conversation is");
            }
            return answer;
        });
}

} // namespace eluvian::game
