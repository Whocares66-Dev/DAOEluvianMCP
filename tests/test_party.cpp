// The party tool over stand-ins for the picker: health's mode, the data
// tree's chosen and hovered names, the view, the cursor held at a pixel, and
// the messages a player's clicks send.
#include "core/Party.h"

#include "Reads.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

struct Follower
{
    std::string name;
    std::string tag;
    // Where the follower shows, in the view's pixels.
    int left = 0;
    int right = 0;
    int y = 0;
};

struct Picker
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    std::string mode = "partypicker";
    std::vector<Follower> scene = {{"Leliana", "gen00fl_leliana", 360, 440, 450},
                                   {"Morrigan", "gen00fl_morrigan", 460, 540, 450},
                                   {"Lyna", "player", 760, 840, 450},
                                   {"Dog", "gen00fl_dog", 860, 940, 450},
                                   {"Alistair", "gen00fl_alistair", 960, 1040, 450},
                                   {"Sten", "gen00fl_sten", 650, 750, 600}};
    std::vector<Follower> chosen = {scene[2], scene[1], scene[4]}; // Lyna, Morrigan, Alistair
    std::string hovered;                                           // PartyPicker.MouseOverName
    bool sticky = false;      // whether the name stays when the cursor leaves a follower
    std::vector<int> pointed; // the last pixel held, empty when released
    int settling = 0;         // reads of the party after Accept that still show the hero alone
    int status = 2;           // GetPartyPickerGUIStatus: PP_GUI_STATUS_USE, the party changeable
    std::string kept;         // one the story keeps: unchoosing them takes no one out
    std::vector<std::string> sent;

    Picker()
    {
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", mode}}; });
        AddReads(bench, [this](const Json &args) -> ToolResult { return FakeInspect(args); });
        bench.Tools().Add(ToolSpec{"script", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeScript(args); });
        bench.Tools().Add(ToolSpec{"screen", "", {}, Thread::Listener}, [](const Json &) -> ToolResult {
            return Json{{"view", {{"width", 1600}, {"height", 1000}}}};
        });
        bench.Tools().Add(ToolSpec{"pointer", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakePointer(args); });
        bench.Tools().Add(ToolSpec{"input", "", {}, Thread::Listener},
                          [this](const Json &args) -> ToolResult { return FakeInput(args); });
        AddPartyTool(bench, std::chrono::milliseconds(0));
    }

    // The stand-in's inspect.
    ToolResult FakeInspect(const Json &args)
    {
        if (args.value("kind", "") == "game")
        {
            Json party = Json::array();
            for (const auto &member : chosen)
            {
                party.push_back(Json{{"name", member.name}, {"tag", member.tag}});
                if (settling > 0)
                {
                    break; // the hero alone until the game settles
                }
            }
            settling = std::max(0, settling - 1);
            return Json{{"party", party}};
        }
        Json values = Json::array();
        for (const auto &path : args.at("paths"))
        {
            values.push_back(Read(path.get<std::string>()));
        }
        return Json{{"values", values}};
    }

    // The stand-in's script.
    ToolResult FakeScript(const Json & /*args*/)
    {
        if (status < 0)
        {
            throw ToolError("script: not now"); // the status unread
        }
        return Json{{"value", status}}; // GetPartyPickerGUIStatus
    }

    // The stand-in's pointer.
    ToolResult FakePointer(const Json &args)
    {
        if (args.value("release", false))
        {
            pointed.clear();
            return Json{{"released", true}};
        }
        pointed = args.at("pixel").get<std::vector<int>>();
        const Follower *under = nullptr;
        for (const auto &follower : scene)
        {
            if (pointed[0] >= follower.left && pointed[0] <= follower.right && std::abs(pointed[1] - follower.y) <= 40)
            {
                under = &follower;
            }
        }
        if (under != nullptr || !sticky)
        {
            hovered = under != nullptr ? under->name : "";
        }
        return Json{{"held", true}};
    }

    // The stand-in's input.
    ToolResult FakeInput(const Json &args)
    {
        const std::string text = args.value("message", args.value("binding", ""));
        sent.push_back(text);
        if (text == "SelectPartyMemberUnderCursorMessage" && !pointed.empty() && chosen.size() < 4)
        {
            for (const auto &follower : scene)
            {
                if (follower.name == hovered)
                {
                    chosen.push_back(follower);
                }
            }
        }
        else if (text.starts_with("GameModeController.UnselectPartyMember "))
        {
            const auto at = chosen.begin() + std::stoi(text.substr(text.rfind(' ') + 1));
            if (at->name != kept)
            {
                chosen.erase(at);
            }
        }
        else if (text == "GameModeController.SubmitPartyChanges")
        {
            mode = "explore";
        }
        else if (text == "GameModeController.ClearPartyPicker")
        {
            chosen.resize(1); // the hero stays
        }
        return Json{{"sent", true}};
    }

    Json Read(const std::string &path) const
    {
        if (path == "PartyPicker.MouseOverName")
        {
            return Json{{"path", path}, {"found", true}, {"text", hovered}};
        }
        const std::string prefix = "PartyPicker.SelectedPartyMember.";
        const auto slot = static_cast<std::size_t>(path[prefix.size()] - '0');
        if (slot >= chosen.size())
        {
            return Json{{"path", path}, {"found", false}};
        }
        const bool name = path.ends_with(".Name");
        return Json{{"path", path}, {"found", true}, {"text", name ? chosen[slot].name : chosen[slot].tag}};
    }

    Json Party(const Json &args)
    {
        return bench.Invoke("party", args).data;
    }
};

} // namespace

