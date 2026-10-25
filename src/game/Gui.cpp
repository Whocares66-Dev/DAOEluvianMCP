// `gui`: what the interface has up, for an agent that cannot
// see it: the game's screens shown (#2), its message box and its loot panel
// (#1). And the loot panel's two buttons, pressed as the panel's own buttons
// do them, for press and the loot tool.
#include "game/Shared.h"

#include "dao/Data.h"
#include "dao/Loot.h"

#include <algorithm>
#include <charconv>

namespace eluvian::game
{

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

// The loot panel is the container part of the inventory movie
// (inventory.gfx, its ActionScript class Container), read by disassembly
// 2026-10-02 (#1). Until #181 Eluvian read it from the data tree
// (GUIInventory.ShowContainer, Container.ID) and sent its buttons' texts as
// the movie sends them: "GUI.SelectedChar.AcquireAllItems <container>" for
// Take All, and for Close "Container.InventoryClosedMessage",
// "GameModeController.PerformUseObject <selected> <container> 2" and
// "GUI.HideFullScreenGUI". DAOLib's dao::loot (#259, 1ef662d, lib-agent-3,
// 2026-10-24) does both as the buttons do, Take All leaving the panel up
// where the backpack filled with items left.
std::optional<ObjectId> LootPanelContainer()
{
    return dao::loot::Container();
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
    return Json{
        {"container", Brief(*container)},
        {"count", items.size()},
        {"items", std::move(items)},
        {"backpack", Backpack()},
        {"buttons", Json::array({Json{{"label", kLootButtons[0]},
                                      {"does", "takes every item into the backpack, then closes the panel, unless "
                                               "the backpack filled with items left"}},
                                 Json{{"label", kLootButtons[1]}, {"does", "closes the panel, taking nothing"}}})}};
}

Json PressLootButton(Bench &bench, std::size_t index)
{
    const auto container = LootPanelContainer();
    if (!container)
    {
        throw ToolError("press: no loot panel is up; order action=use on a container opens one, and "
                        "gui says what is up");
    }
    Json answer = {{"pressed", kLootButtons.at(index)}, {"container", Brief(*container)}};
    // What follows a press is on the bus from here: the game may start a
    // conversation or a plot's step as a container closes.
    answer["cursor"] = bench.Events().Next();
    dao::loot::Result result = dao::loot::Result::NotShown;
    if (index == 0)
    {
        // The items in brief before they move: one that joins a stack in
        // the backpack may be gone after.
        std::map<ObjectId, Json> before;
        for (const ObjectId item : ItemsIn(*container))
        {
            before.emplace(item, ItemJson(item, false));
        }
        result = dao::loot::TakeAll();
        Json left = Json::array();
        for (const ObjectId item : ItemsIn(*container))
        {
            left.push_back(ItemJson(item, false));
            before.erase(item);
        }
        Json taken = Json::array();
        for (auto &[item, json] : before)
        {
            taken.push_back(std::move(json));
        }
        answer["taken"] = std::move(taken);
        answer["left"] = std::move(left);
        answer["full"] = result == dao::loot::Result::BackpackFull;
        answer["backpack"] = Backpack();
    }
    else
    {
        result = dao::loot::Close();
    }
    if (result == dao::loot::Result::NotShown)
    {
        throw ToolError("press: the loot panel was gone before its button was pressed; gui says what is up");
    }
    answer["panel"] = LootPanelJson();
    return answer;
}

void AddGui(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("gui",
             "Returns what the game's interface is showing. Call it when `health` reports mode gui, or when "
             "the game seems to be waiting on something. Works in any mode. The answer includes:\n\n"
             "- `screens`: the screens shown now, top first, each with its `name` (Inventory, Store, "
             "WorldMap, PartyPicker, CharacterRecord, Map, Journal, Skills, Options; Main is the HUD), "
             "`movie` and `layer`.\n"
             "- `fullScreen`: the name of the full-screen screen that is open (inventory, map, journal...), "
             "or null.\n"
             "- `fullScreenHidden`: present when a full-screen screen is still open in the game but not "
             "drawn, e.g. after WorldMap.WorldMapClosed alone. Clear it with `input` "
             "{\"message\": \"GUI.HideFullScreenGUI\"}, as a Close button does.\n"
             "- `popup`: the game's message box, with its text and each button's label and what pressing it "
             "sends. A message box blocks the game until answered, and nothing else reports it, so check "
             "here when a load or an order was accepted but nothing happened.\n"
             "- `loot`: the loot panel a container opens (a chest, remains, a pile of bones), part of the "
             "Inventory screen: its `container`, the `items` in it, the `backpack`'s item count and limit, "
             "and its `buttons`, Take All and Close, with what each does.\n"
             "- `store`: with a merchant's Store screen open, the `stock` with each item's `price`, your "
             "`pack` with what each item `sellsFor`, the `money` (in copper) and the markups.\n\n"
             "To act on these: `press` presses a message box or loot panel button; `loot` walks to a "
             "container, opens it and takes everything in one call; `store` buys and sells.",
             Json::object(), {}),
        [&watch](const Json &) -> ToolResult {
            // DAOLib's ShownScreens and CurrentFullScreen (0dd22bb, #2): a
            // screen is shown while its movie is drawn, and the full-screen
            // one is the last the GUI system pushed. The loot panel is the
            // Inventory screen; dao::loot::Container tells them apart (#181).
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
    bench.Tools().Add(
        Read("controls",
             "Lists the key bindings shown in the Options screen's Controls list: each action's `name`, "
             "`description`, and `primary` and `secondary` keys as displayed (\"Ctrl + T\", or \"\" for "
             "none). Actions added by plugins are included. Read only; works in any mode once the game "
             "has started.",
             Json{{"match",
                   {{"type", "string"},
                    {"description", "Only actions whose name or description contains this text, in any case. "
                                    "Default: all."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "controls");
            if (watch.Mode() < 0)
            {
                throw ToolError("controls: the game is still starting; health says when it runs");
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
