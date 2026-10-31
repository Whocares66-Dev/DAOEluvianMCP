#include "core/Options.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The options screen as DAOLib's dao::options answers it (#190): it opens a
// few reads after it is asked for, a tab shown by its button, a plugin's
// control taking a click two reads later and the game's own at once, and OK
// and Cancel closing it.
struct Screen
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    bool open = false;
    int opening = -1;         // reads of the state before it is up, once asked for
    bool menus = true;        // whether a menu can be had to open it from
    std::string tab = "game"; // the tab shown
    int sharpness = 1;        // ElgaraSharpness, a plugin's choice on the video tab
    int smaa = 0;             // ElgaraSMAA, a plugin's checkbox there
    double music = 0.5;       // AudioOptions.MusicVolume, the game's slider on the audio tab
    bool sound = true;        // whether the game's sound is on, which the volume needs
    int pendingName = 0;      // a plugin's click: 1 sharpness, 2 smaa
    double pendingValue = 0;
    int pendingReads = 0; // reads before the plugin takes it
    bool refuses = false; // the plugin's set keeps its value
    bool confirm = false; // OK asks a confirmation first
    bool pressed = false;
    std::vector<std::string> calls;

    Screen()
    {
        bench.Tools().Add(ToolSpec{"options.state", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return State(); });
        bench.Tools().Add(ToolSpec{"gui", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            return Json{{"popup", confirm && open ? Json{{"text", "Restart?"}} : Json(nullptr)},
                        {"fullScreen", open ? Json("Options") : Json("MainMenu")}};
        });
        for (const char *step : {"options.open", "options.tab", "options.set", "options.ok", "options.cancel"})
        {
            bench.Tools().Add(
                ToolSpec{step, "", {}, Thread::Listener},
                [this, step = std::string(step)](const Json &args) -> ToolResult { return Step(step, args); });
        }
        AddOptionsTool(bench);
    }

    void Settle()
    {
        if (opening > 0)
        {
            --opening;
            open = open || opening == 0;
        }
        if (pendingName != 0)
        {
            --pendingReads;
            if (pendingReads <= 0)
            {
                if (!refuses)
                {
                    (pendingName == 1 ? sharpness : smaa) = static_cast<int>(pendingValue);
                }
                pendingName = 0;
            }
        }
    }

    Json Entries() const
    {
        Json entries = Json::array();
        if (tab == "video")
        {
            entries.push_back(Json{{"name", "ElgaraSharpness"},
                                   {"label", "Sharpness"},
                                   {"kind", "choice"},
                                   {"value", sharpness},
                                   {"values", Json::array({"Off", "Low", "High"})},
                                   {"enabled", true},
                                   {"game", false}});
            entries.push_back(Json{{"name", "ElgaraSMAA"},
                                   {"label", "SMAA"},
                                   {"kind", "toggle"},
                                   {"value", smaa},
                                   {"enabled", true},
                                   {"game", false}});
            entries.push_back(Json{{"name", "ElgaraReset"}, {"label", "Reset"}, {"kind", "button"}, {"value", 0}});
        }
        if (tab == "audio")
        {
            entries.push_back(Json{{"name", "AudioOptions.MusicVolume"},
                                   {"label", "Music Volume"},
                                   {"kind", "slider"},
                                   {"value", music},
                                   {"least", 0.0},
                                   {"most", 1.0},
                                   {"enabled", sound},
                                   {"game", true}});
        }
        return entries;
    }

    Json State()
    {
        Settle();
        return Json{{"open", open},
                    {"tab", open ? Json(tab) : Json(nullptr)},
                    {"tabs", open ? Json::array({Json{{"tab", "game"}}, Json{{"tab", "video"}}}) : Json::array()},
                    {"entries", open ? Entries() : Json::array()}};
    }

    static Json Refused(const char *refusal)
    {
        return Json{{"taken", false}, {"refusal", refusal}};
    }

    Json Set(const Json &args)
    {
        const std::string name = args.value("name", "");
        const double value = args.value("value", -1.0);
        if (name == "ElgaraReset")
        {
            pressed = true;
            return Json{{"taken", true}};
        }
        if (name == "AudioOptions.MusicVolume")
        {
            if (!sound)
            {
                return Refused("disabled");
            }
            music = value;
            return Json{{"taken", true}};
        }
        const int most = name == "ElgaraSMAA" ? 1 : 2;
        if (value < 0 || value > most)
        {
            return Refused("out of range");
        }
        pendingName = name == "ElgaraSharpness" ? 1 : 2;
        pendingValue = value;
        pendingReads = 2;
        return Json{{"taken", true}};
    }

    Json Step(const std::string &step, const Json &args)
    {
        calls.push_back(step + (args.contains("tab") ? " " + args.at("tab").get<std::string>() : std::string()) +
                        (args.contains("name")
                             ? " " + args.at("name").get<std::string>() + " " + args.at("value").dump()
                             : std::string()));
        if (step == "options.open")
        {
            if (!menus)
            {
                return Refused("not shown");
            }
            if (open)
            {
                return Refused("already so");
            }
            opening = 3;
            return Json{{"taken", true}};
        }
        if (step == "options.tab")
        {
            if (args.value("tab", "") == "controls")
            {
                return Refused("disabled");
            }
            if (tab == args.value("tab", ""))
            {
                return Refused("already so");
            }
            tab = args.value("tab", "");
            return Json{{"taken", true}};
        }
        if (step == "options.set")
        {
            return Set(args);
        }
        if (step == "options.ok" && confirm)
        {
            return Json{{"taken", true}};
        }
        open = false;
        return Json{{"taken", true}};
    }

    Json Options(const Json &args)
    {
        return bench.Invoke("options", args).data;
    }

    void OpenOn(const char *shown)
    {
        Options(Json{{"action", "open"}});
        Options(Json{{"action", "tab"}, {"tab", shown}});
    }
};

} // namespace

