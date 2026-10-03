// `input`: one of the game's own messages, sent by its text: what an
// interface's button sends, or what a key's binding names.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

// Whether a message's text names a save: the quicksave key's binding is
// "SaveGameMessage -1 2", which writes over the player's quicksave. A test
// writes no save by accident (one test's walk made the game autosave into a
// player's own playthrough, 2026-10-01), so such a text wants saying twice.
bool NamesASave(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("savegame") != std::string::npos;
}

} // namespace

// The game's own messages by their text: the general way in at the input
// layer, where no tool fits. Two registries, two forms (DAOLib bf85cb1 and
// 0632df5): a `message` is "Receiver.Message arguments", what the
// interface's buttons send (dao::messages::SendText); a `binding` is
// "Message arguments", what a key's binding names, sent where a pressed
// key's goes (dao::messages::SendBinding).
void AddInput(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "input",
            "Send the game one of its own messages by its text. `message`: \"Receiver.Message arguments\", what "
            "the interface's buttons send (\"GUIPopupLayer.ScriptDialogResult 1 1 -1\"; inspect kind=gui shows "
            "what each button of a box sends). `binding`: \"Message arguments\", what a key's binding names, "
            "sent as pressing the key sends it (\"UserTogglePauseMessage\", \"SelectPartyMemberIndexMessage 0 "
            "1\"). The general way in when no tool fits, as `script` is for script commands; the game does "
            "whatever the message means, so know what it is. One that names a save (the quicksave key's "
            "\"SaveGameMessage -1 2\" writes over the player's quicksave) is refused unless `confirmSave` is "
            "true. Answers `sent`: false means the engine knows no such receiver or message.",
            Json{{"type", "object"},
                 {"properties",
                  {{"message", {{"type", "string"}, {"description", "Receiver.Message arguments"}}},
                   {"binding", {{"type", "string"}, {"description", "Message arguments, as a key's binding has it"}}},
                   {"confirmSave",
                    {{"type", "boolean"}, {"description", "Send a text that names a save all the same"}}}}}},
            Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "input");
            if (watch.Mode() < 0)
            {
                throw ToolError("input: the game is still starting; health says when it runs");
            }
            const bool binding = args.Has("binding");
            if (binding == args.Has("message"))
            {
                throw ToolError("input: give one of `message` (\"Receiver.Message arguments\", a button's) or "
                                "`binding` (\"Message arguments\", a key's)");
            }
            const auto text = args.String(binding ? "binding" : "message");
            if (NamesASave(text) && !args.Bool("confirmSave", false))
            {
                throw ToolError("input: that text names a save, and the quicksave key's writes over the "
                                "player's quicksave. game action=save makes a new manual save that writes over "
                                "nothing; to send this all the same, pass `confirmSave`: true");
            }
            const auto cursor = bench.Events().Next();
            const bool sent = binding ? dao::messages::SendBinding(Wide(text)) : dao::messages::SendText(Wide(text));
            return Json{{binding ? "binding" : "message", text}, {"sent", sent}, {"cursor", cursor}};
        });
}

} // namespace eluvian::game
