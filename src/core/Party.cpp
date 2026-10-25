#include "core/Party.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace eluvian
{

namespace
{

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// How long the picker may take to be done showing: its entries are none
// until then, a few frames after the mode enters (lib-agent-3, 2026-10-24).
constexpr milliseconds kShowWait{5000};
// How long the picker may take to close after Accept.
constexpr milliseconds kCloseWait{5000};
// How long the party may take to hold everyone chosen once the picker closed.
constexpr milliseconds kSettleWait{3000};

// The picker's status, as the scripts set it before showing it
// (SetPartyPickerGUIStatus; script.ldf's PP_GUI_STATUS_*): only with USE
// can the party be changed in it.
constexpr std::array<std::string_view, 3> kStatusNames = {"noUse", "readOnly", "use"};
constexpr int kUse = 2;
constexpr const char *kStatusAdvice = "The game opens it to change the party with status use: script "
                                      "SetPartyPickerGUIStatus [2], then ShowPartyPickerGUI again";

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

class Picker
{
  public:
    Picker(Bench &bench, const Args &args)
        : bench_(bench), action_(args.Choice("action", {"look", "choose", "unchoose", "accept", "clear"})),
          name_(args.String("name", ""))
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
        // no one could be chosen or taken out (turn-based-agent, 2026-10-05,
        // #62); the game's own callers set USE first.
        const int status = Status();
        answer_["status"] = status >= 0 && std::cmp_less(status, kStatusNames.size())
                                ? Json(std::string(kStatusNames[static_cast<std::size_t>(status)]))
                                : Json(status);
        if (status >= 0 && status != kUse && action_ != "look" && action_ != "accept")
        {
            throw ToolError("party: the picker is up with status " + answer_["status"].dump() +
                            " (GetPartyPickerGUIStatus " + std::to_string(status) +
                            "), in which no one can be chosen or taken out. " + kStatusAdvice);
        }
        const Json entries = Shown();
        answer_["before"] = Chosen(entries);
        if (action_ == "choose")
        {
            const Json entry = Entry(entries);
            Take("party.choose", entry, entry.value("name", name_) + " was not chosen");
        }
        else if (action_ == "unchoose")
        {
            const Json entry = Entry(entries);
            Take("party.release", entry, entry.value("name", name_) + " was not taken out");
        }
        else if (action_ == "clear")
        {
            Take("party.release_all", Json(), "no one was taken out");
        }
        else if (action_ == "accept")
        {
            return Accept();
        }
        const Json after = Entries();
        answer_["chosen"] = Chosen(after);
        answer_["offered"] = after;
        if (action_ == "clear" && answer_.at("chosen") == answer_.at("before"))
        {
            answer_["note"] = "clear took no one out: those chosen are the ones the story keeps in the party";
        }
        if (action_ == "look")
        {
            answer_.erase("before");
        }
        return answer_;
    }

  private:
    Json Entries()
    {
        return bench_.Invoke("party.entries", Json::object()).data.value("entries", Json::array());
    }

    // The entries once the picker is done showing.
    Json Shown()
    {
        const auto until = Clock::now() + kShowWait;
        while (true)
        {
            Json entries = Entries();
            if (!entries.empty())
            {
                return entries;
            }
            if (Clock::now() >= until)
            {
                throw ToolError("party: the picker showed no one after 5 s");
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    // The chosen, slot by slot, as the portraits show them.
    static Json Chosen(const Json &entries)
    {
        std::vector<Json> chosen;
        for (const auto &entry : entries)
        {
            if (entry.value("slot", -1) >= 0)
            {
                chosen.push_back(Json{{"slot", entry.at("slot")},
                                      {"id", entry.value("id", Json())},
                                      {"name", entry.value("name", std::string())},
                                      {"tag", entry.value("tag", std::string())}});
            }
        }
        std::ranges::sort(chosen, {}, [](const Json &member) { return member.value("slot", -1); });
        Json slots = Json::array();
        for (auto &member : chosen)
        {
            slots.push_back(std::move(member));
        }
        return slots;
    }

    // The entry `name` names: its name in any case, its tag or its id.
    Json Entry(const Json &entries) const
    {
        for (const auto &entry : entries)
        {
            if (Lower(entry.value("name", std::string())) == Lower(name_) || entry.value("tag", "") == name_ ||
                entry.value("id", "") == name_)
            {
                return entry;
            }
        }
        std::string names;
        for (const auto &entry : entries)
        {
            names += (names.empty() ? "" : ", ") + entry.value("name", std::string()) + " (" +
                     entry.value("state", std::string()) + ")";
        }
        throw ToolError("party: the picker shows no one named " + name_ + "; it shows " + names);
    }

    // A step of the picker's; refused with the picker's reason said.
    void Take(std::string_view tool, const Json &entry, const std::string &undone)
    {
        const Json args = entry.is_null() ? Json::object() : Json{{"id", entry.at("id")}};
        const Json took = bench_.Invoke(tool, args).data;
        if (took.value("taken", false))
        {
            return;
        }
        const std::string refusal = took.value("refusal", std::string("refused"));
        std::string why = refusal;
        if (refusal == "party full")
        {
            why = "four are chosen: unchoose one first";
        }
        else if (refusal == "already so")
        {
            why = tool == "party.choose" ? "chosen already" : "not chosen";
        }
        else if (refusal == "locked")
        {
            why = "the story keeps them where they are (the hero, or one it keeps in or out of the party)";
        }
        else if (refusal == "read only")
        {
            why = std::string("the picker is only to look at. ") + kStatusAdvice;
        }
        else if (refusal == "not shown")
        {
            why = "the picker is not up, or is still showing or leaving";
        }
        throw ToolError("party: " + undone + ": " + why);
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
        Take("party.accept", Json(), "the party was not accepted");
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
        // party a moment later (turn-based-agent, 2026-10-03); the scripts
        // change it a frame or more after Accept (DAOLib #259).
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
    Json answer_ = Json::object();
};

} // namespace

void AddPartyTool(Bench &bench)
{
    bench.Tools().Add(
        ToolSpec{"party",
                 "Works the party picker, the screen the game shows after a follower joins and at camp, as a "
                 "player's clicks do. It refuses when the picker is not open (`health` reports mode `partypicker` "
                 "when it is).\n\n"
                 "`action`:\n"
                 "- `look`: returns who is `chosen` (each slot's id, name and tag), and everyone the picker "
                 "`offered`, each with its `state`: chosen, available, unavailable, suspended or lockedActive (kept "
                 "in the party by the story).\n"
                 "- `choose`: picks the follower `name`, as a click on them does.\n"
                 "- `unchoose`: removes the chosen follower `name`, like clicking their portrait.\n"
                 "- `clear`: removes everyone except the hero and those the story keeps.\n"
                 "- `accept`: confirms the party as chosen, and returns the `party` once the picker has closed.\n\n"
                 "Up to four can be chosen: unchoose one before choosing a fifth. A choice the picker refuses is "
                 "an error saying why. `choose`, `unchoose` and `clear` refuse when the picker's `status` is not "
                 "`use`, meaning the picker is only for viewing. A picker you open yourself with the script "
                 "command ShowPartyPickerGUI may be view-only: call SetPartyPickerGUIStatus with 2 through "
                 "`script` before opening it, so it can change the party.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"look", "choose", "unchoose", "accept", "clear"}},
                          {"description", "What to do; each action is described above."}}},
                        {"name",
                         {{"type", "string"},
                          {"description", "With `choose` or `unchoose`: a follower's name as the picker shows it, "
                                          "their tag, or their id."}}}}},
                      {"required", {"action"}}},
                 Thread::Listener},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "party");
            return Picker(bench, args).Run();
        });
}

} // namespace eluvian
