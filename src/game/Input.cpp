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

// A screen's own controls, driven through its movie: a tab, a scroll, what
// no engine message chooses. An agent asked for a picture of the Options
// screen's Controls tab could open the screen but not choose the tab (the
// user, through turn-based-agent, 2026-10-03, #28). Seen in play that day:
// SceneManager.PlayScene("Controls") shows the page and _root.KeyBindings_mc.
// SetScrollPercentage(100) scrolls the list; OptionsMenu.OptionTabAction, the
// name the tab buttons are made with, only returns a button's handler.
// DAOLib's ScreenMovie (lib-agent's #11) finds a shown screen's movie by its
// scene's name.
Json MovieCall(const Args &args)
{
    const std::string screen = args.String("screen");
    auto movie = dao::gui::ScreenMovie(Wide(screen));
    if (!movie)
    {
        throw ToolError("input: the screen \"" + screen +
                        "\" is not shown; inspect kind=gui lists the screens shown, by the names to give here");
    }
    const int forms = (args.Has("call") ? 1 : 0) + (args.Has("set") ? 1 : 0) + (args.Has("get") ? 1 : 0);
    if (forms != 1)
    {
        throw ToolError("input: with `screen`, give one of `call` (an ActionScript function's path, with "
                        "`args`), `set` (a variable's path, with `value`) or `get` (a number's or a bool's path)");
    }
    if (args.Has("get"))
    {
        const std::string path = args.String("get");
        if (const auto number = movie->GetNumber(path.c_str()))
        {
            return Json{{"screen", screen}, {"get", path}, {"value", *number}};
        }
        // A bool is read as a bool, a number as a number; a string or an
        // object is not asked for (GFx keeps a reference to it).
        const auto flag = movie->GetBool(path.c_str());
        return Json{{"screen", screen}, {"get", path}, {"value", flag ? Json(*flag) : Json(nullptr)}};
    }
    // The values' texts must outlive the call.
    std::vector<std::string> texts;
    const auto value = [&texts](const Json &json) {
        if (json.is_boolean())
        {
            return dao::gui::Value::Bool(json.get<bool>());
        }
        if (json.is_number())
        {
            return dao::gui::Value::Number(json.get<double>());
        }
        if (!json.is_string())
        {
            throw ToolError("input: a movie's values are strings, numbers or bools");
        }
        texts.push_back(json.get<std::string>());
        return dao::gui::Value::String(texts.back().c_str());
    };
    if (args.Has("set"))
    {
        const std::string path = args.String("set");
        if (!args.Has("value"))
        {
            throw ToolError("input: `set` wants a `value`");
        }
        texts.reserve(1);
        const bool done = movie->SetVariable(path.c_str(), value(args.Raw().at("value")));
        return Json{{"screen", screen}, {"set", path}, {"done", done}};
    }
    const std::string function = args.String("call");
    const Json list = args.Raw().value("args", Json::array());
    if (!list.is_array())
    {
        throw ToolError("input: `args` is a list of strings, numbers or bools");
    }
    texts.reserve(list.size()); // no reallocation: each Value points into its text
    std::vector<dao::gui::Value> values;
    values.reserve(list.size());
    for (const auto &each : list)
    {
        values.push_back(value(each));
    }
    const bool ran = movie->Invoke(function.c_str(), values);
    return Json{{"screen", screen}, {"call", function}, {"ran", ran}};
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
            "true. Answers `sent`: false means the engine knows no such receiver or message. "
            "`screen` (a screen shown, by its name in inspect kind=gui, \"Options\") drives that screen's own "
            "script, what no message reaches: `call` an ActionScript function by its path with `args` "
            "(\"SceneManager.PlayScene\", [\"Controls\"] shows the Options' Controls page), "
            "`set` a variable to `value`, or "
            "`get` a number's or a bool's value; answers whether it `ran` or was `done`, or the `value`.",
            Json{
                {"type", "object"},
                {"properties",
                 {{"message", {{"type", "string"}, {"description", "Receiver.Message arguments"}}},
                  {"binding", {{"type", "string"}, {"description", "Message arguments, as a key's binding has it"}}},
                  {"confirmSave", {{"type", "boolean"}, {"description", "Send a text that names a save all the same"}}},
                  {"screen", {{"type", "string"}, {"description", "A shown screen's name, for call, set or get"}}},
                  {"call", {{"type", "string"}, {"description", "An ActionScript function's path in its movie"}}},
                  {"args", {{"type", "array"}, {"description", "The call's arguments: strings, numbers, bools"}}},
                  {"set", {{"type", "string"}, {"description", "A variable's path in its movie"}}},
                  {"value", {{"description", "What `set` puts there: a string, number or bool"}}},
                  {"get", {{"type", "string"}, {"description", "A number's or a bool's path in its movie"}}}}}},
            Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "input");
            if (watch.Mode() < 0)
            {
                throw ToolError("input: the game is still starting; health says when it runs");
            }
            const bool binding = args.Has("binding");
            const int forms = (binding ? 1 : 0) + (args.Has("message") ? 1 : 0) + (args.Has("screen") ? 1 : 0);
            if (forms != 1)
            {
                throw ToolError("input: give one of `message` (\"Receiver.Message arguments\", a button's), "
                                "`binding` (\"Message arguments\", a key's) or `screen` (a shown screen's name, "
                                "with call, set or get)");
            }
            if (args.Has("screen"))
            {
                return MovieCall(args);
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
