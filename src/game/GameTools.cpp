#include "game/GameTools.h"

#include "game/Shared.h"

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
    AddScript(bench, watch);
    AddOrder(bench, watch);
    AddStage(bench, watch);
    AddScreenshot(bench, watch);
    AddGame(bench, watch);
    AddDialogue(bench, watch);
    AddCamera(bench, watch);
    AddInput(bench, watch);
    AddScreen(bench, watch);
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
    return Json{{"id", IdJson(id)},
                {"type", name.empty() ? Json(static_cast<int>(command.Type())) : Json(std::string(name))},
                {"playerIssued", command.PlayerIssued()}};
}

} // namespace

namespace
{

// What each of Eluvian's own topics means, in the catalogue a mod's go into
// too (`events describe=true`).
void DescribeTopics(EventCatalog &topics)
{
    const auto describe = [&topics](std::string topic, std::string description, Json fields) {
        topics.Describe(TopicSpec{std::move(topic), std::move(description), std::move(fields), "eluvian"});
    };
    describe("eluvian.mode.changed",
             "The game mode changed: explore, combat, dialog, cutscene, loading, pregame "
             "(the main menu), gui (a menu over the game), and the rest.",
             Json{{"from", "the mode before"}, {"to", "the mode now"}});
    describe("eluvian.paused", "The game was paused or unpaused.", Json{{"paused", "true or false"}});
    describe("eluvian.game.loaded", "A save loaded, or a new game began, and the world is in place.",
             Json{{"newGame", "true for a new game"},
                  {"character", "the save's character folder"},
                  {"kind", "manual, auto or quick"},
                  {"number", "the slot's number"}});
    describe("eluvian.game.saved", "The game wrote a save.",
             Json{{"character", "the save's character folder"},
                  {"kind", "manual, auto or quick"},
                  {"number", "the slot's number"}});
    describe("eluvian.dialogue.line",
             "A line was spoken to the player in a conversation, or by creatures in passing (ambient lines "
             "are conversations too). Some have no text: a line that only routes to the next.",
             Json{{"index", "the line's place in the conversation's file"},
                  {"speaker", "who, as the file writes it: a tag, OWNER or PLAYER"},
                  {"text", "the line, in the player's language"}});
    describe("eluvian.dialogue.replies", "The replies now offered to the player, in their order on the screen.",
             Json{{"replies", "each with its place, index, text and icon"}});
    describe("eluvian.dialogue.chosen", "The player chose a reply, and the conversation goes on from it.",
             Json{{"index", "the reply's place in the file"}, {"text", "the reply"}});
    describe("eluvian.creature.died",
             "A creature died, or a party member fell, the moment it did: seen by watching creatures that were "
             "hurt or staged. The engine's own eluvian.script.death comes 1.5 to 2.5 s later, once the death has "
             "played out.",
             Json{{"id", "the creature"}, {"name", "its name"}, {"killer", "the last who hurt it, if known"}});
    describe("eluvian.command.started",
             "A creature started a command from its queue: an attack, a move, an "
             "ability, a wait, an animation.",
             Json{{"id", "the creature"},
                  {"type", "the command (Attack, MoveToObject, UseAbility, ...)"},
                  {"playerIssued", "whether the player gave it"}});
    describe("eluvian.command.ended", "A creature's command ended, and how.",
             Json{{"id", "the creature"},
                  {"type", "the command"},
                  {"playerIssued", "whether the player gave it"},
                  {"result", "success, a COMMAND_FAILED_*, or cleared"},
                  {"success", "true when it succeeded"}});
    const Json scriptFields = {{"receiver", "the object the event was delivered to (not always the one it is about)"},
                               {"creator", "the event's creator"},
                               {"objects", "its object parameters, as GetEventObject reads them"},
                               {"integers", "its integer parameters"},
                               {"floats", "its float parameters"}};
    for (const auto &[type, name] : kScriptEvents)
    {
        Json fields = scriptFields;
        std::string description =
            std::format("The script event {} (EVENT_TYPE_*), as the scripts receive it.", dao::script::Name(type));
        if (type == dao::script::EventType::AttackImpact)
        {
            fields["attacker"] = "who attacked (delivered to them)";
            fields["target"] = "who was attacked";
            fields["hit"] = "COMBAT_RESULT_*";
            fields["effectId"] = "the effect it made";
        }
        else if (type == dao::script::EventType::Damaged)
        {
            fields["damager"] = "who dealt it";
            fields["amount"] = "how much";
            fields["damageType"] = "DAMAGE_TYPE_*";
            fields["ability"] = "the ability it came from";
        }
        describe(std::format("eluvian.script.{}", name), std::move(description), std::move(fields));
    }
}

} // namespace

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
    dao::dialogue::OnNpcLine([&bench](dao::dialogue::Conversation &, const dao::dialogue::Line &line) {
        bench.Events().Emit("eluvian.dialogue.line", LineJson(line, HeroIsFemale()));
    });
    dao::dialogue::OnReplies([&bench](dao::dialogue::Conversation &, std::span<const dao::dialogue::Line> replies) {
        bench.Events().Emit("eluvian.dialogue.replies", Json{{"replies", RepliesJson(replies, HeroIsFemale())}});
    });
    dao::dialogue::OnReplyChosen([&bench](dao::dialogue::Conversation &, const dao::dialogue::Line &reply) {
        bench.Events().Emit("eluvian.dialogue.chosen", LineJson(reply, HeroIsFemale()));
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
    dao::OnFrame([&bench] { Deaths().Tick(bench); });
}

} // namespace eluvian::game
