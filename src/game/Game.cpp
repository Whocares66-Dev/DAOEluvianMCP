// The game itself: `saves` (the list, loading one, saving), `pause`, and
// `press`, a button of its message box or its loot panel. One `game` tool
// held all three until #110.
#include "game/Shared.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace eluvian::game
{

namespace
{

// Whether the game in the world was force-loaded: loaded without content
// its save needs, which a save of it would lose for good. Such a game is
// never saved (the user's rule, 2026-10-01; force loading for vanilla tests
// allowed 2026-10-02). Frame thread only, as the tool is.
bool g_forceLoaded = false;

dao::saves::Kind KindArg(const Args &args, std::string_view fallback)
{
    const auto kind = args.Choice("kind", {"manual", "auto", "quick"}, fallback);
    if (kind == "auto")
    {
        return dao::saves::Kind::Auto;
    }
    if (kind == "quick")
    {
        return dao::saves::Kind::Quick;
    }
    return dao::saves::Kind::Manual;
}

bool SameWords(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b,
                              [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// A button of the loot panel, by its label or its place.
std::size_t LootButton(const Args &args)
{
    const auto &wanted = args.Raw().value("button", Json());
    for (std::size_t i = 0; i < kLootButtons.size(); ++i)
    {
        if ((wanted.is_number_integer() && wanted.get<std::int64_t>() == static_cast<std::int64_t>(i)) ||
            (wanted.is_string() && SameWords(wanted.get<std::string>(), kLootButtons[i])))
        {
            return i;
        }
    }
    throw ToolError("press: needs `button`, its label or its place from 0; the loot panel has: Take All, Close");
}

Json SlotJson(const dao::saves::Slot &slot)
{
    Json answer = {
        {"character", Narrow(slot.character)}, {"kind", dao::saves::Name(slot.kind)}, {"number", slot.number}};
    if (const auto meta = slot.Meta())
    {
        answer["area"] = Narrow(meta->area);
        answer["name"] = Narrow(meta->name);
        answer["secondsPlayed"] = meta->secondsPlayed;
        answer["level"] = meta->level;
    }
    return answer;
}

} // namespace

namespace
{

// A message box's button pressed, or the loot panel's.
Json GamePress(Bench &bench, const Args &args)
{
    // A message box's button, by its place or its label: the game
    // is sent what the button sends (DAOLib bf85cb1). Until this a
    // box a test met could only be read, and a person had to click.
    const auto popup = dao::gui::CurrentPopup();
    if (!popup)
    {
        // The loot panel's, when no box is over it (#1). A box is
        // the popup layer's, above every other screen.
        if (LootPanelContainer())
        {
            return PressLootButton(bench, LootButton(args));
        }
        throw ToolError("press: no message box and no loot panel is up to press a button of; "
                        "gui reads what is up");
    }
    std::size_t index = popup->buttons.size();
    const auto &wanted = args.Raw().value("button", Json());
    if (wanted.is_number_integer())
    {
        index = wanted.get<std::size_t>();
    }
    else if (wanted.is_string())
    {
        const auto label = wanted.get<std::string>();
        for (std::size_t i = 0; i < popup->buttons.size(); ++i)
        {
            if (SameWords(Narrow(popup->buttons[i].label), label))
            {
                index = i;
            }
        }
    }
    if (index >= popup->buttons.size())
    {
        std::string labels;
        for (const auto &button : popup->buttons)
        {
            labels += (labels.empty() ? "" : ", ") + Narrow(button.label);
        }
        throw ToolError(std::format("press: needs `button`, its label or its place from 0; "
                                    "the box \"{}\" has: {}",
                                    Narrow(popup->text), labels));
    }
    Json answer = {
        {"pressed",
         {{"label", Narrow(popup->buttons[index].label)}, {"sends", Narrow(popup->buttons[index].callback)}}},
        {"box", Narrow(popup->text)},
        {"cursor", bench.Events().Next()}};
    if (!dao::gui::PressPopupButton(index))
    {
        throw ToolError("press: the game took no press of that button (the engine made no message of "
                        "what it sends)");
    }
    // Force Load pressed as a button is a force-load all the same:
    // the game it loads is not to be saved either.
    auto pressed = Narrow(popup->buttons[index].label);
    std::ranges::transform(pressed, pressed.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (pressed.find("force") != std::string::npos)
    {
        g_forceLoaded = true;
    }
    answer["popup"] = PopupJson();
    return answer;
}

// The saves on disk, a character's or every one's.
Json GameSaves(const Args &args)
{
    std::vector<std::wstring> characters;
    if (args.Has("character"))
    {
        characters.push_back(Wide(args.String("character")));
    }
    else
    {
        characters = dao::saves::Characters();
    }
    Json saves = Json::array();
    for (const auto &character : characters)
    {
        for (const auto &slot : dao::saves::List(character))
        {
            saves.push_back(SlotJson(slot));
        }
    }
    return Json{{"current", Narrow(dao::saves::Character())}, {"saves", std::move(saves)}};
}

Json GameLoad(const Watch &watch, const Args &args, std::uint64_t cursor)
{
    // Asked during a conversation, the game took the load and did
    // nothing, no loaded event, in the Fade (turn-based-agent,
    // 2026-10-03, #36): refused, saying how to go on.
    if (watch.Mode() == static_cast<int>(dao::GameMode::Dialog))
    {
        throw ToolError("saves: a load is not taken during a conversation: answer or end it first "
                        "(dialogue), or start the game in that save (game_launch with `load`)");
    }
    const dao::saves::Slot slot{Wide(args.String("character", "")),
                                static_cast<std::int32_t>(args.Integer("number", 1, 0, 500)), KindArg(args, "manual")};
    if (!dao::saves::Load(slot))
    {
        throw ToolError("saves: no such save to load: the slot holds none, or the game knows no such "
                        "character; action saves lists them");
    }
    g_forceLoaded = false;
    return Json{{"asked", true},
                {"save", SlotJson(slot)},
                {"cursor", cursor},
                {"note", "wait_for eluvian.game.loaded from `cursor`. If none comes and health's mode does not "
                         "move, the game is showing a message box of its own (a save that needs add-ins "
                         "not registered asks Force Load or Cancel): gui reads it"}};
}

// The game's own Force Load, for a test on a save made with content
// this install has not (the user's vanilla tests, 2026-10-02):
// DAOLib's ForceLoad, the dialog's button.
Json GameForceLoad(std::uint64_t cursor)
{
    if (!dao::saves::ForceLoad())
    {
        throw ToolError("saves: no load was asked for to force; action=load asks for one, and the "
                        "game's dialog for missing content then waits on Force Load");
    }
    g_forceLoaded = true;
    return Json{{"forced", true},
                {"cursor", cursor},
                {"note", "wait_for eluvian.game.loaded from `cursor`. The game loads without the "
                         "content the save needs: it is for testing, and is never saved (action "
                         "save refuses until another load)"}};
}

// The lowest manual slot no save holds, where a new manual save goes;
// refused past the most the engine keeps.
std::int32_t FreeManualSlot()
{
    std::set<std::int32_t> taken;
    for (const auto &slot : dao::saves::List(dao::saves::Character()))
    {
        if (slot.kind == dao::saves::Kind::Manual)
        {
            taken.insert(slot.number);
        }
    }
    std::int32_t number = 1;
    while (taken.contains(number))
    {
        ++number;
    }
    // The engine refuses a manual save above its most, a literal in its
    // save handler, and starts none: in Denerim's market every save
    // was refused, Lyna having 500 (turn-based-agent, 2026-10-05;
    // lib-agent's reading, DAOLib #37, #79).
    if (number > dao::saves::MostSlot(dao::saves::Kind::Manual))
    {
        throw ToolError(std::format("saves: {} has {} manual saves, the most the game keeps, and it "
                                    "makes no more; free a slot (game_saves lists them, each a folder)",
                                    Narrow(dao::saves::Character()), dao::saves::MostSlot(dao::saves::Kind::Manual)));
    }
    return number;
}

Json GameSave(const Args &args, std::uint64_t cursor)
{
    if (g_forceLoaded)
    {
        throw ToolError("saves: this game was force-loaded without content its save needs, and a save of "
                        "it would lose that content for good: it is not saved. Load a save normally to "
                        "save again");
    }
    // A new manual save by default: a test's save must not write over
    // the player's own, as a quicksave does (computer-use-agent would
    // not call this for that reason, 2026-09-30).
    const auto kind = KindArg(args, "manual");
    auto number = static_cast<std::int32_t>(args.Integer("number", -1, -1, 500));
    // The game's "next free slot" (-1) is set up by its save screen:
    // in the world without it, a manual save to -1 was declined, and
    // one to a free slot named began and was written (2026-10-02). So
    // the free slot is found here, the lowest no save holds.
    if (number < 0 && kind == dao::saves::Kind::Manual)
    {
        number = FreeManualSlot();
    }
    // Tests may make saves of their own (the user, 2026-10-02): new
    // ones. A save already there is written over only when asked.
    if (!args.Bool("overwrite", false))
    {
        if (kind == dao::saves::Kind::Quick)
        {
            throw ToolError("saves: the quicksave is the player's; a test makes a new manual save (leave out "
                            "`kind` and `number`), or passes overwrite: true for a quicksave of its own");
        }
        if (number >= 0)
        {
            for (const auto &slot : dao::saves::List(dao::saves::Character()))
            {
                if (slot.kind == kind && slot.number == number)
                {
                    throw ToolError(std::format(
                        "saves: {} save {} of {} holds a save; a test makes a new one (leave out `number`), "
                        "or passes overwrite: true for a save of its own",
                        dao::saves::Name(kind), number, Narrow(slot.character)));
                }
            }
        }
    }
    if (!dao::saves::Save(kind, number, Wide(args.String("name", ""))))
    {
        throw ToolError("saves: the game began no save: it allows none now (in combat, in a cutscene), or "
                        "is still starting");
    }
    return Json{{"began", true},
                {"kind", dao::saves::Name(kind)},
                {"slot", number < 0 ? Json("the game's choice: eluvian.game.saved names it") : Json(number)},
                {"cursor", cursor},
                {"note", "wait_for eluvian.game.saved from `cursor`"}};
}

// The game tool's handler, registered by AddGame: an action a function
// (#77).
// The game up, or a refusal naming the tool asked.
dao::CClientExoApp &RunningApp(const Watch &watch, const std::string &tool)
{
    auto *app = dao::CClientExoApp::Get();
    if (app == nullptr || watch.Mode() < 0)
    {
        throw ToolError(tool + ": the game is still starting; health says when it runs");
    }
    return *app;
}

ToolResult ToolSaves(Bench &bench, const Watch &watch, const Json &raw)
{
    const Args args(raw, "saves");
    const auto action = args.Choice("action", {"list", "load", "force_load", "save"});
    RunningApp(watch, "saves");
    if (action == "list")
    {
        return GameSaves(args);
    }
    RequireWorld(watch, "saves");
    const auto cursor = bench.Events().Next();
    if (action == "load")
    {
        return GameLoad(watch, args, cursor);
    }
    if (action == "force_load")
    {
        return GameForceLoad(cursor);
    }
    return GameSave(args, cursor);
}

} // namespace

void AddGame(Bench &bench, const Watch &watch)
{
    // No game speed here. Each plugin's copy of DAOLib hooks the frame's
    // timer on its first SetSpeed, so two plugins setting a speed
    // multiply, and each reads back only its own (DAOLib ed55ab8,
    // 2026-09-30). One plugin owns the speed: in a turn-based test the
    // mod, through its own settings.
    bench.Tools().Add(
        Spec("saves",
             "The game's saves. `action`: list, the saves of `character` (or every one) with each one's area, "
             "name, time played and level, as the load screen shows them; load, a save by `character` (default "
             "the one played, or the menu's), `kind` (manual, auto, quick; default manual) and `number`, from the "
             "main menu or in the world, as the load screen loads; force_load, the game's Force Load for a load "
             "it answered with \"requires the following content\" (gui), which loads without that content: such "
             "a game is never saved; save, a new manual save in the next free slot, with `name` (the default, and "
             "what a test does: it writes over nothing). A slot `number` that holds a save, and `kind` quick (the "
             "player's quicksave), are refused unless `overwrite` is true, for a test's own save. A load or save "
             "answers that the game was asked, and `cursor`: wait_for eluvian.game.loaded or eluvian.game.saved "
             "from it. The game declines a save in combat or a cutscene.",
             Json{{"action", {{"type", "string"}, {"enum", {"list", "load", "force_load", "save"}}}},
                  {"character", {{"type", "string"}, {"description", "A character's save folder, \"Aedan\""}}},
                  {"kind", {{"type", "string"}, {"enum", {"manual", "auto", "quick"}}}},
                  {"number", {{"type", "integer"}, {"description", "The slot's number"}}},
                  {"name", {{"type", "string"}, {"description", "save: a manual save's name"}}},
                  {"overwrite",
                   {{"type", "boolean"},
                    {"description", "save: write over the save in slot `number`, or the quicksave"}}}},
             {"action"}),
        [&bench, &watch](const Json &raw) -> ToolResult { return ToolSaves(bench, watch, raw); });
    bench.Tools().Add(
        Spec("pause",
             "Pause the game or let it run, as the space bar does: `paused` true pauses, false unpauses. Answers "
             "whether it is `paused` after. Tools on the frame thread run paused or not.",
             Json{{"paused", {{"type", "boolean"}, {"description", "true pauses, false lets it run"}}}}, {"paused"}),
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "pause");
            auto &app = RunningApp(watch, "pause");
            app.SetPaused(args.Bool("paused", true));
            return Json{{"paused", app.IsPaused()}};
        });
    bench.Tools().Add(
        Spec("press",
             "Press a `button` by its label or place: of the game's message box, or with none up, of the loot "
             "panel a container opens, Take All or Close (gui reads both): the game is sent what a click on it "
             "sends. Take All answers what was `taken`, what is `left` and whether the backpack was `full`, and "
             "closes the panel unless it was.",
             Json{{"button", {{"description", "A button's label (\"Take All\") or place from 0"}}}}, {"button"}),
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "press");
            RunningApp(watch, "press");
            return GamePress(bench, args);
        });
}

} // namespace eluvian::game
