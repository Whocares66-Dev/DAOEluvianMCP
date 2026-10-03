// `inspect kind=gui`: what the interface has up beyond the HUD, for an
// agent that cannot see it: the game's message box and its loot panel. And
// the loot panel's two buttons, pressed as the panel's own script presses
// them, for game action=press and the loot tool. Which other screen is up
// (the inventory, the map, a store) is not read yet: DAOLib does not say
// which of the game's scenes are shown (#2).
#include "game/Shared.h"

#include "dao/Data.h"

#include <charconv>

namespace eluvian::game
{

namespace
{

// The loot panel is the container part of the inventory movie
// (inventory.gfx in guiexport.erf, its ActionScript class Container), read
// by disassembly 2026-10-02 (#1). What it shows is the data tree's:
// GUIInventory.ShowContainer, a bool, is true while it is up, and
// Container.* is in the tree only while a container is open (read in the
// game that night). Its buttons send texts through the FSCommand
// SendMessage, the path dao::messages::SendText takes, the panel joining a
// message's arguments with spaces:
//
// - Take All sends "SelectedChar.AcquireAllItems <Container.ID>"; then, if
//   the container still holds items and Party.BackpackSize is not below
//   Party.BackpackMaxSize, flashes the backpack-full overlay and stays
//   open, and otherwise does what Close does.
// - Close sets GUIInventory.ShowContainer to 0, then sends
//   "Container.InventoryClosedMessage", "GameModeController.PerformUseObject
//   <SelectedChar.ID> <Container.ID> 2" and "GUI.HideFullScreenGUI".
//
// Take All goes here as "GUI.SelectedChar..." rather than the movie's
// "SelectedChar...": the same node of the tree (both read 269 for 0x10d),
// and the form turn-based-agent sent through `input` the same night, which
// emptied a body bag, a pile of bones, a chest and a cocoon and closed the
// panel each time.

// An object id the tree holds as text, in decimal ("1004").
std::optional<ObjectId> TreeId(std::wstring_view path)
{
    const auto text = dao::data::Text(path);
    if (!text || text->empty())
    {
        return std::nullopt;
    }
    const auto narrow = Narrow(*text);
    ObjectId value = 0;
    const auto [end, error] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
    if (error != std::errc{} || end != narrow.data() + narrow.size())
    {
        return std::nullopt;
    }
    return value;
}

std::wstring TakeAllText(ObjectId container)
{
    return std::format(L"GUI.SelectedChar.AcquireAllItems {}", container);
}

std::vector<std::wstring> CloseTexts(ObjectId selected, ObjectId container)
{
    return {L"Container.InventoryClosedMessage",
            std::format(L"GameModeController.PerformUseObject {} {} 2", selected, container), L"GUI.HideFullScreenGUI"};
}

ObjectId Selected()
{
    return TreeId(L"SelectedChar.ID").value_or(api::GetMainControlled());
}

std::vector<ObjectId> ItemsIn(ObjectId container)
{
    const auto list = api::GetItemsInInventory(container, k::GET_ITEMS_OPTION_BACKPACK);
    std::vector<ObjectId> items;
    items.reserve(list.Size());
    for (std::size_t i = 0; i < list.Size(); ++i)
    {
        items.push_back(list.Object(i));
    }
    return items;
}

Json Backpack()
{
    return Json{{"items", dao::data::Number(L"Party.BackpackSize").value_or(0)},
                {"most", dao::data::Number(L"Party.BackpackMaxSize").value_or(0)}};
}

} // namespace

std::optional<ObjectId> LootPanelContainer()
{
    const auto *shown = dao::data::Find(L"GUIInventory.ShowContainer");
    if (shown == nullptr || shown->Number() == 0)
    {
        return std::nullopt;
    }
    return TreeId(L"Container.ID");
}

Json LootPanelJson()
{
    const auto container = LootPanelContainer();
    if (!container)
    {
        return nullptr;
    }
    Json items = Json::array();
    for (const ObjectId item : ItemsIn(*container))
    {
        items.push_back(ItemJson(item, false));
    }
    Json close = Json::array();
    for (const auto &text : CloseTexts(Selected(), *container))
    {
        close.push_back(Narrow(text));
    }
    return Json{{"container", Brief(*container)},
                {"count", items.size()},
                {"items", std::move(items)},
                {"backpack", Backpack()},
                {"buttons", Json::array({Json{{"label", kLootButtons[0]},
                                              {"sends", Json::array({Narrow(TakeAllText(*container))})},
                                              {"then", "Close, unless items are left and the backpack is full"}},
                                         Json{{"label", kLootButtons[1]},
                                              {"sets", {{"GUIInventory.ShowContainer", false}}},
                                              {"sends", std::move(close)}}})}};
}

Json PressLootButton(Bench &bench, std::size_t index)
{
    const auto container = LootPanelContainer();
    if (!container)
    {
        throw ToolError("game: no loot panel is up; order action=use on a container opens one, and inspect "
                        "kind=gui says what is up");
    }
    Json answer = {{"pressed", kLootButtons.at(index)}, {"container", Brief(*container)}};
    // What follows a press is on the bus from here: the game may start a
    // conversation or a plot's step as a container closes.
    answer["cursor"] = bench.Events().Next();
    Json sent = Json::array();
    const auto send = [&sent](const std::wstring &text) {
        sent.push_back(Json{{"text", Narrow(text)}, {"sent", dao::messages::SendText(text)}});
    };
    bool close = index == 1;
    if (index == 0)
    {
        // The items in brief before they move: one that joins a stack in
        // the backpack may be gone after.
        std::map<ObjectId, Json> before;
        for (const ObjectId item : ItemsIn(*container))
        {
            before.emplace(item, ItemJson(item, false));
        }
        send(TakeAllText(*container));
        const auto after = ItemsIn(*container);
        Json left = Json::array();
        for (const ObjectId item : after)
        {
            left.push_back(ItemJson(item, false));
            before.erase(item);
        }
        Json taken = Json::array();
        for (auto &[item, json] : before)
        {
            taken.push_back(std::move(json));
        }
        const auto size = dao::data::Number(L"Party.BackpackSize").value_or(0);
        const auto most = dao::data::Number(L"Party.BackpackMaxSize").value_or(0);
        const bool full = !after.empty() && most > 0 && size >= most;
        answer["taken"] = std::move(taken);
        answer["left"] = std::move(left);
        answer["full"] = full;
        answer["backpack"] = Backpack();
        close = !full;
    }
    if (close)
    {
        if (auto *shown = dao::data::Find(L"GUIInventory.ShowContainer"); shown != nullptr)
        {
            shown->Set(false);
        }
        // The ids from the tree, as the movie reads them; but read before
        // the first message rather than after it, so that one which lets
        // the container go from the tree leaves the use its id.
        const ObjectId still = TreeId(L"Container.ID").value_or(*container);
        for (const auto &text : CloseTexts(Selected(), still))
        {
            send(text);
        }
    }
    answer["sent"] = std::move(sent);
    answer["panel"] = LootPanelJson();
    return answer;
}

void AddGui(Bench &bench, const Watch &watch)
{
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"gui", "What the interface has up beyond the HUD, for when health's mode is gui or the game waits on "
                        "something: `popup`, the game's message box (as kind=popup), and `loot`, the loot panel a "
                        "container opens (a chest, a body's remains, a pile of bones): its `container`, the `items` "
                        "in it, the `backpack`'s items and most, and its `buttons`, Take All and Close, with what "
                        "each sends. Each is null when not up. game action=press presses a button of either; the "
                        "loot tool walks to a container, opens it and takes all in one call. Which other screen is "
                        "up (the inventory, the map, a store) is not read yet: a screenshot shows it. Answers in any "
                        "mode."},
        [&watch](const Json &) -> ToolResult {
            Json answer = {{"mode", ModeName(watch.Mode())}, {"popup", PopupJson()}};
            const auto mode = watch.Mode();
            answer["loot"] =
                mode < 0 || mode == static_cast<std::int32_t>(dao::GameMode::Loading) ? Json(nullptr) : LootPanelJson();
            return answer;
        });
}

} // namespace eluvian::game
