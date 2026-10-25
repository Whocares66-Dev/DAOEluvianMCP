// `store`: buy and sell at the store screen that is up, as its own buttons
// do; and the store as gui reads it. A playthrough driven by
// MCP alone had no health poultices left and gems to sell, and no way to a
// merchant's wares but a mouse (turn-based-agent, 2026-10-03, #9).
//
// Read by disassembly from store.gfx, 2026-10-03, and seen in play the
// same night at Ilen's wares: buying sent "Store.BuyItem <buyer> <item>
// <quantity>" and selling "Store.SellItem <seller> <item> <quantity>", and
// the screen reckons a price from the item's value and the store's
// Store.MarkUp and Store.MarkDown (percent). Since #185 all of it is DAOLib's
// dao::store (#259, 0fa042f, lib-agent-3, 2026-10-25): the store up, its
// stock and the store's own prices, which price a buy-back at what was paid
// where Eluvian's sum from the marks did not, a buy or a sale refused as the
// screen or the store refuses it, and the screen closed.
#include "game/Shared.h"

#include "dao/Data.h"
#include "dao/Store.h"

namespace eluvian::game
{

namespace
{

namespace shop = dao::store;

// The store's percentages, as the screen shows them.
struct Marks
{
    double up = 100;
    double down = 100;
};

Marks MarksNow()
{
    return Marks{dao::data::Number(L"Store.MarkUp").value_or(100), dao::data::Number(L"Store.MarkDown").value_or(100)};
}

// Whose items a list holds, and whether it holds this one.
bool Holds(ObjectId owner, ObjectId item)
{
    const auto items = ItemsIn(owner);
    return std::ranges::find(items, item) != items.end();
}

std::string Refusal(shop::Refusal refusal)
{
    switch (refusal)
    {
    case shop::Refusal::NoStore:
        return "no store screen is up";
    case shop::Refusal::NotOffered:
        return "the item is not on offer: not in the store's stock to buy, or not in the party's pack to sell";
    case shop::Refusal::BadCount:
        return "the count is below 1";
    case shop::Refusal::PlotItem:
        return "it is an item the story needs, which the screen never offers for sale";
    case shop::Refusal::TooExpensive:
        return "the party has not the money";
    case shop::Refusal::BackpackFull:
        return "the party's backpack has no room, and the store kept the item";
    case shop::Refusal::StoreWontBuy:
        return "the store buys no such item, has not the money, or pays nothing for it";
    default:
        return "refused";
    }
}

} // namespace

std::optional<ObjectId> StoreUp()
{
    return shop::Current();
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
    for (const shop::Entry &entry : shop::Stock())
    {
        Json json = ItemJson(entry.item, false);
        json["price"] = entry.price;
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
        json["sellsFor"] = shop::SellPrice(item);
        pack.push_back(std::move(json));
    }
    return Json{{"store", Brief(*store)}, {"name", Narrow(dao::data::Text(L"Store.Name").value_or(L""))},
                {"markUp", marks.up},     {"markDown", marks.down},
                {"money", shop::Money()}, {"stock", std::move(stock)},
                {"pack", std::move(pack)}};
}

void AddStore(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"store",
                 "Buys or sells an item at the store screen that is open, or closes it. Open a store by talking "
                 "to a merchant and asking to see the wares.\n\n"
                 "While a store is open, `gui` reports `fullScreen` Store, and its `store` lists the `stock` with "
                 "each item's `price`, your `pack` with what each item `sellsFor`, and your `money`, all in "
                 "copper, as the store itself reckons them.\n\n"
                 "`action` is one of:\n"
                 "- buy: buys `item` from the stock.\n"
                 "- sell: sells `item` from your pack.\n"
                 "- close: closes the store, as its screen's close does. An item sold can be bought back at what "
                 "was paid for it only until then.\n\n"
                 "A buy or sale goes as the screen's own does, and one the screen or the store refuses is an "
                 "error saying why: not in the stock or pack, a plot item, not enough money, a full backpack, or "
                 "a store that will not buy it. The answer includes what the `count` cost or fetched (`total`), "
                 "and the `money` before and after.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"buy", "sell", "close"}},
                          {"description", "Buy, sell, or close the store."}}},
                        {"item",
                         {{"type", "string"},
                          {"description",
                           "With buy or sell: the item, by its id from the `stock` or `pack` that `gui` lists."}}},
                        {"count",
                         {{"type", "integer"},
                          {"minimum", 1},
                          {"description", "How many of a stack to buy or sell, up to 999. Default 1."}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "store");
            RequireWorld(watch, "store");
            const auto action = args.Choice("action", {"buy", "sell", "close"});
            const auto store = StoreUp();
            if (!store)
            {
                throw ToolError("store: no store screen is up; talk to a merchant and choose to see the wares "
                                "(gui says `fullScreen` Store when it is)");
            }
            Json answer = {{"action", action}, {"store", Brief(*store)}, {"cursor", bench.Events().Next()}};
            if (action == "close")
            {
                shop::Close();
                answer["closed"] = !StoreUp().has_value();
                return answer;
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
            answer["item"] = ItemJson(item, false);
            answer["count"] = count;
            answer["total"] = buying ? shop::BuyPrice(item, count) : shop::SellPrice(item, count);
            const std::int32_t before = shop::Money();
            const shop::Refusal refusal = buying ? shop::Buy(item, count) : shop::Sell(item, count);
            if (refusal != shop::Refusal::None)
            {
                throw ToolError(
                    std::format("store: {} refused: {}", buying ? "the buy was" : "the sale was", Refusal(refusal)));
            }
            answer["money"] = Json{{"before", before}, {"after", shop::Money()}};
            return answer;
        });
}

} // namespace eluvian::game
