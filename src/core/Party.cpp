#include "core/Party.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int kSlots = 4;
// Rows of the view to look along, as fractions of its height, the likeliest
// first: the five at 0.45 and Sten alone at 0.6 at his recruitment.
constexpr std::array kRows = {0.45, 0.6, 0.5, 0.4, 0.55, 0.65, 0.35, 0.7};
// Across a row, a twenty-fourth of the width at a time: 67 pixels of 1600,
// where the followers stood 100 apart. Some seconds a row at 0.3 s a pixel.
constexpr int kAcross = 24;
// How long the picker may take to close after Accept.
constexpr milliseconds kCloseWait{5000};
// How long the party may take to hold everyone chosen once the picker closed.
constexpr milliseconds kSettleWait{3000};

// The picker's status, as the scripts set it before showing it
// (SetPartyPickerGUIStatus; script.ldf's PP_GUI_STATUS_*): only with USE
// can the party be changed in it.
constexpr std::array<std::string_view, 3> kStatusNames = {"noUse", "readOnly", "use"};
constexpr int kUse = 2;

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

class Picker
{
  public:
    Picker(Bench &bench, const Args &args, milliseconds settle)
        : bench_(bench), action_(args.Choice("action", {"look", "choose", "unchoose", "accept", "clear"})),
          name_(args.String("name", "")), settle_(settle)
    {
        answer_["cursor"] = bench_.Events().Next();
        if ((action_ == "choose" || action_ == "unchoose") && name_.empty())
        {
            throw ToolError("party: " + action_ + " needs `name`, a follower's as the picker shows it");
        }
    }

    Json Run()
    {
        const Json health = bench_.Invoke("health", Json::object()).data;
        const std::string mode = health.value("mode", std::string());
        if (mode != "partypicker")
        {
            throw ToolError("party: the party picker is not up (the game is in mode " + mode +
                            "); the game opens it after a follower joins, and at camp");
        }
        // Opened by ShowPartyPickerGUI alone it came up with status 0, and
        // no one could be chosen or taken out, the calls answering as if
        // done (turn-based-agent, 2026-10-05, #62); the game's own callers
        // set USE first.
        const int status = Status();
        answer_["status"] = status >= 0 && std::cmp_less(status, kStatusNames.size())
                                ? Json(std::string(kStatusNames[static_cast<std::size_t>(status)]))
                                : Json(status);
        if (status >= 0 && status != kUse && action_ != "look" && action_ != "accept")
        {
            throw ToolError("party: the picker is up with status " + answer_["status"].dump() +
                            " (GetPartyPickerGUIStatus " + std::to_string(status) +
                            "), in which no one can be chosen or taken out. The game opens it to change the party "
                            "with status use: script SetPartyPickerGUIStatus [2], then ShowPartyPickerGUI again");
        }
        answer_["before"] = Chosen();
        if (action_ == "choose")
        {
            Choose();
        }
        else if (action_ == "unchoose")
        {
            Unchoose();
        }
        else if (action_ == "clear")
        {
            Send("GameModeController.ClearPartyPicker");
            std::this_thread::sleep_for(settle_);
            if (Chosen() == answer_.at("before"))
            {
                answer_["note"] = "clear took no one out: those chosen are the ones the story keeps in the party";
            }
        }
        else if (action_ == "accept")
        {
            return Accept();
        }
        answer_["chosen"] = Chosen();
        if (action_ == "look")
        {
            answer_.erase("before");
        }
        return answer_;
    }

  private:
    // The chosen, slot by slot, as the portraits show them.
    Json Chosen()
    {
        Json paths = Json::array();
        for (int slot = 0; slot < kSlots; ++slot)
        {
            paths.push_back("PartyPicker.SelectedPartyMember." + std::to_string(slot) + ".Name");
            paths.push_back("PartyPicker.SelectedPartyMember." + std::to_string(slot) + ".Tag");
        }
        const Json values = Read(paths);
        Json chosen = Json::array();
        for (int slot = 0; slot < kSlots; ++slot)
        {
            const Json &name = values.at(2 * slot);
            if (name.value("found", false))
            {
                chosen.push_back(Json{{"slot", slot},
                                      {"name", name.value("text", std::string())},
                                      {"tag", values.at((2 * slot) + 1).value("text", std::string())}});
            }
        }
        return chosen;
    }