TEST_CASE("options opens the screen as the player does and answers once it is up", "[options]")
{
    Screen screen;
    const auto answer = screen.Options(Json{{"action", "open"}});
    CHECK(answer["open"] == true);
    CHECK(answer["tab"] == "game");
    CHECK(screen.calls == std::vector<std::string>{"options.open"});
    // Asked again with it up: not a refusal.
    CHECK(screen.Options(Json{{"action", "open"}})["open"] == true);
}

TEST_CASE("options says why the screen cannot be opened", "[options]")
{
    Screen screen;
    screen.menus = false;
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "open"}}), ContainsSubstring("neither the start menu nor"));
}

TEST_CASE("options refuses a step with the screen closed, saying how to open it", "[options]")
{
    Screen screen;
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "tab"}, {"tab", "video"}}), ContainsSubstring("action open"));
    CHECK(screen.calls.empty());
}

TEST_CASE("options shows a tab by its button, its controls listed, and refuses a disabled one", "[options]")
{
    Screen screen;
    screen.Options(Json{{"action", "open"}});
    const auto answer = screen.Options(Json{{"action", "tab"}, {"tab", "video"}});
    CHECK(answer["tab"] == "video");
    REQUIRE(answer["entries"].size() == 3);
    CHECK(answer["entries"][0]["name"] == "ElgaraSharpness");
    CHECK(screen.Options(Json{{"action", "tab"}, {"tab", "video"}})["tab"] == "video"); // shown already
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "tab"}, {"tab", "controls"}}),
                      ContainsSubstring("tab was refused: the screen greys it out"));
}

TEST_CASE("options sets a choice by a value's name, and answers once the plugin took it", "[options]")
{
    Screen screen;
    screen.OpenOn("video");
    const auto answer = screen.Options(Json{{"action", "set"}, {"name", "sharpness"}, {"value", "high"}});
    CHECK(answer["took"] == true);
    CHECK(answer["value"] == 2);
    CHECK(screen.calls.back() == "options.set ElgaraSharpness 2.0");
}

TEST_CASE("options sets a checkbox by true or false", "[options]")
{
    Screen screen;
    screen.OpenOn("video");
    CHECK(screen.Options(Json{{"action", "set"}, {"name", "SMAA"}, {"value", true}})["value"] == 1);
    CHECK(screen.smaa == 1);
    CHECK(screen.Options(Json{{"action", "set"}, {"name", "SMAA"}, {"value", false}})["value"] == 0);
    CHECK(screen.smaa == 0);
}

TEST_CASE("options says when the plugin kept its value", "[options]")
{
    Screen screen;
    screen.OpenOn("video");
    screen.refuses = true;
    const auto answer = screen.Options(Json{{"action", "set"}, {"name", "ElgaraSharpness"}, {"value", 0}});
    CHECK(answer["took"] == false);
    CHECK(answer["value"] == 1);
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("refused the change"));
}

TEST_CASE("options sets a game's slider to a fraction, and refuses it while it is disabled", "[options]")
{
    Screen screen;
    screen.OpenOn("audio");
    const auto answer = screen.Options(Json{{"action", "set"}, {"name", "Music Volume"}, {"value", 0.25}});
    CHECK(answer["took"] == true);
    CHECK(screen.music == 0.25);
    screen.sound = false;
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "set"}, {"name", "AudioOptions.MusicVolume"}, {"value", 1}}),
                      ContainsSubstring("the screen greys it out"));
}

TEST_CASE("options presses a plugin's button, and refuses a control or value not there", "[options]")
{
    Screen screen;
    screen.Options(Json{{"action", "open"}});
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "set"}, {"name", "ElgaraSharpness"}, {"value", 1}}),
                      ContainsSubstring("on the game tab; there: none"));
    screen.Options(Json{{"action", "tab"}, {"tab", "video"}});
    CHECK(screen.Options(Json{{"action", "set"}, {"name", "Reset"}})["pressed"] == true);
    CHECK(screen.pressed);
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "set"}, {"name", "ElgaraSharpness"}, {"value", "ultra"}}),
                      ContainsSubstring("its values: [\"Off\",\"Low\",\"High\"]"));
    CHECK_THROWS_WITH(screen.Options(Json{{"action", "set"}, {"name", "ElgaraSharpness"}, {"value", 7}}),
                      ContainsSubstring("a checkbox takes true or false"));
}

TEST_CASE("options closes the screen with OK or Cancel, and answers a confirmation the game asks", "[options]")
{
    Screen screen;
    screen.Options(Json{{"action", "open"}});
    const auto cancelled = screen.Options(Json{{"action", "cancel"}});
    CHECK(cancelled["closed"] == true);
    CHECK(cancelled["back"] == "MainMenu"); // Escape's menu, up again as for a player
    CHECK_THAT(cancelled["note"].get<std::string>(), ContainsSubstring("GUI.HideFullScreenGUI"));
    CHECK_FALSE(screen.open);
    screen.Options(Json{{"action", "open"}});
    screen.confirm = true;
    const auto answer = screen.Options(Json{{"action", "ok"}});
    CHECK(answer["closed"] == false);
    CHECK(answer["popup"]["text"] == "Restart?");
}
