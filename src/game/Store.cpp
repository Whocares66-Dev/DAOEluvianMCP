// `store`: buy and sell at the store screen that is up, as its own buttons
// do; and the store as gui reads it. A playthrough driven by
// MCP alone had no health poultices left and gems to sell, and no way to a
// merchant's wares but a mouse (turn-based-agent, 2026-10-03, #9).
//
// Read by disassembly from store.gfx, 2026-10-03, and seen in play the
// same night at Ilen's wares: buying sends "Store.BuyItem <buyer> <item>
// <quantity>" and selling "Store.SellItem <seller> <item> <quantity>", the
// buyer or seller the screen's m_oidPlayer, which it reads from
// GUI.SelectedChar.ID as it opens, every id in decimal. With the screen up
// the data tree has the store object (Store.ID, whose stock its inventory
// is), Store.Name, Store.MarkUp and Store.MarkDown (percent) and the
// party's money (Party.Money.Total, in copper). The screen reckons a sell
// value as floor(cost x MarkDown / 100), and a price as cost x MarkUp /
// 100: a Greenstone of value 400 sold for 100 at 25, a poultice of value
// 100 cost 100 at 100.
#include "game/Shared.h"

#include "dao/Data.h"

namespace eluvian::game
{

namespace
{

double Money()
{
    return dao::data::Number(L"Party.Money.Total").value_or(0);
}

// The store's percentages, as the screen reads them.
struct Marks
{
    double up = 100;
    double down = 100;
};

Marks MarksNow()
{
    return Marks{dao::data::Number(L"Store.MarkUp").value_or(100), dao::data::Number(L"Store.MarkDown").value_or(100)};
}

// What one of an item costs at the store, or fetches there, in copper.
double Price(ObjectId item, double mark)
{
    return std::floor(static_cast<double>(api::GetItemValue(item)) * mark / 100.0);
}

// Whose items a list holds, and whether it holds this one.
bool Holds(ObjectId owner, ObjectId item)
{
    const auto items = ItemsIn(owner);
    return std::ranges::find(items, item) != items.end();
}

} // namespace

std::optional<ObjectId> StoreUp()
{
    return TreeId(L"Store.ID");
}

Json StoreJson()
{
    const auto store = StoreUp();
    if (!store)
    {
        return nullptr;
    }
    const auto marks = MarksNow();
    Json stock = Json::array();
    for (const ObjectId item : ItemsIn(*store))
    {
        Json json = ItemJson(item, false);
        json["price"] = Price(item, marks.up);
        stock.push_back(std::move(json));
    }
    Json pack = Json::array();
    for (const ObjectId item : ItemsIn(api::GetHero()))
    {
        if (api::IsPlot(item) != 0)
        {
            continue; // the game's plot items are not for sale
        }
        Json json = ItemJson(item, false);
        json["sellsFor"] = Price(item, marks.down);
        pack.push_back(std::move(json));
    }
    return Json{{"store", Brief(*store)}, {"name", Narrow(dao::data::Text(L"Store.Name").value_or(L""))},
                {"markUp", marks.up},     {"markDown", marks.down},
                {"money", Money()},       {"stock", std::move(stock)},
                {"pack", std::move(pack)}};
}

void AddStore(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"store",
                 "Buys or sells an item at the store screen that is open. Open a store by talking to a merchant "
                 "and asking to see the wares.\n\n"
                 "While a store is open, `gui` reports `fullScreen` Store, and its `store` lists the `stock` with "
                 "each item's `price`, your `pack` with what each item `sellsFor`, and your `money`, all in "
                 "copper.\n\n"
                 "`action` is one of:\n"
                 "- buy: buys `item` from the stock.\n"
                 "- sell: sells `item` from your pack.\n\n"
                 "The tool sends what the screen's own button sends, and the game does the rest; it refuses a "
                 "purchase you cannot afford. The answer includes the price of one (`each`), the `money` before "
                 "and after, and `taken`, which is false when the money did not change. To close the store, "
                 "call `input` with {\"message\": \"GUI.HideFullScreenGUI\"}.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"buy", "sell"}}, {"description", "Buy or sell."}}},
                        {"item",
                         {{"type", "string"},
                          {"description", "The item, by its id from the `stock` or `pack` that `gui` lists."}}},
                        {"count",
                         {{"type", "integer"},
                          {"minimum", 1},
                          {"description", "How many of a stack to buy or sell, up to 999. Default 1."}}}}},
                      {"required", {"action", "item"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "store");
            RequireWorld(watch, "store");
            const auto action = args.Choice("action", {"buy", "sell"});
            const auto store = StoreUp();
            if (!store)
            {
                throw ToolError("store: no store screen is up; talk to a merchant and choose to see the wares "
                                "(gui says `fullScreen` Store when it is)");
            }
            const ObjectId item = args.Id("item", "an item, from gui's store");
            const auto count = static_cast<std::int32_t>(args.Integer("count", 1, 1, 999));
            const bool buying = action == "buy";
            const ObjectId owner = buying ? *store : api::GetHero();
            if (!Holds(owner, item))
            {
                throw ToolError(std::format("store: {} is not in the {}; gui's store lists the {}", FormatId(item),
                                            buying ? "store's stock" : "party's pack", buying ? "`stock`" : "`pack`"));
            }
            const auto marks = MarksNow();
            Json answer = {{"action", action},
                           {"item", ItemJson(item, false)},
                           {"count", count},
                           {"each", Price(item, buying ? marks.up : marks.down)},
                           {"cursor", bench.Events().Next()}};
            const double before = Money();
            const auto text =
                std::format(L"Store.{} {} {} {}", buying ? L"BuyItem" : L"SellItem", Selected(), item, count);
            if (!dao::messages::SendText(text))
            {
                throw ToolError("store: the game took no " + Narrow(text));
            }
            const double after = Money();
            answer["sent"] = Narrow(text);
            answer["money"] = Json{{"before", before}, {"after", after}};
            answer["taken"] = after != before;
            return answer;
        });
}

} // namespace eluvian::game
