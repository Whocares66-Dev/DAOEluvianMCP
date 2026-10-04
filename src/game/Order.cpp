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
                throw ToolError(std::format("order: {} carries no item tagged \"{}\"; inspect kind=inventory lists "
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
        throw ToolError("order: a click needs explore or combat mode; inspect kind=game says the mode");
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

void AddOrder(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"order",
                 "Give an order as the player does, by id. `action`: attack `target`; move to `point`; ability "
                 "`ability` (an id from the ability table) on `target` or `point`; use `target` (a placeable: a "
                 "door, a chest, a lever); control, make `id`, a party member, the creature controlled, as a "
                 "click on its portrait does (out of combat a follower takes an order by command poorly; "
                 "control it, then order it by click). "
                 "`via`: \"command\" (the default, but for use by the controlled creature) gives it through "
                 "the engine's own click functions, so the "
                 "command is player-issued and anything watching the command queue sees one; any creature "
                 "`id` may obey. \"click\" sends explore mode's click message instead (the primary action on the "
                 "target as if the cursor were over it; an ability as the quickbar uses it, with `target` "
                 "selected), so the click's own handling runs too, and whatever hooks it (a mod watching clicks, "
                 "asking for an ability's target); "
                 "it is the controlled creature's, and cannot move. (Seen 2026-09-30: an ability by click works, the "
                 "mod asking for its target when none is given. An attack or use by click is the primary action "
                 "of every party member selected in the interface, and its answer cannot say whether anyone "
                 "obeyed: the events from `cursor` do.) A use goes by click when the controlled creature is to "
                 "do it, since a click chooses what the object takes (a chest and a door wanted open, and did "
                 "nothing for use, their command ending COMMAND_FAILED); by command it does `objectAction`, "
                 "default use. An area ability with a range (Fireball; inspect kind=abilities says each one's "
                 "`aoe` and `range`) given a creature `target` is aimed at its position, as a ground cursor on "
                 "it (`aimedAt`; `atCreature`: true keeps the creature). Answers whether the order was taken, and "
                 "`cursor`: wait_for from it for what came of it (eluvian.command.started and ended, and a mod's "
                 "own events).",
                 Json{{"type", "object"},
                      {"properties",
                       {{"action", {{"type", "string"}, {"enum", {"attack", "move", "ability", "use", "control"}}}},
                        {"id", {{"description", "Who obeys: a creature id; default the one controlled"}}},
                        {"atCreature",
                         {{"type", "boolean"},
                          {"description", "action=ability: keep a creature as an area ability's target, not its "
                                          "position"}}},
                        {"target", {{"description", "A creature or object id"}}},
                        {"point", {{"description", "{x, y, z} or [x, y, z]"}}},
                        {"ability", {{"type", "integer"}}},
                        {"item", {{"description", "For an ability from an item: the item's id"}}},
                        {"itemTag",
                         {{"type", "string"},
                          {"description", "For an ability from an item: the item's tag, found on the creature "
                                          "(gen_im_qck_health_101, a lesser health poultice)"}}},
                        {"via",
                         {{"type", "string"},
                          {"enum", {"command", "click"}},
                          {"description", "command (default): the engine's click functions. click: explore "
                                          "mode's click message (the default for use by the controlled "
                                          "creature)"}}},
                        {"objectAction",
                         {{"type", "string"},
                          {"description", "action=use by command: what to do with the object: open, close, "
                                          "unlock, use (default), examine, disarm, dialog, open_inventory, "
                                          "trigger_trap, topple, area_transition, and the rest of "
                                          "PLACEABLE_ACTION_*. It must be the one the object takes as it "
                                          "is, or the command ends COMMAND_FAILED: a locked chest (state "
                                          "1) took unlock, then open_inventory; a door open"}}}}},
                      {"required", {"action"}}},
                 Thread::Frame},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "order");
            const auto action = args.Choice("action", {"attack", "move", "ability", "use", "control"});
            RequireWorld(watch, "order");
            const ObjectId controlled = api::GetMainControlled();
            if (action == "control")
            {
                // The scripts' SetPrimaryControlled, what the game's own
                // scripts use to hand control to a follower. A follower given
                // an order by command out of combat took none, and the
                // selection's binding left the hero controlled
                // (turn-based-agent, 2026-10-03, #22).
                const ObjectId member = args.Id("id", "a party member, as inspect kind=game lists the party");
                if (!PartyOf(api::GetHero()).contains(member))
                {
                    throw ToolError(
                        std::format("order: {} is not in the party; inspect kind=game lists it", FormatId(member)));
                }
                api::SetPrimaryControlled(member);
                const ObjectId now = api::GetMainControlled();
                return Json{{"action", action},
                            {"before", Brief(controlled)},
                            {"controlled", Brief(now)},
                            {"taken", now == member},
                            {"cursor", bench.Events().Next()}};
            }
            const ObjectId who = args.OptionalId("id", kCreatureHint).value_or(controlled);
            if (who == kInvalidObject)
            {
                throw ToolError("order: nobody is controlled and no `id` was given");
            }
            const bool useByClick = action == "use" && who == controlled && !args.Has("objectAction");
            const auto via = args.Choice("via", {"command", "click"}, useByClick ? "click" : "command");
            if (via == "click" && who != controlled)
            {
                throw ToolError(std::format("order: a click is the controlled creature's ({}); via \"command\" "
                                            "orders any creature",
                                            controlled == kInvalidObject ? "nobody" : FormatId(controlled)));
            }
            const auto cursor = bench.Events().Next();
            Json answer = {{"action", action}, {"via", via}, {"creature", Brief(who)}};
            const bool taken =
                via == "click" ? OrderByClick(args, action, who, answer) : OrderByCommand(args, action, who, answer);
            if (!taken)
            {
                throw ToolError(via == "click"
                                    ? "order: explore mode did not take the ability's click"
                                    : "order: the game took no order: the creature is not there, or (for move) "
                                      "the game is not in explore or combat mode");
            }
            answer["given"] = true;
            answer["cursor"] = cursor;
            return answer;
        });
}

} // namespace eluvian::game
