// The options screen's steps through DAOLib's dao::options (DAOLib #272,
// b0293ec, lib-agent-3, 2026-10-30): the screen opened as the player opens
// it, with its menu's backing panel, a tab shown by its own button, a
// plugin's control set as a click on it does, and OK and Cancel. core's
// options asks these, unlisted, where agents drove the screen through movie
// calls: a scene played by itself showed a page under another tab's light
// and with no panel, and plugins' sliders were clicked through their clips
// (the user, DAOElgaraShaders #68). The game's own controls, with the tab
// shown, since DAOLib #273 (d87db2e): named by their option
// (AudioOptions.MusicVolume), and a slider's value a number in its range.
#include "game/Shared.h"

#include "dao/Options.h"

namespace eluvian::game
{

namespace
{

namespace options = dao::options;

constexpr std::array<std::pair<options::Tab, std::string_view>, 5> kTabs = {{{options::Tab::Game, "game"},
                                                                             {options::Tab::Video, "video"},
                                                                             {options::Tab::Online, "online"},
                                                                             {options::Tab::Controls, "controls"},
                                                                             {options::Tab::Audio, "audio"}}};

std::string_view TabName(options::Tab tab)
{
    for (const auto &[each, name] : kTabs)
    {
        if (each == tab)
        {
            return name;
        }
    }
    return "?";
}

options::Tab TabNamed(const Args &args)
{
    const auto name = args.Choice("tab", {"game", "video", "online", "controls", "audio"});
    for (const auto &[each, named] : kTabs)
    {
        if (named == name)
        {
            return each;
        }
    }
    return options::Tab::Game;
}

Json Taken(options::Refusal refusal)
{
    if (refusal == options::Refusal::None)
    {
        return Json{{"taken", true}};
    }
    return Json{{"taken", false},
                {"refusal", Label(refusal, {{options::Refusal::NotShown, "not shown"},
                                            {options::Refusal::AlreadySo, "already so"},
                                            {options::Refusal::Disabled, "disabled"},
                                            {options::Refusal::NotOffered, "not offered"},
                                            {options::Refusal::OutOfRange, "out of range"}})}};
}

ToolSpec Step(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.listed = false;
    return spec;
}

} // namespace

void AddOptions(Bench &bench)
{
    bench.Tools().Add(
        Read("options.state",
             "The options screen as it stands: whether it is `open`, its `tabs` (tab, name, enabled, current), and "
             "the `entries` on the tab shown, the game's own and the plugins' (name, label, kind, value, values or "
             "least and most, enabled, game).",
             Json::object(), {}, false),
        [](const Json &) -> ToolResult {
            Json tabs = Json::array();
            std::optional<options::Tab> shown;
            for (const options::TabState &tab : options::Tabs())
            {
                tabs.push_back(Json{{"tab", TabName(tab.tab)},
                                    {"name", Narrow(tab.name)},
                                    {"enabled", tab.enabled},
                                    {"current", tab.current}});
                if (tab.current)
                {
                    shown = tab.tab;
                }
            }
            Json entries = Json::array();
            if (shown)
            {
                for (const options::EntryState &entry : options::Entries(*shown))
                {
                    Json values = Json::array();
                    for (const std::wstring &value : entry.values)
                    {
                        values.push_back(Narrow(value));
                    }
                    Json json = {{"name", Narrow(entry.name)},
                                 {"label", Narrow(entry.label)},
                                 {"kind", Label(entry.kind, {{options::Kind::Toggle, "toggle"},
                                                             {options::Kind::Choice, "choice"},
                                                             {options::Kind::Slider, "slider"},
                                                             {options::Kind::Button, "button"}})},
                                 {"value", entry.value},
                                 {"enabled", entry.enabled},
                                 {"game", entry.game}};
                    if (entry.kind == options::Kind::Choice)
                    {
                        json["values"] = std::move(values);
                    }
                    if (entry.kind == options::Kind::Slider)
                    {
                        json["least"] = entry.least;
                        json["most"] = entry.most;
                    }
                    entries.push_back(std::move(json));
                }
            }
            return Json{{"open", options::IsOpen()},
                        {"tab", shown ? Json(std::string(TabName(*shown))) : Json(nullptr)},
                        {"tabs", std::move(tabs)},
                        {"entries", std::move(entries)}};
        });

    bench.Tools().Add(Step("options.open", "Opens the options screen as the player does: `taken`, or the `refusal`.",
                           Json::object(), {}),
                      [](const Json &) -> ToolResult { return Taken(options::Open()); });
    bench.Tools().Add(
        Step("options.tab", "Shows a tab as a click on its button does.", Json{{"tab", {{"type", "string"}}}}, {"tab"}),
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "options.tab");
            return Taken(options::ShowTab(TabNamed(args)));
        });
    bench.Tools().Add(
        Step("options.set",
             "Sets a control as a click or drag on it does: a checkbox 0 or 1, a choice's index, a slider's number.",
             Json{{"name", {{"type", "string"}}}, {"value", {{"type", "number"}}}}, {"name", "value"}),
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "options.set");
            return Taken(options::Set(Wide(args.String("name")), args.Number("value")));
        });
    bench.Tools().Add(Step("options.ok", "Presses OK as the player does.", Json::object(), {}),
                      [](const Json &) -> ToolResult { return Taken(options::Ok()); });
    bench.Tools().Add(Step("options.cancel", "Presses Cancel as the player does.", Json::object(), {}),
                      [](const Json &) -> ToolResult { return Taken(options::Cancel()); });
}

} // namespace eluvian::game
