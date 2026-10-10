// `order`: the player's orders by id (attack, move, ability, use), through
// the engine's own click functions or explore mode's click message.
#include "game/Shared.h"

namespace eluvian::game
{

namespace
{

std::int32_t AbilityArg(const Args &args)
{
    const auto ability =
        static_cast<std::int32_t>(args.Integer("ability", -1, 0, std::numeric_limits<std::int32_t>::max()));
    if (ability < 0)
    {
        throw ToolError("order: `ability` is the ability's id (the ABI_* row in the ability table)");
    }
    return ability;
}

// Through the engine's own click functions (dao/Orders.h): the command
// player-issued, queued as a click queues it. Any creature may obey.
// PLACEABLE_ACTION_* from 1 (script.ldf), by the names a tester writes.
constexpr std::array<std::string_view, 17> kPlaceableActionNames = {
    "open",      "close",  "unlock",         "destroy",      "use",    "examine",     "disarm",          "flip_cover",
    "use_cover", "dialog", "open_inventory", "trigger_trap", "topple", "leave_cover", "area_transition", "turn_left",
    "turn_right"};

bool OrderByCommand(const Args &args, const std::string &action, ObjectId who, Json &answer)
{
    if (action == "attack")
    {
        const ObjectId target = args.Id("target", kCreatureHint);
        answer["target"] = Brief(target);
        return dao::orders::Attack(who, target);
    }
    if (action == "move")
    {
        const auto point = VectorOf(args.Raw().value("point", Json()), "order: `point`");
        answer["point"] = VectorJson(point);
        return dao::orders::MoveTo(who, point);
    }
    if (action == "ability")
    {
        const auto ability = AbilityArg(args);
        const ObjectId target = args.OptionalId("target", kCreatureHint).value_or(kInvalidObject);
        const auto point = args.Has("point") ? VectorOf(args.Raw().at("point"), "order: `point`") : dao::Vector{};
        ObjectId item = args.OptionalId("item", "an item").value_or(kInvalidObject);
        // An item named by its tag, found on the creature: a potion's id
        // changes each launch, its tag does not (turn-based-agent, 2026-10-02).
        if (item == kInvalidObject && args.Has("itemTag"))
        {
            const std::wstring tag = Wide(args.String("itemTag"));
            const auto items = api::GetItemsInInventory(who, k::GET_ITEMS_OPTION_ALL);
            for (std::size_t i = 0; i < items.Size() && item == kInvalidObject; ++i)
            {
                if (api::GetTag(items.Object(i)) == tag)
                {
                    item = items.Object(i);
                }
            }
            if (item == kInvalidObject)
            {
                throw ToolError(std::format("order: {} carries no item tagged \"{}\"; inventory lists "
                                            "what it has",
                                            FormatId(who), args.String("itemTag")));
            }
        }
        answer["ability"] = AbilityJson(ability);
        if (item != kInvalidObject)
        {
            answer["item"] = Brief(item);
        }
        answer["target"] = Brief(target);
        // An area aimed at a place, ordered at a creature: at its feet, as a
        // player's ground cursor on it. Ordered at the creature the caster
        // set out to reach it, and a path that wanted an action failed the
        // order every time (turn-based-agent, 2026-10-03, #35).
        if (target != kInvalidObject && !args.Has("point") && !args.Bool("atCreature", false) &&
            item == kInvalidObject && AimedAtAPlace(ability))
        {
            const auto at = api::GetPosition(target);
            answer["aimedAt"] = Json{{"point", VectorJson(at)}, {"of", Brief(target)}};
            return dao::orders::UseAbility(who, ability, kInvalidObject, item, at);
        }
        return dao::orders::UseAbility(who, ability, target, item, point);
    }
    // The command carries one PLACEABLE_ACTION, and the object does nothing
    // for one it does not take: USE left a chest and a door in the Elven
    // Ruins shut, the command ending COMMAND_FAILED, where a click (which
    // chooses the object's own action) opened both (computer-use-agent,
    // then here, 2026-10-01). So the action is the caller's to name.
    const ObjectId target = args.Id("target", "a placeable or other object to use");
    answer["target"] = Brief(target);
    const auto name = args.String("objectAction", "use");
    const auto found = std::ranges::find(kPlaceableActionNames, name);
    if (found == kPlaceableActionNames.end())
    {
        throw ToolError(std::format("order: no `objectAction` \"{}\"; open, close, unlock, use, examine, disarm, "
                                    "dialog, open_inventory, trigger_trap, topple and area_transition are the "
                                    "usual ones",
                                    name));
    }
    answer["objectAction"] = name;
    return dao::orders::UseObject(who, target,
                                  static_cast<dao::script::PlaceableAction>(found - kPlaceableActionNames.begin() + 1));
}

// Through explore mode's own click messages, so the click's front half runs
// too: what turnbased's order watch and talent targeting hook (DAOLib
// 9ea5e2c). A click is the controlled creature's. The object under the
// cursor is set and the message sent in the same frame, before the mode
// picks again from the real cursor.
bool OrderByClick(const Args &args, const std::string &action, ObjectId who, Json &answer)
{
    auto *explore = dao::CClientExoApp::CurrentExplore();
    if (explore == nullptr)
    {
        throw ToolError("order: a click needs explore or combat mode; state says the mode");
    }
    if (action == "move")
    {
        throw ToolError("order: a move cannot be clicked yet (the cursor's ground hit has no safe setter); via "
                        "\"command\" moves as the click's own handler does");
    }
    if (action == "ability")
    {
        const auto ability = AbilityArg(args);
        const auto target = args.OptionalId("target", kCreatureHint);
        answer["ability"] = AbilityJson(ability);
        answer["target"] = target ? Brief(*target) : Json(nullptr);
        if (target)
        {
            explore->SelectTarget(*target); // as the cycle-target keys select it
        }
        auto message = dao::messages::Make<dao::messages::PerformAbilityMessage>();
        message.ability = ability;
        message.caster = who;
        message.target = target.value_or(kInvalidObject);
        return dao::messages::Send(explore, message);
    }
    // attack and use: the click's primary action on the object under the
    // cursor (attack an enemy, use a placeable, talk to a friend). Its
    // handler (0x007132B0) is a void function, so Send's answer is a leftover
    // register and says nothing; and it orders an attack from every party
    // member selected in the interface, not the controlled creature alone
    // (lib-agent, 2026-09-30, after a click in combat queued nothing). What
    // came of it is in the events: eluvian.command.started, and the mod's.
    const ObjectId target = args.Id("target", "the creature or object to click on");
    answer["target"] = Brief(target);
    explore->SetCursorObject(target);
    dao::messages::Send(explore, dao::messages::Make<dao::messages::PerformPrimaryActionOnTargetUnderCursorMessage>());
    Json selected = Json::array();
    for (const ObjectId member : dao::party::Selected())
    {
        selected.push_back(Brief(member));
    }
    answer["selected"] = std::move(selected);
    answer["note"] = "a click's primary action orders the party members selected (`selected`, the party bar's; the "
                     "engine's own list may differ); eluvian.command.started from `cursor` says who obeyed";
    return true;
}

} // namespace

namespace
{

// The control tool: the scripts' own hand-over to a party member.
Json ToolControl(Bench &bench, const Watch &watch, const Json &raw)
{
    const Args args(raw, "control");
    RequireWorld(watch, "control");
    const ObjectId controlled = api::GetMainControlled();
    // The scripts' SetPrimaryControlled, what the game's own
    // scripts use to hand control to a follower. A follower given
    // an order by command out of combat took none, and the
    // selection's binding left the hero controlled
    // (turn-based-agent, 2026-10-03, #22).
    const ObjectId member = args.Id("id", "a party member, as state lists the party");
    if (!PartyOf(api::GetHero()).contains(member))
    {
        throw ToolError(std::format("control: {} is not in the party; state lists it", FormatId(member)));
    }
    api::SetPrimaryControlled(member);
    const ObjectId now = api::GetMainControlled();
    return Json{{"before", Brief(controlled)},
                {"controlled", Brief(now)},
                {"taken", now == member},
                {"cursor", bench.Events().Next()}};
}

// Control handed to a party member before its move, as a click on its
// portrait: whom it was before.
Json HandOver(ObjectId who, ObjectId controlled)
{
    if (!PartyOf(api::GetHero()).contains(who))
    {
        throw ToolError(std::format("order: a move, or a use or item's ability out of combat, is the "
                                    "controlled creature's ({}), not {}'s: control is handed over to a "
                                    "party member first, and {} is not in the party (via \"command\" "
                                    "orders any creature to use or cast)",
                                    controlled == kInvalidObject ? "nobody" : FormatId(controlled), FormatId(who),
                                    FormatId(who)));
    }
    api::SetPrimaryControlled(who);
    if (api::GetMainControlled() != who)
    {
        throw ToolError(std::format("order: {} would not take control, and the order is the controlled "
                                    "creature's; nothing was ordered",
                                    FormatId(who)));
    }
    return Brief(controlled);
}

// A door's use toggles it, and a second use sent unseen shut the
// door the first had opened (turn-based-agent, 2026-10-05, #68):
// the state it is in as the use is ordered, read before the use
// reaches it.
void SayDoorWas(const Args &args, Json &answer)
{
    if (const auto target = args.OptionalId("target", "a placeable or other object to use");
        target && api::GetObjectType(*target) == k::OBJECT_TYPE_PLACEABLE &&
        PlaceableController(*target) == "StateCnt_Door")
    {
        answer["doorWas"] = PlaceableStateWord(*target);
        if (answer["doorWas"] == "open")
        {
            answer["note"] = "the door was open: this use shuts it (placeables says a "
                             "door's state)";
        }
    }
}

// Who carries out an order, and how: by click, the controlled creature's,
// or by command, any creature's.
struct Acting
{
    Json handedOver;          // whom control was taken from, if it was handed over
    std::string via;          // "click" or "command"
    bool itemByClick = false; // an item's ability out of combat, by click
};

Acting WhoActs(const Args &args, const std::string &action, ObjectId who, ObjectId controlled, const Watch &watch)
{
    const bool explore = watch.Mode() == static_cast<std::int32_t>(dao::GameMode::Explore);
    const bool item = action == "ability" && (args.Has("item") || args.Has("itemTag"));
    // A move goes to the controlled creature, whatever `id` says: it is
    // explore mode's own move click (GameModeExplore::MoveTo), which moves
    // whom the player controls. Ordered for a follower in the Gauntlet, the
    // hero walked there instead, and the answer named the follower
    // (turn-based-agent, 2026-10-04, #48). A script's move would reach it,
    // but the party's following takes a follower back to the leader once it
    // ends. So another party member is given control first, as a click on
    // its portrait, and keeps it; judged at the moment of the order, so a
    // fight's member on its turn, controlled already, is moved as before.
    // A use or an item's ability out of combat is a click too, and for
    // another party member control is handed over first the same way, where
    // the driver gave `order control` before and after by hand
    // (turn-based-agent's survey, 2026-10-06, #86). In a fight the command
    // path works and stays.
    const bool clicked = (action == "use" && !args.Has("objectAction")) || (item && explore);
    Acting acts;
    if (who != controlled && (action == "move" || (clicked && explore && !args.Has("via"))))
    {
        acts.handedOver = HandOver(who, controlled);
    }
    const ObjectId acting = acts.handedOver.is_null() ? controlled : who;
    const bool useByClick = action == "use" && who == acting && !args.Has("objectAction");
    // An item's ability out of combat by command takes nothing: Kolgrim's
    // Horn ordered so three times on the mountain top answered given and
    // stayed in the pack, its script never run; by click it was used at once
    // (turn-based-agent, 2026-10-04, #51). In fights the command path works
    // (poultices, the Litany), and stays.
    acts.itemByClick = item && who == acting && explore;
    acts.via = args.Choice("via", {"command", "click"}, useByClick || acts.itemByClick ? "click" : "command");
    if (acts.via == "click" && who != acting)
    {
        throw ToolError(std::format("order: a click is the controlled creature's ({}); via \"command\" "
                                    "orders any creature",
                                    controlled == kInvalidObject ? "nobody" : FormatId(controlled)));
    }
    return acts;
}

// The order tool's handler, registered by AddOrder.
ToolResult ToolOrder(Bench &bench, const Watch &watch, const Json &raw)
{
    const Args args(raw, "order");
    const auto action = args.Choice("action", {"attack", "move", "ability", "use"});
    RequireWorld(watch, "order");
    const ObjectId controlled = api::GetMainControlled();
    const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(controlled);
    if (who == kInvalidObject)
    {
        throw ToolError("order: nobody is controlled and no `id` was given");
    }
    const auto acts = WhoActs(args, action, who, controlled, watch);
    const auto cursor = bench.Events().Next();
    Json answer = {{"action", action}, {"via", acts.via}, {"creature", Brief(who)}};
    if (!acts.handedOver.is_null())
    {
        answer["controlled"] = Brief(who);
        answer["before"] = acts.handedOver;
        answer["note"] = action == "move" ? "a move is the player's move click, which moves whom the player controls: "
                                            "control passed to the creature first, and stays with it (order "
                                            "action=control gives it back)"
                                          : "out of combat a use or an item's ability is a click, the controlled "
                                            "creature's: control passed to the creature first, and stays with it while "
                                            "it acts (order action=control gives it back)";
    }
    if (action == "use")
    {
        SayDoorWas(args, answer);
    }
    const bool taken =
        acts.via == "click" ? OrderByClick(args, action, who, answer) : OrderByCommand(args, action, who, answer);
    if (!taken)
    {
        throw ToolError(acts.via == "click" ? "order: explore mode did not take the ability's click"
                                            : "order: the game took no order: the creature is not there, or (for move) "
                                              "the game is not in explore or combat mode");
    }
    if (acts.itemByClick && !args.Has("via"))
    {
        answer["note"] = "an item's ability out of combat goes by click: by command it takes nothing there";
    }
    answer["given"] = true;
    answer["cursor"] = cursor;
    return answer;
}

} // namespace

void AddOrder(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"order",
                 "Gives an order to a creature by id, as the player would. `action`:\n\n"
                 "- `attack`: attack `target`.\n"
                 "- `move`: walk to `point`. Only the creature you control can be moved this way, so another "
                 "party member `id` is given control first and keeps it (the answer says `controlled` and "
                 "`before`). Creatures outside the party cannot be moved.\n"
                 "- `ability`: use `ability` (an id from `abilities`) on `target` or at `point`. For an item's "
                 "ability, such as a potion's, also pass `item` or `itemTag`. An area ability with a range (such "
                 "as Fireball; `abilities` gives each one's `aoe` and `range`) aimed at a creature is aimed at the "
                 "ground under it (`aimedAt`), unless `atCreature` is true.\n"
                 "- `use`: use the object `target`, such as a door, chest or lever. Using a door toggles it; "
                 "`doorWas` says whether it was open.\n\n"
                 "How the order is sent (`via`):\n"
                 "- `command`: through the engine's own order functions. The order counts as the player's, and "
                 "anything watching the command queue sees it. Any creature can obey. A use by command does "
                 "`objectAction`.\n"
                 "- `click`: as if the player clicked, so anything hooked to clicks also runs (for example a mod "
                 "that asks for an ability's target when none is given). Only the creature you control can click, "
                 "only in explore or combat mode, and a move cannot be clicked. An attack or use by click is "
                 "carried out by every party member selected in the party bar (`selected`), and the answer cannot "
                 "say who obeyed: check the events from `cursor`.\n\n"
                 "`via` defaults to `command`, except: a use without `objectAction` by the creature you control "
                 "goes by click, since a click picks the object's own action (a plain use by command can leave a "
                 "chest or door shut). Out of combat, an item's ability by the creature you control also goes by "
                 "click, since by command it does nothing there. Out of combat, when `id` is another party member, "
                 "control is handed to it first for these, as for a move, and stays with it.\n\n"
                 "The answer says the order was `given` and includes a `cursor`; `wait_for` from it shows what "
                 "happened (eluvian.command.started and eluvian.command.ended, and a mod's own events).",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action",
                         {{"type", "string"},
                          {"enum", {"attack", "move", "ability", "use"}},
                          {"description", "What to do."}}},
                        {"id", {{"description", "Who obeys, by creature id. Default: the creature you control."}}},
                        {"atCreature",
                         {{"type", "boolean"},
                          {"description", "With `ability`: aim an area ability at the creature itself, not the "
                                          "ground under it. Default false."}}},
                        {"target", {{"description", "The creature or object to act on, by id."}}},
                        {"point", {{"description", "With `move` or `ability`: the point, {x, y, z} or [x, y, z]."}}},
                        {"ability", {{"type", "integer"}, {"description", "The ability's id, from `abilities`."}}},
                        {"item", {{"description", "For an item's ability: the item's id."}}},
                        {"itemTag",
                         {{"type", "string"},
                          {"description", "For an item's ability: the item's tag, looked up in the creature's "
                                          "inventory (for example gen_im_qck_health_101, a lesser health "
                                          "poultice)."}}},
                        {"via",
                         {{"type", "string"},
                          {"enum", {"command", "click"}},
                          {"description", "How the order is sent. Default command, but click for a use without "
                                          "`objectAction`, or an item's ability out of combat, by the creature "
                                          "you control."}}},
                        {"objectAction",
                         {{"type", "string"},
                          {"description", "With `use` by command: what to do with the object, a PLACEABLE_ACTION_* "
                                          "name such as open, close, unlock, use, examine, disarm, dialog, "
                                          "open_inventory, trigger_trap, topple or area_transition. Default use. "
                                          "It must be an action the object accepts in its current state, or the "
                                          "command ends with COMMAND_FAILED: a locked chest takes unlock, then "
                                          "open_inventory."}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult { return ToolOrder(bench, watch, raw); });
    bench.Tools().Add(
        Spec("control",
             "Gives control to a party member, like clicking its portrait. Returns who was controlled `before`, "
             "who is `controlled` now, whether the change worked (`taken`), and a `cursor`. `order` hands control "
             "over by itself when another party member has to move or use something.",
             Json{{"id", {{"type", "string"}, {"description", "The party member, by id; `state` lists the party."}}}},
             {"id"}),
        [&bench, &watch](const Json &raw) -> ToolResult { return ToolControl(bench, watch, raw); });
}

} // namespace eluvian::game
