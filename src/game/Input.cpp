// `input`: one of the game's own messages, sent by its text.
#include "game/Shared.h"

namespace eluvian::game
{

// The game's own messages by their text, as a movie's button sends its
// callback and a key binding names what it does: the general way in at the
// input layer, where no tool fits. DAOLib's dao::messages::SendText
// (bf85cb1).
void AddInput(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "input",
            "Send the game one of its own messages by its text, \"Receiver.Message arguments\": what the "
            "interface's buttons send and key bindings name (\"GUIPopupLayer.ScriptDialogResult 1 1 -1\", "
            "\"CClientExoApp.GUIDialogResult 1 0\"). inspect kind=popup shows what each button of a box "
            "sends. The general way in when no tool fits, as `script` is for script commands; the game "
            "does whatever the message means, so know what it is. Answers `sent`: false means the engine "
            "knows no such receiver or message.",
            Json{{"type", "object"},
                 {"properties", {{"message", {{"type", "string"}, {"description", "Receiver.Message arguments"}}}}},
                 {"required", {"message"}}},
            Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "input");
            if (watch.Mode() < 0)
            {
                throw ToolError("input: the game is still starting; health says when it runs");
            }
            const auto message = args.String("message");
            const auto cursor = bench.Events().Next();
            const bool sent = dao::messages::SendText(Wide(message));
            return Json{{"message", message}, {"sent", sent}, {"cursor", cursor}};
        });
}

} // namespace eluvian::game