    Json Read(const Json &paths)
    {
        return bench_.Invoke("data", Json{{"paths", paths}}).data.value("values", Json::array());
    }

    std::string Hovered()
    {
        const Json values = Read(Json::array({"PartyPicker.MouseOverName"}));
        return values.empty() ? std::string() : values.at(0).value("text", std::string());
    }

    std::optional<int> SlotOf(const Json &chosen) const
    {
        for (const auto &member : chosen)
        {
            if (Lower(member.value("name", std::string())) == Lower(name_) || member.value("tag", "") == name_)
            {
                return member.value("slot", -1);
            }
        }
        return std::nullopt;
    }

    void Send(const std::string &message)
    {
        const Json sent = bench_.Invoke("input", Json{{"message", message}}).data;
        if (!sent.value("sent", false))
        {
            throw ToolError("party: the game took no \"" + message + "\"");
        }
    }

    // A follower chosen in by a click on them in the scene: their pixel
    // found by holding the cursor across the view until the name under it is
    // theirs, then the click's binding there.
    void Choose()
    {
        if (SlotOf(answer_.at("before")))
        {
            throw ToolError("party: " + name_ + " is chosen already");
        }
        const Json screen = bench_.Invoke("screen", Json::object()).data;
        const double width = screen.value(Json::json_pointer("/view/width"), 0.0);
        const double height = screen.value(Json::json_pointer("/view/height"), 0.0);
        if (width <= 0 || height <= 0)
        {
            throw ToolError("party: the game gave no view to look across");
        }
        // Whether the name clears when the cursor leaves a follower is not
        // known: a pixel is taken as theirs only where the name turns to
        // theirs, so one read there from before is no hit.
        std::string last = Hovered();
        const auto pixel = Find(width, height, last);
        if (!pixel)
        {
            Release();
            throw ToolError("party: no follower named " + name_ + " was found in the picker's scene" +
                            (seen_.empty() ? std::string() : "; seen there: " + seen_));
        }
        answer_["pixel"] = *pixel;
        const Json sent = bench_.Invoke("input", Json{{"binding", "SelectPartyMemberUnderCursorMessage"}}).data;
        Release(); // the cursor the mouse's again once the click is sent
        if (!sent.value("sent", false))
        {
            throw ToolError("party: the game took no SelectPartyMemberUnderCursorMessage");
        }
        std::this_thread::sleep_for(settle_);
        if (!SlotOf(Chosen()))
        {
            throw ToolError("party: " + name_ + " was under the cursor at " + pixel->dump() +
                            " and the click chose nobody: four may be chosen already (unchoose one first)");
        }
    }

    std::optional<Json> Find(double width, double height, std::string &last)
    {
        for (const double row : kRows)
        {
            for (int step = 1; step < kAcross; ++step)
            {
                const Json pixel =
                    Json::array({static_cast<int>(width * step / kAcross), static_cast<int>(height * row)});
                bench_.Invoke("pointer", Json{{"pixel", pixel}});
                std::this_thread::sleep_for(settle_);
                const std::string name = Hovered();
                if (name != last && !name.empty() && seen_.find(name) == std::string::npos)
                {
                    seen_ += (seen_.empty() ? "" : ", ") + name;
                }
                const bool turned = name != last;
                last = name;
                if (turned && Lower(name) == Lower(name_))
                {
                    return std::make_optional(pixel);
                }
            }
        }
        return std::nullopt;
    }

    void Release()
    {
        try
        {
            bench_.Invoke("pointer", Json{{"release", true}});
        }
        catch (const std::exception &e)
        {
            // Said, not thrown: the cursor goes back to the mouse at its next
            // move anyway.
            answer_["pointer"] = std::string("not given back: ") + e.what();
        }
    }

