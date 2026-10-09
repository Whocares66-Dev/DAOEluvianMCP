// The board tool: the quest board's screen ("Chanters": the Chanter's board,
// Awakening's Letters for the Commander) read and answered as the screen does
// (#103). Its list is the data tree's Journal.ChanterBoard.ChanterEntries,
// each with PlotName, JournalText, PlotResRef and Accepted; Accept sends
// Journal.AcceptChantersQuest with the entry's index and reads Accepted back,
// set at once; Close is GUI.HideFullScreenGUI, and the movie's FinishScene
// sends Journal.ClearChanterBoard as it closes (lib-agent, read in
// chanters.gfx's ActionScript, 2026-10-09). turn-based-agent drove it by the
// movie's clicks in Awakening, 2026-10-09.
#include "game/Shared.h"

#include "dao/Data.h"

#include <cctype>

namespace eluvian::game
{

namespace
{

constexpr std::wstring_view kEntries = L"Journal.ChanterBoard.ChanterEntries";

std::string Text(const std::wstring &path)
{
    return Narrow(dao::data::Text(path).value_or(L""));
}

Json Entry(std::size_t index)
{
    const std::wstring base = std::format(L"{}.{}", kEntries, index);
    return Json{{"index", index},
                {"name", Text(base + L".PlotName")},
                {"plot", Text(base + L".PlotResRef")},
                {"accepted", dao::data::Number(base + L".Accepted").value_or(0) != 0},
                {"text", Text(base + L".JournalText")}};
}

Json Entries()
{
    Json entries = Json::array();
    const std::size_t count = dao::data::MemberCount(kEntries).value_or(0);
    for (std::size_t i = 0; i < count; ++i)
    {
        entries.push_back(Entry(i));
    }
    return entries;
}

bool BoardUp()
{
    const auto full = dao::gui::CurrentFullScreen();
    return full && full->name == L"Chanters";
}

std::string Lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The entry `entry` names: its index, or its name (any case) or plot.
std::size_t Named(const Args &args, const Json &entries)
{
    const Json &raw = args.Raw().at("entry");
    for (const Json &entry : entries)
    {
        const bool byIndex = raw.is_number_integer() && raw.get<std::size_t>() == entry.at("index").get<std::size_t>();
        const bool byName =
            raw.is_string() && (Lower(entry.value("name", std::string())) == Lower(raw.get<std::string>()) ||
                                entry.value("plot", std::string()) == raw.get<std::string>());
        if (byIndex || byName)
        {
            return entry.at("index").get<std::size_t>();
        }
    }
    Json names = Json::array();
    for (const Json &entry : entries)
    {
        names.push_back(entry.value("name", std::string()));
    }
    throw ToolError(std::format("board: no entry {} on the board; its entries: {}", raw.dump(), names.dump()));
}

ToolResult ToolBoard(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "board");
    const Args args(raw, "board");
    const auto action = args.Choice("action", {"look", "accept", "close"}, "look");
    if (!BoardUp())
    {
        throw ToolError("board: the quest board is not up (inspect kind=gui says the full screen); a use of the "
                        "board placeable opens it");
    }
    if (action == "close")
    {
        dao::messages::SendText(L"GUI.HideFullScreenGUI");
        return Json{{"closed", true}, {"note", "the board's own close: it clears the board's list as it goes"}};
    }
    const Json entries = Entries();
    if (action == "look")
    {
        return Json{{"count", entries.size()}, {"entries", entries}};
    }
    if (!args.Has("entry"))
    {
        throw ToolError("board: accept needs `entry`, an index, a name or a plot from board's look");
    }
    const std::size_t index = Named(args, entries);
    if (entries.at(index).value("accepted", false))
    {
        return Json{{"entry", entries.at(index)}, {"accepted", true}, {"note", "accepted already"}};
    }
    dao::messages::SendText(std::format(L"Journal.AcceptChantersQuest {}", index));
    const Json after = Entry(index);
    return Json{{"entry", after}, {"accepted", after.value("accepted", false)}};
}

} // namespace

void AddBoard(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"board",
                 "The quest board while it is up (the Chanter's board; Awakening's Letters for the Commander), "
                 "as its screen reads and answers it. `action`: look (default), its `entries`, each with "
                 "`index`, `name`, `plot` (the plot's resource), whether `accepted`, and its `text`; accept "
                 "`entry` (an index, a name in any case, or a plot), answering whether it was `accepted`, as the "
                 "game marks it at once; close, as its Close button. A use of the board placeable opens it.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"look", "accept", "close"}}}},
                        {"entry", {{"description", "accept: the entry's index, name or plot"}}}}}},
                 Thread::Frame},
        [&watch](const Json &raw) -> ToolResult { return ToolBoard(watch, raw); });
}

} // namespace eluvian::game
