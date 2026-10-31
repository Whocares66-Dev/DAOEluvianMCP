#include "core/Options.h"

#include "core/Args.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// How long the screen may take to open or close, and a plugin's control to
// take a click: a few frames in play.
constexpr milliseconds kWait{5000};

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A refusal of the screen's, in words.
std::string Why(const std::string &refusal)
{
    if (refusal == "not shown")
    {
        return "the screen is not open, or the control's tab is not shown";
    }
    if (refusal == "disabled")
    {
        return "the screen greys it out";
    }
    if (refusal == "not offered")
    {
        return "no control by that name is on the tab shown";
    }
    if (refusal == "out of range")
    {
        return "a checkbox takes true or false, a choice one of its values, a slider a number from its least "
               "to its most";
    }
    return refusal;
}

class Working
{
  public:
    Working(Bench &bench, const Args &args)
        : bench_(bench), args_(args), action_(args.Choice("action", {"look", "open", "tab", "set", "ok", "cancel"}))
    {
    }

    Json Run()
    {
        if (action_ == "open")
        {
            return Open();
        }
        if (action_ == "look")
        {
            return Look(State());
        }
        Json state = State();
        if (!state.value("open", false))
        {
            throw ToolError("options: the options screen is not open; options with action open opens it");
        }
        if (action_ == "tab")
        {
            Step("options.tab", Json{{"tab", args_.String("tab")}}, true);
            return Look(State());
        }
        if (action_ == "set")
        {
            return Set(state);
        }
        Step(action_ == "ok" ? "options.ok" : "options.cancel", Json::object(), false);
        return Closed();
    }

  private:
    Json State()
    {
        return bench_.Invoke("options.state", Json::object()).data;
    }

    static Json Look(Json state)
    {
        return state;
    }

    // A step of the screen's; refused with its reason, but where `already`
    // the screen being so already is no refusal.
    void Step(const char *tool, const Json &args, bool already)
    {
        const Json took = bench_.Invoke(tool, args).data;
        const std::string refusal = took.value("refusal", std::string());
        if (took.value("taken", false) || (already && refusal == "already so"))
        {
            return;
        }
        throw ToolError("options: " + std::string(tool).substr(8) + " was refused: " + Why(refusal));
    }

