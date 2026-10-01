// `game`: the game itself: pause, the saves and loading one, saving, and
// pressing a button of its message box.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

dao::saves::Kind KindArg(const Args &args, std::string_view fallback)
{
    const auto kind = args.Choice("kind", {"manual", "auto", "quick"}, fallback);
    return kind == "auto" ? dao::saves::Kind::Auto
                          : (kind == "quick" ? dao::saves::Kind::Quick : dao::saves::Kind::Manual);
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

void AddGame(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        // No game speed here. Each plugin's copy of DAOLib hooks the frame's
        // timer on its first SetSpeed, so two plugins setting a speed
        // multiply, and each reads back only its own (DAOLib ed55ab8,
        // 2026-09-30). One plugin owns the speed: in a turn-based test the
        // mod, through turnbased_settings' ai_turn_speed.
        ToolSpec{
            "game",
            "The game itself. `action`: pause, unpause; saves, the saves of `character` (or every one) with "
            "each one's area, name, time played and level, as the load screen shows them; load, a save by "
            "`character` (default the one played, or the menu's), `kind` (manual, auto, quick; default "
            "manual) and `number`, from the main menu or in the world, as the load screen loads; save, a new "
            "manual save in the next free slot, with `name` (the default: it writes over nothing), a manual "
            "save to slot `number` (which writes over what is there), or `kind` quick, which writes over "
            "the player's quicksave. A load or save answers that the game was asked, and `cursor`: wait_for "
            "eluvian.game.loaded or eluvian.game.saved from it. The game declines a save in combat or a "
            "cutscene. press, a `button` of the game's message box by its label or place (inspect "
            "kind=popup reads the box): the game is sent what a click on it sends. (Not the game's speed: each "
            "plugin's copy of DAOLib scales the clock by its own, so "
            "two setting it multiply; a mod that sets speed owns it.)",
            Json{{"type", "object"},
                 {"properties",
                  {{"action", {{"type", "string"}, {"enum", {"pause", "unpause", "saves", "load", "save", "press"}}}},
                   {"button", {{"description", "action=press: the button's label (\"Cancel\") or place from 0"}}},
                   {"character", {{"type", "string"}, {"description", "A character's save folder, \"Aedan\""}}},
                   {"kind", {{"type", "string"}, {"enum", {"manual", "auto", "quick"}}}},
                   {"number", {{"type", "integer"}, {"description", "The slot's number"}}},
                   {"name", {{"type", "string"}, {"description", "action=save: a manual save's name"}}}}},
                 {"required", {"action"}}},
            Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "game");
            const auto action = args.Choice("action", {"pause", "unpause", "saves", "load", "save", "press"});
            auto *app = dao::CClientExoApp::Get();
            if (app == nullptr || watch.Mode() < 0)
            {
                throw ToolError("game: the game is still starting; health says when it runs");
            }
            if (action == "press")
            {
                // A message box's button, by its place or its label: the game
                // is sent what the button sends (DAOLib bf85cb1). Until this a
                // box a test met could only be read, and a person had to click.
                const auto popup = dao::gui::CurrentPopup();
                if (!popup)
                {
                    throw ToolError("game: no message box is up to press a button of; inspect kind=popup reads one");
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
                        const auto has = Narrow(popup->buttons[i].label);
                        if (std::ranges::equal(has, label, [](unsigned char a, unsigned char b) {
                                return std::tolower(a) == std::tolower(b);
                            }))
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
                    throw ToolError(std::format("game: press needs `button`, its label or its place from 0; "
                                                "the box \"{}\" has: {}",
                                                Narrow(popup->text), labels));
                }
                Json answer = {{"pressed",
                                {{"label", Narrow(popup->buttons[index].label)},
                                 {"sends", Narrow(popup->buttons[index].callback)}}},
                               {"box", Narrow(popup->text)},
                               {"cursor", bench.Events().Next()}};
                if (!dao::gui::PressPopupButton(index))
                {
                    throw ToolError("game: the game took no press of that button (the engine made no message of "
                                    "what it sends)");
                }
                answer["popup"] = PopupJson();
                return answer;
            }
            if (action == "pause" || action == "unpause")
            {
                app->SetPaused(action == "pause");
                return Json{{"paused", app->IsPaused()}};
            }
            if (action == "saves")
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
            RequireWorld(watch, "game");
            const auto cursor = bench.Events().Next();
            if (action == "load")
            {
                const dao::saves::Slot slot{Wide(args.String("character", "")),
                                            static_cast<std::int32_t>(args.Integer("number", 1, 0, 500)),
                                            KindArg(args, "manual")};
                if (!dao::saves::Load(slot))
                {
                    throw ToolError("game: no such save to load: the slot holds none, or the game knows no such "
                                    "character; action saves lists them");
                }
                return Json{{"asked", true},
                            {"save", SlotJson(slot)},
                            {"cursor", cursor},
                            {"note",
                             "wait_for eluvian.game.loaded from `cursor`. If none comes and health's mode does not "
                             "move, the game is showing a message box of its own (a save that needs add-ins "
                             "not registered asks Force Load or Cancel): inspect kind=popup reads it"}};
            }
            // A new manual save by default: a test's save must not write over
            // the player's own, as a quicksave does (computer-use-agent would
            // not call this for that reason, 2026-09-30).
            const auto kind = KindArg(args, "manual");
            const auto number = static_cast<std::int32_t>(args.Integer("number", -1, -1, 500));
            if (!dao::saves::Save(kind, number, Wide(args.String("name", ""))))
            {
                throw ToolError("game: the game began no save: it allows none now (in combat, in a cutscene), or "
                                "is still starting");
            }
            return Json{{"began", true},
                        {"kind", dao::saves::Name(kind)},
                        {"slot", number < 0 ? Json("the game's choice: eluvian.game.saved names it") : Json(number)},
                        {"cursor", cursor},
                        {"note", "wait_for eluvian.game.saved from `cursor`"}};
        });
}

} // namespace eluvian::game