TEST_CASE("a picker not open to change the party refuses choose, unchoose and clear, saying how", "[party]")
{
    Picker picker;
    picker.status = 0; // shown by ShowPartyPickerGUI alone
    for (const Json &args : {Json{{"action", "choose"}, {"name", "Leliana"}},
                             Json{{"action", "unchoose"}, {"name", "Morrigan"}}, Json{{"action", "clear"}}})
    {
        CHECK_THROWS_WITH(picker.Party(args), ContainsSubstring("status \"noUse\" (GetPartyPickerGUIStatus 0)"));
    }
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "clear"}}), ContainsSubstring("SetPartyPickerGUIStatus [2]"));
    CHECK(picker.sent.empty());
    // A look is answered, its status said.
    CHECK(picker.Party(Json{{"action", "look"}})["status"] == "noUse");
    picker.status = 1;
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "clear"}}), ContainsSubstring("status \"readOnly\""));
}

TEST_CASE("a picker whose status cannot be read is worked as before", "[party]")
{
    Picker picker;
    picker.status = -1;
    const auto answer = picker.Party(Json{{"action", "unchoose"}, {"name", "Morrigan"}});
    CHECK(answer["chosen"].size() == 2);
    CHECK(answer["status"] == -1);
}

TEST_CASE("an unchoose the picker does not take is said, not answered as done", "[party]")
{
    Picker picker;
    picker.kept = "Morrigan";
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "unchoose"}, {"name", "Morrigan"}}),
                      ContainsSubstring("Morrigan was not taken out"));
}

TEST_CASE("a clear that takes no one out says so", "[party]")
{
    Picker picker;
    picker.chosen.resize(1); // the hero alone, whom the story keeps
    const auto answer = picker.Party(Json{{"action", "clear"}});
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("clear took no one out"));
    picker.chosen.push_back(picker.scene[0]);
    CHECK_FALSE(picker.Party(Json{{"action", "clear"}}).contains("note"));
}

TEST_CASE("look says who is chosen, slot by slot", "[party]")
{
    Picker picker;
    const auto answer = picker.Party(Json{{"action", "look"}});
    REQUIRE(answer["chosen"].size() == 3);
    CHECK(answer["chosen"][0]["name"] == "Lyna");
    CHECK(answer["chosen"][2]["slot"] == 2);
    CHECK(answer["chosen"][2]["tag"] == "gen00fl_alistair");
    CHECK_FALSE(answer.contains("before"));
}

