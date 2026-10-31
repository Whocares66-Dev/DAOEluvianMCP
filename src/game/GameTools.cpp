#include "game/GameTools.h"

#include "game/Shared.h"

#include "dao/Animation.h"
#include "dao/Audio.h"
#include "dao/Creatures.h"
#include "dao/Equipment.h"
#include "dao/GameModes.h"
#include "dao/Party.h"

#include <unordered_map>

namespace eluvian::game
{

namespace
{

struct ScriptEventTopic
{
    dao::script::EventType type;
    std::string_view name;
};

// The script events a test waits on, by the name of their topic,
// eluvian.script.<name>. Not heartbeats and perception, which come by the
// dozen a second and say little; nor EVENT_TYPE_COMMAND_COMPLETE, the most
// frequent of all in play (2,094 of a session's 12,743 events, 2026-09-30),
// which eluvian.command.ended says better: who, what, and how it ended.
// The animations table, anim_base, anim_combat and anim_placeables merged
// (DAOLib's dao/core/Anb.h: table 0).
constexpr std::int32_t kTableAnimations = 0;

constexpr ScriptEventTopic kScriptEvents[] = {
    {dao::script::EventType::Damaged, "damaged"},
    {dao::script::EventType::Death, "death"},
    {dao::script::EventType::AttackImpact, "attack_impact"},
    {dao::script::EventType::CombatInitiated, "combat_initiated"},
    {dao::script::EventType::AbilityCastStart, "ability_cast_start"},
    {dao::script::EventType::AbilityCastImpact, "ability_cast_impact"},
    {dao::script::EventType::ApplyEffect, "apply_effect"},
    {dao::script::EventType::RemoveEffect, "remove_effect"},
    {dao::script::EventType::GamemodeChange, "gamemode_change"},
    {dao::script::EventType::ArealoadPostloadexit, "area_loaded"},
    {dao::script::EventType::PartymemberAdded, "partymember_added"},
    {dao::script::EventType::OnOrderReceived, "order_received"},
    {dao::script::EventType::PopupResult, "popup_result"},
};

} // namespace

void AddGameTools(Bench &bench, const Watch &watch)
{
    AddInspect(bench, watch);
    AddCharacter(bench, watch);
    AddGui(bench, watch);
    AddStore(bench, watch);
    AddWorldMap(bench, watch);
    AddTactics(bench, watch);
    AddBoard(bench, watch);
    AddLevelUp(bench, watch);
    AddPartyPicker(bench);
    AddCrafting(bench, watch);
    AddCharGen(bench);
    AddOptions(bench);
    AddScript(bench, watch);
    AddOrder(bench, watch);
    AddStage(bench, watch);
    AddScreenshot(bench, watch);
    AddGame(bench, watch);
    AddDialogue(bench, watch);
    AddCamera(bench, watch);
    AddInput(bench, watch);
    AddScreen(bench, watch);
    AddResources(bench);
    AddWarpArea(bench);
    AddPeek(bench);
    AddSnapshot(bench, watch);
    AddFrameCapture(bench);
}

namespace
{

// A creature's, not a placeable's or a trigger's: their commands are the
// world's own business and would fill the buffer.
bool IsCreature(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    auto *object = app != nullptr ? app->GetGameObjectById(id) : nullptr;
    return object != nullptr && object->AsCreature() != nullptr;
}

// A script event as the bus carries it. `receiver` is the object the event
// is delivered to, which is not always the one it is about: an attack's
// impact goes to the attacker (seen in play, 2026-09-30, Aedan's impact
// delivered to Aedan). So the event's own parameters come too, as the
// scripts read them with GetEventObject, GetEventInteger and GetEventFloat:
// the first four of each, without the defaults that trail (-1, 0, 0.0),
// which is what reading past the end gives.
Json ScriptEventPayload(ObjectId receiver, const dao::CScriptEvent &event)
{
    constexpr std::int32_t kRead = 4;
    Json objects = Json::array();
    Json integers = Json::array();
    Json floats = Json::array();
    for (std::int32_t i = 0; i < kRead; ++i)
    {
        objects.push_back(IdJson(event.Object(i)));
        integers.push_back(event.Integer(i));
        floats.push_back(event.Float(i));
    }
    const auto trim = [](Json &list, const Json &fallback) {
        while (!list.empty() && list.back() == fallback)
        {
            list.erase(list.end() - 1);
        }
    };
    trim(objects, Json(nullptr));
    trim(integers, Json(0));
    trim(floats, Json(0.0F));
    Json payload = {{"receiver", IdJson(receiver)}, {"creator", IdJson(event.Creator())}};
    // The parameters' meanings, from the game's own rules_core.nss
    // (lib-agent, 2026-09-30).
    if (event.Type() == dao::script::EventType::AttackImpact)
    {
        payload["attacker"] = IdJson(event.Object(0));
        payload["target"] = IdJson(event.Object(1));
        payload["hit"] = Named(static_cast<dao::script::CombatResult>(event.Integer(0)));
        payload["effectId"] = event.Integer(1);
    }
    else if (event.Type() == dao::script::EventType::Damaged)
    {
        payload["damager"] = IdJson(event.Creator());
        payload["amount"] = event.Float(0);
        payload["damageType"] = Named(static_cast<dao::script::DamageType>(event.Integer(0)));
        payload["ability"] = AbilityJson(event.Integer(1));
    }
    if (!objects.empty())
    {
        payload["objects"] = std::move(objects);
    }
    if (!integers.empty())
    {
        payload["integers"] = std::move(integers);
    }
    if (!floats.empty())
    {
        payload["floats"] = std::move(floats);
    }
    return payload;
}

Json CommandPayload(ObjectId id, const dao::Command &command)
{
    const auto name = dao::core::Name(command.Type());
    Json payload = {{"id", IdJson(id)},
                    {"type", name.empty() ? Json(static_cast<int>(command.Type())) : Json(std::string(name))},
                    {"playerIssued", command.PlayerIssued()}};
    // What it acts on and where it goes, from its own parameters: read back
    // from the event file, a UseObject did not say which door it used
    // (travel in the darkspawn tunnels, 2026-10-05, #75).
    if (const auto target = command.Target(); target && *target != kInvalidObject)
    {
        payload["target"] = IdJson(*target);
    }
    if (const auto point = command.Position())
    {
        payload["point"] = Json::array({point->x, point->y, point->z});
    }
    if (const auto ability = command.Ability())
    {
        payload["ability"] = *ability;
    }
    return payload;
}

// Each clip a creature begins, for DAOParagonEngine's timelines diffed row by
// row against the game's (#165, DAOLib #217's hook on the play call,
// 0x006AA7E0). Called often in a fight, always on the game's thread: a row's
// label is looked up once.
void AnimationPlayed(Bench &bench, ObjectId id, std::int32_t row, float speed, float blend, bool flag)
{
    static std::unordered_map<std::int32_t, std::string> labels;
    auto [label, added] = labels.try_emplace(row);
    if (added)
    {
        label->second = Narrow(api::GetM2DAString(kTableAnimations, L"Label", row));
    }
    const dao::Vector at = api::GetPosition(id);
    bench.Events().Emit("eluvian.animation.played", Json{{"id", IdJson(id)},
                                                         {"tag", Narrow(api::GetTag(id))},
                                                         {"row", row},
                                                         {"label", label->second},
                                                         {"speed", speed},
                                                         {"blend", blend},
                                                         {"flag", flag},
                                                         {"position", Json::array({at.x, at.y, at.z})}});
}

// Each FMOD event the game starts (#166): put on the bus and nothing more,
// since it comes on the thread that started it.
void SoundPlayed(Bench &bench, const dao::audio::EventStart &start)
{
    Json payload = {
        {"path", std::string(start.path)}, {"name", std::string(start.name)}, {"positional", start.positional}};
    if (start.positional)
    {
        payload["position"] = Json::array({start.position[0], start.position[1], start.position[2]});
    }
    bench.Events().Emit("eluvian.sound.played", std::move(payload));
}

// The moments DAOLib hears (#255), all on the game's thread (#177).
void AddMoments(Bench &bench)
{
    dao::creatures::OnDeath([&bench](ObjectId creature, ObjectId killer) { Deaths().Died(bench, creature, killer); });
    dao::creatures::OnRevive(
        [&bench](ObjectId creature) { bench.Events().Emit("eluvian.creature.revived", Brief(creature)); });
    dao::creatures::OnReady(
        [&bench](ObjectId creature) { bench.Events().Emit("eluvian.creature.ready", Brief(creature)); });
    dao::modes::OnAreaLoaded([&bench] {
        Deaths().AreaLoaded();
        const ObjectId area = api::GetArea(api::GetMainControlled());
        bench.Events().Emit("eluvian.area.loaded", Json{{"area", Brief(area)}});
    });
    dao::modes::OnInputBack([&bench] {
        const auto mode = dao::CClientExoApp::CurrentMode();
        bench.Events().Emit("eluvian.input.back",
                            Json{{"mode", mode ? Json(ModeName(static_cast<std::int32_t>(*mode))) : Json(nullptr)}});
    });
}

} // namespace

void DescribeTopics(EventCatalog &topics)
{
    const auto describe = [&topics](std::string topic, std::string description, Json fields) {
        topics.Describe(TopicSpec{std::move(topic), std::move(description), std::move(fields), "eluvian"});
    };
    describe("eluvian.mode.changed",
             "The game's mode changed. Modes include explore, combat, dialog, cutscene, loading, "
             "pregame (the main menu) and gui (a menu open over the game).",
             Json{{"from", "the mode before the change"}, {"to", "the mode now"}});
    describe("eluvian.paused", "The game was paused or unpaused.",
             Json{{"paused", "true when the game is now paused"}});
    describe("eluvian.party.control_changed",
             "The player now controls a different creature. It fires whatever caused the switch: the party "
             "bar, a key, a script, or an `order` move that hands control to a follower. After a load, it "
             "fires as a switch from nobody.",
             Json{{"before", "the creature controlled before, as {id, name, tag}, or null for nobody"},
                  {"now", "the creature controlled now, as {id, name, tag}"}});
    describe("eluvian.gui.message",
             "A game screen sent a message to the engine, as its buttons and scripts do. It shows what the "
             "menu asked the game to do, in the same form `input` sends with `message`.",
             Json{{"screen", "the screen that sent it, by the game's name for it, such as \"levelup\" or "
                             "\"inventory\""},
                  {"text", "the message, as \"Receiver.Message arguments\""}});
    describe("eluvian.equipment.equipped",
             "An item was put on a creature, by any means: the inventory screen, `equip`, or a script. It "
             "does not fire when equipping is refused, nor for the ammunition the engine gives each "
             "creature when an area loads.",
             Json{{"creature", "the creature, as {id, name, tag}"},
                  {"item", "the item, as {id, name, tag}"},
                  {"slot", "the slot's number (an INVENTORY_SLOT_* value)"},
                  {"slotName", "the slot's name, as `inventory` names it"}});
    describe("eluvian.equipment.unequipped",
             "An item was taken off a creature, by any means. When equipping one item pushes another out "
             "(a shield, for a two-handed weapon), the pushed-out item's event comes first.",
             Json{{"creature", "the creature, as {id, name, tag}"},
                  {"item", "the item, as {id, name, tag}"},
                  {"slot", "the slot it was in (an INVENTORY_SLOT_* value)"},
                  {"slotName", "the slot's name"},
                  {"displaced", "true when equipping another item pushed it out"}});
    describe("eluvian.game.loaded", "A save finished loading, or a new game started, and the world is ready.",
             Json{{"newGame", "true for a new game"},
                  {"character", "the save's character folder"},
                  {"kind", "the save's kind: manual, auto or quick"},
                  {"number", "the save slot's number"}});
    describe("eluvian.game.saved", "The game wrote a save.",
             Json{{"character", "the save's character folder"},
                  {"kind", "the save's kind: manual, auto or quick"},
                  {"number", "the save slot's number"}});
    describe("eluvian.dialogue.line",
             "Someone spoke a line: to the player in a conversation, or between creatures in passing "
             "(background chatter is a conversation too). Some lines have no text; they only lead on to the "
             "next line.",
             Json{{"index", "the line's position in the conversation file"},
                  {"speaker", "who speaks it, as the file names them: a creature's tag, OWNER or PLAYER"},
                  {"by", "the creature who speaks it, as {id, name, tag}. OWNER means the conversation's "
                         "owner, PLAYER the hero, and a tag the creature with that tag"},
                  {"to", "who it is spoken to: `listener` as the file names it (often empty), and `creature`, "
                         "the creature the game picks"},
                  {"text", "the line, in the player's language"}});
    describe("eluvian.dialogue.replies", "The player is offered replies, listed in the order shown on screen.",
             Json{{"replies", "each reply with its position, index, text and icon"}});
    describe("eluvian.dialogue.chosen", "The player chose a reply, and the conversation continues from it.",
             Json{{"index", "the reply's position in the conversation file"}, {"text", "the reply's text"}});
    describe("eluvian.creature.died",
             "A creature died, or a party member fell, on the frame its health reached 0: any creature. The "
             "engine's own eluvian.script.death fires about 2 seconds later, once the death has played out.",
             Json{{"id", "the creature's id"},
                  {"name", "its name"},
                  {"killer", "who killed it: the death's own creator, else the last creature to hurt it; null for "
                             "`stage_kill`'s, whose damage comes from no one"}});
    describe("eluvian.creature.revived", "A dead creature or a fallen party member came back to life.",
             Json{{"id", "the creature's id"}, {"name", "its name"}, {"tag", "its tag"}});
    describe("eluvian.creature.ready",
             "A creature just made (stage_spawn, a script's) is set up by its spawn script: its health, "
             "equipment and abilities are now its own. A creature restored from a save gives none.",
             Json{{"id", "the creature's id"}, {"name", "its name"}, {"tag", "its tag"}});
    describe("eluvian.area.loaded",
             "An area finished loading: the party is placed in it and its load scripts have run. Once per load, "
             "a save's own load aside (that is eluvian.game.loaded).",
             Json{{"area", "the area"}});
    describe("eluvian.input.back",
             "The game takes the player's input again: explore or combat entered from another mode (a load, a "
             "conversation, a cutscene, a screen).",
             Json{{"mode", "the mode entered: explore or combat"}});
    describe("eluvian.animation.played",
             "A creature began a clip on its base layer, by the engine's own play call: a command's step (a "
             "swing, an evade, a deathblow, a knockdown), a conversation line, a walk, the idle. Clips the "
             "animation set starts on its own (a one-shot returning to the idle) are not seen. Often several a "
             "second in a fight: read it with `match` on the creature's id or tag.",
             Json{{"id", "the creature's id"},
                  {"tag", "its tag"},
                  {"row", "the animation's row of the animations table (anim_base, anim_combat, anim_placeables), "
                          "a number"},
                  {"label", "the row's Label"},
                  {"speed", "the play speed, 1 normal"},
                  {"blend", "the fade into it, in weight per second; -1 instant, -2 over a fifth of the clip"},
                  {"flag", "the play call's last argument, as the engine passes it"},
                  {"position", "where the creature stood, [x, y, z]"}});

    describe("eluvian.sound.played",
             "The game started a sound: an FMOD event, by the event's own start, so music, the interface, "
             "footsteps, blows, voices and ambience alike. Often many a second: read it with `match` on the "
             "path, or a part of the path's own field. Which creature made it is not known.",
             Json{{"path", "the event's path, \"project/group/.../event\", as the scripts' PlaySound names it"},
                  {"name", "the event's own name, the path's last part"},
                  {"positional", "true for a sound placed in the world (FMOD's 3-D mode); false for music and the "
                                 "interface"},
                  {"position", "where it was placed, [x, y, z], for a positional sound; a 3-D sound nobody placed "
                               "yet is at its default, usually the origin"}});
    describe("eluvian.command.started",
             "A creature started a command from its queue: an attack, a move, an ability, a wait, an "
             "animation, and so on.",
             Json{{"id", "the creature's id"},
                  {"type", "the kind of command (Attack, MoveToObject, UseAbility, ...)"},
                  {"playerIssued", "whether the player gave the command"},
                  {"target", "the object it acts on, for an attack, a use or a move to an object, if any"},
                  {"point", "the [x, y, z] point it moves to or aims at, for a move or an ability, if any"},
                  {"ability", "the ability's id, for a UseAbility"}});
    describe("eluvian.command.ended", "A creature's command ended. `result` says how.",
             Json{{"id", "the creature's id"},
                  {"type", "the kind of command"},
                  {"playerIssued", "whether the player gave the command"},
                  {"target", "the same as when it started"},
                  {"point", "the same as when it started"},
                  {"ability", "the same as when it started"},
                  {"result", "how it ended: success, a COMMAND_FAILED_* code saying why it failed, or cleared "
                             "(dropped from the queue before it finished)"},
                  {"success", "true when it succeeded"}});
    const Json scriptFields = {{"receiver", "the object the event was sent to, which is not always the one "
                                            "it is about"},
                               {"creator", "the object that created the event"},
                               {"objects", "the event's object parameters, as scripts read them with "
                                           "GetEventObject"},
                               {"integers", "the event's whole-number parameters"},
                               {"floats", "the event's decimal-number parameters"}};
    for (const auto &[type, name] : kScriptEvents)
    {
        Json fields = scriptFields;
        std::string description =
            std::format("The game's script event {} (an EVENT_TYPE_* value), as the game's scripts receive it.",
                        dao::script::Name(type));
        if (type == dao::script::EventType::AttackImpact)
        {
            fields["attacker"] = "the attacker (the event is sent to them)";
            fields["target"] = "the creature attacked";
            fields["hit"] = "how the attack landed (a COMBAT_RESULT_* value)";
            fields["effectId"] = "the id of the effect the hit made";
        }
        else if (type == dao::script::EventType::Damaged)
        {
            fields["damager"] = "the creature that dealt the damage";
            fields["amount"] = "how much damage";
            fields["damageType"] = "the kind of damage (a DAMAGE_TYPE_* value)";
            fields["ability"] = "the ability that dealt it";
        }
        describe(std::format("eluvian.script.{}", name), std::move(description), std::move(fields));
    }
}

void AddEngineEvents(Bench &bench)
{
    DescribeTopics(bench.Topics());

    // Each command a creature starts and ends, and how it ended: what a test
    // waits on after an order ("did the attack happen"), which the scripts'
    // COMMAND_COMPLETE reaches only sometimes (DAOLib 38eea39). On the game's
    // thread inside the command step: emit and return.
    dao::commands::OnStart([&bench](ObjectId id, const dao::Command &command) {
        if (IsCreature(id))
        {
            bench.Events().Emit("eluvian.command.started", CommandPayload(id, command));
        }
    });
    dao::commands::OnEnd([&bench](ObjectId id, const dao::Command &command, std::int32_t result) {
        if (IsCreature(id))
        {
            Json payload = CommandPayload(id, command);
            const auto name = dao::commands::ResultName(result);
            payload["result"] = name.empty() ? Json(result) : Json(std::string(name));
            payload["success"] = result == dao::commands::kSuccess;
            bench.Events().Emit("eluvian.command.ended", std::move(payload));
        }
    });

    dao::animation::OnPlayed([&bench](ObjectId id, std::int32_t row, float speed, float blend, bool flag) {
        AnimationPlayed(bench, id, row, speed, blend, flag);
    });

    // Each FMOD event the game starts, for DAOParagonEngine's mixer checked
    // against the game's (#166, DAOLib #218's hook on fmod_event.dll's
    // Event::start). On the thread that started it, which need not be the
    // game's: nothing of the game is read here, only the event put on the bus.
    dao::audio::OnEventStart([&bench](const dao::audio::EventStart &start) { SoundPlayed(bench, start); });
    dao::saves::OnGameLoaded([&bench](const std::optional<dao::saves::Slot> &slot) {
        Json payload = {{"newGame", !slot.has_value()}};
        if (slot)
        {
            payload["character"] = Narrow(slot->character);
            payload["kind"] = dao::saves::Name(slot->kind);
            payload["number"] = slot->number;
        }
        bench.Events().Emit("eluvian.game.loaded", std::move(payload));
    });
    // Conversations, as DAOLib's hooks see them: inside the engine's own
    // work, so each does no more than read the line and put it on the bus.
    dao::dialogue::OnNpcLine([&bench](dao::dialogue::Conversation &talk, const dao::dialogue::Line &line) {
        bench.Events().Emit("eluvian.dialogue.line", LineJson(line, &talk));
    });
    dao::dialogue::OnReplies([&bench](dao::dialogue::Conversation &talk, std::span<const dao::dialogue::Line> replies) {
        bench.Events().Emit("eluvian.dialogue.replies", Json{{"replies", RepliesJson(replies, &talk)}});
    });
    dao::dialogue::OnReplyChosen([&bench](dao::dialogue::Conversation &talk, const dao::dialogue::Line &reply) {
        bench.Events().Emit("eluvian.dialogue.chosen", LineJson(reply, &talk));
    });
    // Who the player controls, and what goes on or comes off whom, by any
    // cause: DAOLib #22's hooks at the engine's own functions, so an order
    // handing control over (#48) or a script's equip is caught as a click
    // is (lib-agent, 2026-10-04, #57). On the game's thread: emit and return.
    dao::party::OnControlChanged([&bench](ObjectId before, ObjectId now) {
        bench.Events().Emit("eluvian.party.control_changed",
                            Json{{"before", before == kInvalidObject ? Json(nullptr) : Brief(before)},
                                 {"now", now == kInvalidObject ? Json(nullptr) : Brief(now)}});
    });
    // Only what was done into a real slot: a refused equip comes with slot
    // 255 and done false, and as an area loads the engine equips every
    // creature's ammunition with slot 255, a burst at each load
    // (turn-based-agent and lib-agent, 2026-10-04). An item an equip pushed
    // out of its slot comes as an unequip, `displaced` (DAOLib 66ab84a).
    constexpr std::int32_t kNoSlot = 255;
    const auto equipment = [&bench](const char *topic, const dao::equipment::Change &change) {
        if (!change.done || change.slot == kNoSlot)
        {
            return;
        }
        const std::string_view slot = EquipSlotName(change.slot);
        bench.Events().Emit(topic, Json{{"creature", Brief(change.creature)},
                                        {"item", Brief(change.item)},
                                        {"slot", change.slot},
                                        {"slotName", slot.empty() ? Json(nullptr) : Json(std::string(slot))},
                                        {"displaced", change.displaced}});
    };
    dao::equipment::OnEquip(
        [equipment](const dao::equipment::Change &change) { equipment("eluvian.equipment.equipped", change); });
    dao::equipment::OnUnequip(
        [equipment](const dao::equipment::Change &change) { equipment("eluvian.equipment.unequipped", change); });
    // Every message a screen sends the engine, as it sent it: the
    // level-up screen's own SpendPoint took a point and the engine kept
    // nothing, and the question was what went out, with which creature
    // (turn-based-agent, 2026-10-05, #58). DAOLib's OnAnyFSCommand sees
    // each FSCommand before its handler (lib-agent); the screens send their
    // messages as SendMessage, and its other commands (sounds, movies,
    // telemetry) are left off.
    dao::gui::OnAnyFSCommand([&bench](void *owner, std::string_view name, std::string_view args) {
        if (name != "SendMessage")
        {
            return;
        }
        bench.Events().Emit("eluvian.gui.message",
                            Json{{"screen", Narrow(dao::gui::NameOf(owner))}, {"text", std::string(args)}});
    });
    dao::saves::OnSaved([&bench](const dao::saves::Slot &slot) {
        bench.Events().Emit("eluvian.game.saved", Json{{"character", Narrow(slot.character)},
                                                       {"kind", dao::saves::Name(slot.kind)},
                                                       {"number", slot.number}});
    });
    for (const auto &[type, name] : kScriptEvents)
    {
        dao::events::OnScriptEvent(type, [&bench, type, topic = std::format("eluvian.script.{}", name)](
                                             ObjectId receiver, const dao::CScriptEvent &event) {
            bench.Events().Emit(topic, ScriptEventPayload(receiver, event));
            if (type == dao::script::EventType::Damaged)
            {
                Deaths().Note(receiver, event.Creator());
            }
            else if (type == dao::script::EventType::AttackImpact)
            {
                Deaths().Note(event.Object(1), event.Object(0));
            }
        });
    }
    AddMoments(bench);
}

} // namespace eluvian::game