    // One taken out by a click on their portrait, by its slot.
    void Unchoose()
    {
        const auto slot = SlotOf(answer_.at("before"));
        if (!slot)
        {
            throw ToolError("party: " + name_ + " is not chosen; the chosen: " + answer_.at("before").dump());
        }
        Send("GameModeController.UnselectPartyMember " + std::to_string(*slot));
        std::this_thread::sleep_for(settle_);
        if (SlotOf(Chosen()))
        {
            throw ToolError("party: " + name_ +
                            " was not taken out: the picker keeps them, as it keeps those the "
                            "story wants in the party");
        }
    }

    // The picker's status (GetPartyPickerGUIStatus); -1 if it cannot be
    // read, which refuses nothing.
    int Status()
    {
        try
        {
            const Json read = bench_.Invoke("script", Json{{"command", "GetPartyPickerGUIStatus"}}).data;
            const Json value = read.value("value", Json());
            return value.is_number() ? value.get<int>() : -1;
        }
        catch (const ToolError &)
        {
            return -1;
        }
    }

    // Accept, and the party it made once the picker has closed.
    Json Accept()
    {
        Send("GameModeController.SubmitPartyChanges");
        const auto until = Clock::now() + kCloseWait;
        std::string mode = "partypicker";
        while (mode == "partypicker" && Clock::now() < until)
        {
            std::this_thread::sleep_for(milliseconds(100));
            mode = bench_.Invoke("health", Json::object()).data.value("mode", std::string());
        }
        answer_["mode"] = mode;
        if (mode == "partypicker")
        {
            throw ToolError("party: the picker was still up 5 s after Accept");
        }
        answer_["accepted"] = true;
        // The party read once it holds everyone chosen: read as the picker
        // closed it answered two where four were chosen and four were in the
        // party a moment later (turn-based-agent, 2026-10-03).
        const std::size_t chosen = answer_.at("before").size();
        const auto settle = Clock::now() + kSettleWait;
        Json party = nullptr;
        while (true)
        {
            try
            {
                party = bench_.Invoke("state", Json::object()).data.value("party", Json::array());
            }
            catch (const std::exception &)
            {
                party = nullptr; // the world not yet back
            }
            if ((party.is_array() && party.size() >= chosen) || Clock::now() >= settle)
            {
                break;
            }
            std::this_thread::sleep_for(milliseconds(200));
        }
        answer_["party"] = party; // null if the world was not back: state says it after
        return answer_;
    }

    Bench &bench_;
    std::string action_;
    std::string name_;
    milliseconds settle_;
    std::string seen_; // the names met under the cursor, for a follower not found
    Json answer_ = Json::object();
};

} // namespace

void AddPartyTool(Bench &bench, std::chrono::milliseconds settle)
{
    bench.Tools().Add(
        ToolSpec{"party",
                 "The party picker, the screen the game shows after a follower joins and at camp, as a player "
                 "works it. `action`: look, who is `chosen` (each slot's name and tag); choose `name`, a "
                 "follower standing in the picker's scene, found by holding the cursor across the view until "
                 "the name under it is theirs (some seconds), then clicked as a player clicks them; unchoose "
                 "`name`, a chosen one, as a click on their portrait; clear, all but those the story keeps; "
                 "accept, the party as chosen, answering the `party` once the picker has closed. Four may be "
                 "chosen: unchoose one before choosing a fifth. Refused when the picker is not up (health "
                 "says mode partypicker when it is), and choose, unchoose and clear when it is up with a "
                 "`status` other than use (the scripts' GetPartyPickerGUIStatus: one opened by "
                 "ShowPartyPickerGUI alone may be noUse; SetPartyPickerGUIStatus 2 before it opens it to "
                 "change the party).",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"look", "choose", "unchoose", "accept", "clear"}}}},
                        {"name",
                         {{"type", "string"},
                          {"description", "choose, unchoose: a follower's name as the picker shows it, or a tag"}}}}},
                      {"required", {"action"}}},
                 Thread::Listener},
        [&bench, settle](const Json &raw) -> ToolResult {
            const Args args(raw, "party");
            return Picker(bench, args, settle).Run();
        });
}

} // namespace eluvian
