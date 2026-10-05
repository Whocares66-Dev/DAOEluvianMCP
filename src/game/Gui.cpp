// `inspect kind=gui`: what the interface has up, for an agent that cannot
// see it: the game's screens shown (#2), its message box and its loot panel
// (#1). And the loot panel's two buttons, pressed as the panel's own script
// presses them, for game action=press and the loot tool.
#include "game/Shared.h"

#include "dao/Data.h"

#include <algorithm>
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

} // namespace

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

namespace
{

std::wstring TakeAllText(ObjectId container)
{
    return std::format(L"GUI.SelectedChar.AcquireAllItems {}", container);
}

std::vector<std::wstring> CloseTexts(ObjectId selected, ObjectId container)
{
    return {L"Container.InventoryClosedMessage",
            std::format(L"GameModeController.PerformUseObject {} {} 2", selected, container), L"GUI.HideFullScreenGUI"};
}

} // namespace

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

namespace
{

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
        KindSpec{"gui", "What the interface has up, for when health's mode is gui or the game waits on something: "
                        "`screens`, the game's screens on the screen now, the top first, each by `name` (Inventory, "
                        "Store, WorldMap, PartyPicker, CharacterRecord, Map, Journal, Skills, Options, Main the HUD), "
                        "its `movie` and `layer`; `fullScreen`, the name of the full-screen one up (the inventory, "
                        "the map, the journal...), null with none; `fullScreenHidden`, a full-screen one left on "
                        "the game's stack of them but not on the screen (a screen closed by its report alone, "
                        "WorldMap.WorldMapClosed): input {\"message\": \"GUI.HideFullScreenGUI\"} clears it, as a "
                        "screen's Close button does; `popup`, the game's message box with "
                        "its text and each button's label and what pressing it sends (a box holds the game until "
                        "it is answered, and nothing else says it is there: ask when a load or an order was taken "
                        "and nothing followed); and `loot`, the loot panel a container opens (a chest, a body's "
                        "remains, a pile of bones), "
                        "which is the Inventory screen: its `container`, the `items` in it, the `backpack`'s items "
                        "and most, and its `buttons`, Take All and Close, with what each sends. game action=press "
                        "presses a button of the message box or the loot panel; the loot tool walks to a "
                        "container, opens it and takes all in one call. `store`, with a merchant's Store screen up: "
                        "the `stock` with each item's `price`, the `pack` with what each `sellsFor`, the `money` "
                        "(copper), the markups; the store tool buys and sells. Answers in any mode."},
        [&watch](const Json &) -> ToolResult {
            // DAOLib's ShownScreens and CurrentFullScreen (0dd22bb, #2): a
            // screen is shown while its movie is drawn, and the full-screen
            // one is the last the GUI system pushed. The loot panel is the
            // Inventory screen; GUIInventory.ShowContainer tells them apart.
            Json screens = Json::array();
            std::vector<std::wstring> shown;
            for (const auto &screen : dao::gui::ShownScreens())
            {
                shown.push_back(screen.name);
                screens.push_back(Json{{"name", Narrow(screen.name)},
                                       {"movie", Narrow(screen.movie)},
                                       {"layer", screen.layer},
                                       {"id", screen.id}});
            }
            // The stack's top is the full-screen one only while it is on
            // the screen: the world map closed by WorldMap.WorldMapClosed
            // alone, which is the screen's report that it closed and not
            // the close (worldmap.gfx: its Close button sends
            // GUI.HideFullScreenGUI, and FinishScene the report), stayed
            // on the stack, and fullScreen went on saying WorldMap with no
            // map drawn (turn-based-agent, 2026-10-04, #39).
            const auto full = dao::gui::CurrentFullScreen();
            const bool fullShown = full && std::ranges::find(shown, full->name) != shown.end();
            Json answer = {{"mode", ModeName(watch.Mode())},
                           {"screens", std::move(screens)},
                           {"fullScreen", fullShown ? Json(Narrow(full->name)) : Json(nullptr)},
                           {"popup", PopupJson()}};
            if (full && !fullShown)
            {
                answer["fullScreenHidden"] = Narrow(full->name);
            }
            const auto mode = watch.Mode();
            const bool world = mode >= 0 && mode != static_cast<std::int32_t>(dao::GameMode::Loading);
            answer["loot"] = world ? LootPanelJson() : Json(nullptr);
            answer["store"] = world ? StoreJson() : Json(nullptr);
            return answer;
        });

    // The key bindings as the Options screen's Controls list shows them, read
    // as text: a test of a mod's keys checks them without a picture. Each
    // action is InputMappings.Mappings.<name>, with its Description and the
    // keys as shown, "Ctrl + T", "Enter", "" for none (read in play by
    // turn-based-agent, 2026-10-03, #28); the names come from DAOLib's
    // MemberNames, as the screen's ListMemberNames gives them.
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"controls",
                 "The key bindings, as the Options screen's Controls list shows them: each action's `name`, "
                 "its `description`, and its `primary` and `secondary` keys as shown (\"Ctrl + T\", \"\" for "
                 "none). `match` keeps those whose name or description holds it, in any case. A plugin's own "
                 "actions are among them. Reading only; answers in any mode once the game runs."},
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "inspect");
            if (watch.Mode() < 0)
            {
                throw ToolError("inspect: the game is still starting; health says when it runs");
            }
            const auto lower = [](std::string text) {
                std::ranges::transform(text, text.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return text;
            };
            const std::string match = lower(args.String("match", ""));
            Json controls = Json::array();
            for (const auto &name : dao::data::MemberNames(L"InputMappings.Mappings"))
            {
                std::wstring base = L"InputMappings.Mappings.";
                base += name;
                const auto text = [&base](std::wstring_view field) {
                    std::wstring path = base;
                    path += field;
                    return Narrow(dao::data::Text(path).value_or(L""));
                };
                Json row = {{"name", Narrow(name)},
                            {"description", text(L".Description")},
                            {"primary", text(L".PrimaryMappedKey")},
                            {"secondary", text(L".SecondaryMappedKey")}};
                if (!match.empty() && lower(row["name"].get<std::string>()).find(match) == std::string::npos &&
                    lower(row["description"].get<std::string>()).find(match) == std::string::npos)
                {
                    continue;
                }
                controls.push_back(std::move(row));
            }
            return Json{{"count", controls.size()}, {"controls", std::move(controls)}};
        });
}

} // namespace eluvian::game