TEST_CASE("choose finds a follower in the scene by the name under the cursor, and clicks there", "[party]")
{
    Picker picker;
    const auto answer = picker.Party(Json{{"action", "choose"}, {"name", "sten"}});
    REQUIRE(answer["chosen"].size() == 4);
    CHECK(answer["chosen"][3]["name"] == "Sten");
    const auto pixel = answer["pixel"].get<std::vector<int>>();
    CHECK(pixel[0] >= 650);
    CHECK(pixel[0] <= 750);
    CHECK(pixel[1] == 600);
    CHECK(picker.sent == std::vector<std::string>{"SelectPartyMemberUnderCursorMessage"});
    CHECK(picker.pointed.empty()); // the cursor given back
    CHECK(answer["before"].size() == 3);
}

TEST_CASE("a name left from before is no hit: the pixel is where it turns to theirs", "[party]")
{
    Picker picker;
    picker.sticky = true;
    picker.hovered = "Sten"; // read from before the sweep, the cursor elsewhere
    const auto answer = picker.Party(Json{{"action", "choose"}, {"name", "Sten"}});
    CHECK(answer["chosen"].size() == 4);
    CHECK(answer["pixel"][1] == 600);
}

TEST_CASE("a fifth is refused by the game, and said so", "[party]")
{
    Picker picker;
    picker.chosen.push_back(picker.scene[3]); // Dog: four
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "choose"}, {"name", "Sten"}}), ContainsSubstring("four"));
    CHECK(picker.pointed.empty());
}

TEST_CASE("one not in the scene is refused, with who was seen there", "[party]")
{
    Picker picker;
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "choose"}, {"name", "Zevran"}}),
                      ContainsSubstring("seen there: Leliana, Morrigan, Lyna, Dog, Alistair, Sten"));
    CHECK(picker.pointed.empty());
    CHECK(picker.sent.empty());
}

TEST_CASE("one chosen already is not chosen again", "[party]")
{
    Picker picker;
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "choose"}, {"name", "Morrigan"}}),
                      ContainsSubstring("chosen already"));
}

TEST_CASE("unchoose clicks the portrait of their slot", "[party]")
{
    Picker picker;
    const auto answer = picker.Party(Json{{"action", "unchoose"}, {"name", "Alistair"}});
    CHECK(picker.sent == std::vector<std::string>{"GameModeController.UnselectPartyMember 2"});
    CHECK(answer["chosen"].size() == 2);
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "unchoose"}, {"name", "Sten"}}), ContainsSubstring("not chosen"));
}

TEST_CASE("accept waits for the picker to close and answers the party", "[party]")
{
    Picker picker;
    const auto answer = picker.Party(Json{{"action", "accept"}});
    CHECK(answer["accepted"] == true);
    CHECK(answer["mode"] == "explore");
    CHECK(answer["party"].size() == 3);
}

TEST_CASE("accept answers the party once it holds everyone chosen", "[party]")
{
    Picker picker;
    picker.settling = 2;
    const auto answer = picker.Party(Json{{"action", "accept"}});
    CHECK(answer["party"].size() == 3);
}

TEST_CASE("clear sends the picker's Clear", "[party]")
{
    Picker picker;
    const auto answer = picker.Party(Json{{"action", "clear"}});
    CHECK(picker.sent == std::vector<std::string>{"GameModeController.ClearPartyPicker"});
    CHECK(answer["chosen"].size() == 1);
}

TEST_CASE("the party tool is refused away from the picker, and choose wants a name", "[party]")
{
    Picker picker;
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "choose"}}), ContainsSubstring("`name`"));
    picker.mode = "explore";
    CHECK_THROWS_WITH(picker.Party(Json{{"action", "look"}}), ContainsSubstring("not up"));
}
