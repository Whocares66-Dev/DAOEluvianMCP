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

// A movie's answer as JSON, as this GFx gives it (DAOLib #24): undefined,
// null, a bool, a number or a string, and an object, array or clip as its
// string, a clip's being its full path.
Json Answered(const std::optional<dao::gui::Movie::Answer> &answer)
{
    if (!answer)
    {
        return Json{{"type", nullptr}, {"value", nullptr}};
    }
    using Type = dao::gui::Value::Type;
    switch (answer->type)
    {
    case Type::Null:
        return Json{{"type", "null"}, {"value", nullptr}};
    case Type::Boolean:
        return Json{{"type", "boolean"}, {"value", answer->boolean}};
    case Type::Number:
        return Json{{"type", "number"}, {"value", answer->number}};
    case Type::String:
    case Type::StringW:
        return Json{{"type", "string"}, {"value", answer->text}};
    case Type::Object:
    case Type::Array:
    case Type::DisplayObject:
        return Json{{"type", "object"}, {"value", answer->text}};
    default:
        return Json{{"type", "undefined"}, {"value", nullptr}};
    }
}

// A clip's children, by ActionScript's own depth functions (lib-agent,
// 2026-10-05): getNextHighestDepth is one above the highest depth in use,
// getInstanceAtDepth a depth's clip, which comes back as its full path. A
// clip made at run time (attachMovie, createEmptyMovieClip) sits at 0 and
// up; one placed in the authoring tool from -16384 up, walked when
// `placed`. The level-up screen's ability icons are made with no name to
// guess (#58); this reaches them.
constexpr int kMostDepths = 4096;
constexpr int kPlacedFrom = -16384;

Json Children(dao::gui::Movie &movie, const std::string &clip, bool placed)
{
    const auto highest = movie.Call((clip + ".getNextHighestDepth").c_str());
    if (!highest || highest->type != dao::gui::Value::Type::Number)
    {
        throw ToolError("input: \"" + clip +
                        "\" is not a clip (it has no getNextHighestDepth); `get` it to see what it is");
    }
    const int next = std::clamp(static_cast<int>(highest->number), 0, kMostDepths);
    const std::string at = clip + ".getInstanceAtDepth";
    Json children = Json::array();
    const auto look = [&](int depth) {
        const std::array<dao::gui::Value, 1> where{dao::gui::Value::Number(depth)};
        const auto child = movie.Call(at.c_str(), where);
        // Whatever answers is listed with its type: this GFx gave three
        // depths in use under the level-up scroller and none of them as a
        // display object (turn-based-agent, 2026-10-05), so a clip may come
        // as an object.
        Json answered = Answered(child);
        const Json type = answered["type"];
        if (type.is_string() && type != "undefined" && type != "null")
        {
            children.push_back(Json{{"depth", depth}, {"type", type}, {"path", answered["value"]}});
        }
    };
    if (placed)
    {
        for (int depth = kPlacedFrom; depth < 0; ++depth)
        {
            look(depth);
        }
    }
    for (int depth = 0; depth < next; ++depth)
    {
        look(depth);
    }
    return Json{{"children", std::move(children)}, {"nextHighestDepth", highest->number}};
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
    const int forms = (args.Has("call") ? 1 : 0) + (args.Has("set") ? 1 : 0) + (args.Has("get") ? 1 : 0) +
                      (args.Has("children") ? 1 : 0);
    if (forms != 1)
    {
        throw ToolError("input: with `screen`, give one of `call` (an ActionScript function's path, with "
                        "`args`), `set` (a variable's path, with `value`), `get` (a path, answered with its type "
                        "and value) or `children` (a clip's path, answered with its children's)");
    }
    if (args.Has("children"))
    {
        const std::string clip = args.String("children");
        Json answer = Children(*movie, clip, args.Bool("placed", false));
        answer["screen"] = screen;
        answer["of"] = clip;
        return answer;
    }
    if (args.Has("get"))
    {
        const std::string path = args.String("get");
        // For reading a screen's tree in play (#58).
        const auto answer = movie->Get(path.c_str());
        Json read = Answered(answer);
        read["screen"] = screen;
        read["get"] = path;
        if (!answer)
        {
            read["note"] = "the movie did not read that path";
        }
        return read;
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
        // GFx's SetVariable makes a variable where none is, so done says only
        // that it took the set: nExitPoint set without its LevelUp. answered
        // done, and the screen, reading LevelUp.nExitPoint, threw the pick
        // away (turn-based-agent, 2026-10-09, #107). Whether the path was
        // there is asked first.
        const bool existed = movie->IsAvailable(path.c_str());
        const bool done = movie->SetVariable(path.c_str(), value(args.Raw().at("value")));
        Json answer = {{"screen", screen}, {"set", path}, {"done", done}, {"existed", existed}};
        if (!existed)
        {
            answer["note"] = "the movie had no such path: the set made a new variable, which nothing reads unless "
                             "the movie's scripts ask for it by this path; give the full path they use "
                             "(get reads it, as LevelUp.nExitPoint)";
        }
        return answer;
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
    // What it returned with it (#58): a clip from getInstanceAtDepth, a
    // count, a label.
    const auto answer = movie->Call(function.c_str(), values);
    Json called = Answered(answer);
    called["screen"] = screen;
    called["call"] = function;
    called["ran"] = answer.has_value();
    return called;
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
            "`set` a variable to `value`, `get` a path's value, or list a clip's `children` (each's `depth` and "
            "`path`, to `get` or `call` under; `placed`: true for those placed in the movie's authoring as "
            "well as those made at run time); a call answers whether it `ran` and what it returned, a get or a "
            "call the `type` and `value` (an object or a clip as its string, a clip's being its full path), a "
            "set whether it was `done`.",
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
                  {"get",
                   {{"type", "string"},
                    {"description", "A path in its movie: answers its `type` and `value` (a number, a bool, a "
                                    "string; an object or a clip as its string, a clip's being its full path)"}}},
                  {"children", {{"type", "string"}, {"description", "A clip's path: its children, by depth"}}},
                  {"placed",
                   {{"type", "boolean"},
                    {"description", "With `children`: the clips placed in authoring too (depths below 0)"}}}}}},
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
                                "with call, set, get or children)");
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