    // Asked for, then waited for: it opens a few frames later.
    Json Open()
    {
        const Json took = bench_.Invoke("options.open", Json::object()).data;
        const std::string refusal = took.value("refusal", std::string());
        if (refusal == "not shown")
        {
            throw ToolError("options: neither the start menu nor Escape's menu can be had now (a conversation, a "
                            "cutscene, a load or another full screen is up); gui says what is");
        }
        if (!took.value("taken", false) && refusal != "already so")
        {
            throw ToolError("options: open was refused: " + Why(refusal));
        }
        const auto until = Clock::now() + kWait;
        while (true)
        {
            Json state = State();
            if (state.value("open", false))
            {
                return Look(std::move(state));
            }
            if (Clock::now() >= until)
            {
                throw ToolError("options: the options screen was asked for and was not open after 5 s; gui says "
                                "what is up");
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    // A control on the tab shown, the game's own (by its option,
    // AudioOptions.MusicVolume) or a plugin's, by its name or label in any
    // case; refused naming those there.
    static Json Entry(const Json &state, const std::string &asked)
    {
        std::string names;
        for (const auto &entry : state.value("entries", Json::array()))
        {
            if (Lower(entry.value("name", std::string())) == Lower(asked) ||
                Lower(entry.value("label", std::string())) == Lower(asked))
            {
                return entry;
            }
            names += (names.empty() ? "" : ", ") + entry.value("name", std::string());
        }
        throw ToolError("options: no control \"" + asked + "\" on the " + state.value("tab", std::string()) +
                        " tab; there: " + (names.empty() ? std::string("none") : names) +
                        " (options with action tab shows another)");
    }

    // The value asked as the control takes it: a checkbox's true or false, a
    // choice's index or the name of one of its values, a slider's number (a
    // fraction for gamma or the mouse's sensitivity); a button's none.
    static double Value(const Json &entry, const Json &asked)
    {
        if (asked.is_boolean())
        {
            return asked.get<bool>() ? 1 : 0;
        }
        if (asked.is_number())
        {
            return asked.get<double>();
        }
        if (asked.is_string())
        {
            const Json values = entry.value("values", Json::array());
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                if (Lower(values[i].get<std::string>()) == Lower(asked.get<std::string>()))
                {
                    return static_cast<double>(i);
                }
            }
            throw ToolError("options: " + entry.value("name", std::string()) + " has no value \"" +
                            asked.get<std::string>() + "\"; its values: " + values.dump());
        }
        if (entry.value("kind", std::string()) == "button")
        {
            return 0;
        }
        throw ToolError("options: `value` is true or false for a checkbox, an index or a value's name for a choice, "
                        "a number for a slider");
    }

    // Set as a click does, then read back once the plugin has taken it, a
    // frame later: its own `set` may refuse, leaving the value as it was.
    Json Set(const Json &state)
    {
        const Json entry = Entry(state, args_.String("name"));
        const std::string name = entry.value("name", std::string());
        const double value = Value(entry, args_.Raw().value("value", Json()));
        Step("options.set", Json{{"name", name}, {"value", value}}, false);
        Json answer = {{"name", name}, {"asked", value}};
        if (entry.value("kind", std::string()) == "button")
        {
            answer["pressed"] = true;
            return answer;
        }
        const auto until = Clock::now() + kWait;
        while (true)
        {
            std::this_thread::sleep_for(milliseconds(100));
            const Json now = Entry(State(), name);
            const double read = now.value("value", -1.0);
            const bool took = std::abs(read - value) < 1e-4;
            if (took || Clock::now() >= until)
            {
                answer["value"] = read;
                answer["took"] = took;
                if (!took)
                {
                    answer["note"] = "the control kept its value: a plugin's own set refused the change";
                }
                return answer;
            }
        }
    }

    // OK or Cancel, then the screen waited for to close; a change the game
    // asks a confirmation for opens its message box first.
    Json Closed()
    {
        const auto until = Clock::now() + kWait;
        while (Clock::now() < until)
        {
            if (!State().value("open", false))
            {
                Json answer = {{"action", action_}, {"closed", true}};
                // The menu it was opened from, up again as for a player: in the
                // game, Escape's menu (fullScreen MainMenu), which kept the mode
                // gui until closed (play, 2026-10-30).
                const Json back = bench_.Invoke("gui", Json::object()).data.value("fullScreen", Json());
                if (back.is_string() && !back.get<std::string>().empty())
                {
                    answer["back"] = back;
                    answer["note"] = "the menu it was opened from is up again; input {\"message\": "
                                     "\"GUI.HideFullScreenGUI\"} closes it";
                }
                return answer;
            }
            const Json popup = bench_.Invoke("gui", Json::object()).data.value("popup", Json());
            if (popup.is_object())
            {
                return Json{{"action", action_},
                            {"closed", false},
                            {"popup", popup},
                            {"note", "the game asks first: press answers its box"}};
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        return Json{{"action", action_}, {"closed", false}, {"note", "the screen was still open after 5 s"}};
    }

    Bench &bench_;
    const Args &args_;
    std::string action_;
};

} // namespace

void AddOptionsTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{"options",
                 "Works the game's options screen as a player does, through its own buttons. `action`:\n"
                 "- open: opens it from the start menu, or from Escape's menu in the game, with that menu's panel, "
                 "and answers once it is up (refused in a conversation, a cutscene, a load or another full screen).\n"
                 "- look: whether it is `open`, its `tabs` (tab, name, enabled, current), the `tab` shown, and its "
                 "controls (`entries`), the game's own and the plugins': name (a game control's is its option, as "
                 "AudioOptions.MusicVolume), label, kind (toggle, choice, slider or button), value, a choice's "
                 "`values` or a slider's `least` and `most`, whether it is `enabled`, and whether it is the `game`'s.\n"
                 "- tab: shows `tab` (game, video, online, controls or audio) as a click on its button does.\n"
                 "- set: sets the control `name` (its name or label) on the tab shown to `value`: true or false for "
                 "a checkbox, an index or a value's name for a choice, a number for a slider, nothing for a "
                 "plugin's button, which it presses; answers whether it `took`. A control not `enabled` is refused. "
                 "A change of resolution or full screen asks its confirmation at ok.\n"
                 "- ok: keeps the changes and closes it; cancel: closes it and gives each plugin's control back the "
                 "value it had when the screen opened. A change the game asks to confirm answers with its `popup`, "
                 "which press answers.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"look", "open", "tab", "set", "ok", "cancel"}},
                          {"description", "What to do."}}},
                        {"tab",
                         {{"type", "string"},
                          {"enum", {"game", "video", "online", "controls", "audio"}},
                          {"description", "For tab: the tab to show."}}},
                        {"name", {{"type", "string"}, {"description", "For set: the control, by its name or label."}}},
                        {"value",
                         {{"description", "For set: true or false, a choice's index or the name of one of its "
                                          "values."}}}}},
                      {"required", {"action"}}},
                 Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "options");
            return Working(bench, args).Run();
        });
}

} // namespace eluvian
