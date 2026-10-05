// What the game's tools share (game/Shared.h): a creature's answers, the
// checks before the game is touched, and the watch on deaths.
#include "game/Shared.h"

#include "core/Placeables.h"

#include <algorithm>
#include <cwctype>
#include <regex>

namespace eluvian::game
{

// Script commands are not to be called while an area loads: objects come
// and go (lib-agent, 2026-09-30). Before the application is up there is
// nothing to ask.
void RequireWorld(const Watch &watch, std::string_view tool)
{
    const auto mode = watch.Mode();
    if (mode < 0)
    {
        throw ToolError(std::format("{}: the game is still starting; health says when it runs", tool));
    }
    if (mode == static_cast<std::int32_t>(dao::GameMode::Loading))
    {
        throw ToolError(
            std::format("{}: an area is loading; wait_for topic \"eluvian.mode.changed\" and try again", tool));
    }
}

// The controller read as the scripts read it: the placeable types table
// (93) by the placeable's appearance (core_h.nss,
// GetPlaceableStateCntTable).
constexpr std::int32_t kTablePlaceableTypes = 93;

std::string PlaceableController(ObjectId id)
{
    return Narrow(api::GetM2DAString(kTablePlaceableTypes, L"StateController", api::GetAppearanceType(id)));
}

Json PlaceableStateWord(ObjectId id)
{
    const auto word = PlaceableStateName(PlaceableController(id), api::GetPlaceableState(id));
    return word.empty() ? Json(nullptr) : Json(std::string(word));
}

Json Brief(ObjectId id)
{
    if (id == kInvalidObject)
    {
        return nullptr;
    }
    return Json{{"id", IdJson(id)}, {"name", Narrow(api::GetName(id))}, {"tag", Narrow(api::GetTag(id))}};
}

std::vector<ObjectId> Objects(const dao::script::Array &array)
{
    std::vector<ObjectId> ids;
    ids.reserve(array.Size());
    for (std::size_t i = 0; i < array.Size(); ++i)
    {
        ids.push_back(array.Object(i));
    }
    return ids;
}

float Distance(const dao::Vector &a, const dao::Vector &b)
{
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

Json Depletable(ObjectId id, std::int32_t property)
{
    return Json{{"current", api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_CURRENT)},
                {"max", api::GetCreatureProperty(id, property, k::PROPERTY_VALUE_TOTAL)}};
}

// Whose side a creature is on: the party, hostile to it, or neither. Judged
// from the creature the player controls while that one is in the party, the
// hero where none is: the story can take the hero out of the party, and in
// Fort Drakon's rescue, Leliana and Dog the party and the hero a captive,
// every guard attacking them read neutral from the hero's view
// (turn-based-agent, 2026-10-06, #94).
std::string Side(ObjectId id, ObjectId hero, const std::set<ObjectId> &party)
{
    if (party.contains(id))
    {
        return "party";
    }
    ObjectId from = api::GetMainControlled();
    if (from == kInvalidObject || !party.contains(from))
    {
        from = hero;
    }
    if (from != kInvalidObject && api::IsObjectHostile(from, id) != 0)
    {
        return "hostile";
    }
    return "neutral";
}

Json PathJson(const dao::world::WalkPath &path, const dao::Vector &from, bool waypoints)
{
    Json answer = {{"length", path.Length()},
                   {"remaining", path.Remaining(from)},
                   {"waypointCount", path.waypoints.size()},
                   {"next", path.next}};
    if (waypoints)
    {
        Json points = Json::array();
        for (const auto &point : path.waypoints)
        {
            points.push_back(VectorJson(point));
        }
        answer["waypoints"] = std::move(points);
    }
    return answer;
}

// The command a creature is carrying out, if any (DAOLib's CCObject).
const dao::Command *CurrentCommand(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    const auto *object = app != nullptr ? app->GetObjectById(id) : nullptr;
    return object != nullptr ? object->CurrentCommand() : nullptr;
}

// What a creature is doing: its current command's type, whether the player
// gave it, and how many commands are queued.
Json CommandJson(ObjectId id)
{
    auto *app = dao::CClientExoApp::Get();
    const auto *object = app != nullptr ? app->GetObjectById(id) : nullptr;
    if (object == nullptr)
    {
        return nullptr;
    }
    const auto *command = object->CurrentCommand();
    Json current = nullptr;
    if (command != nullptr)
    {
        const auto name = dao::core::Name(command->Type());
        current = Json{{"type", name.empty() ? Json(static_cast<int>(command->Type())) : Json(std::string(name))},
                       {"playerIssued", command->PlayerIssued()}};
    }
    return Json{{"current", std::move(current)}, {"queued", object->CommandQueueSize()}};
}

bool IsMove(dao::core::CommandType type)
{
    using dao::core::CommandType;
    switch (type)
    {
    case CommandType::MoveToLocation:
    case CommandType::MoveToObject:
    case CommandType::MoveToMultipleLocations:
    case CommandType::MoveAwayFromObject:
    case CommandType::MoveAwayFromPoint:
    case CommandType::RandomWalk:
    case CommandType::ForceFollowObject:
        return true;
    default:
        return false;
    }
}

// The path the creature's movement holds, and whether it is walking it now.
// The movement keeps its last path after the walk: in play, 2026-09-30, a
// hero standing still since the save loaded held one 2.45 m long. So the
// path alone says nothing; the current command says whether it is walked.
Json PathNow(ObjectId id, const dao::Vector &from, bool waypoints)
{
    const auto path = dao::world::CurrentPath(id);
    if (!path)
    {
        return nullptr;
    }
    Json answer = PathJson(*path, from, waypoints);
    const auto *command = CurrentCommand(id);
    answer["beingWalked"] = command != nullptr && IsMove(command->Type());
    return answer;
}

// The game's own message box, if one is up: its text, and each button with
// what pressing it sends. A box holds the game until it is answered and no
// event tells of it: a load asked for a save that needed add-ins no longer
// registered stopped at one, "Force Load / Cancel", and the call had
// answered `asked` (2026-10-01). DAOLib's dao::gui::CurrentPopup (788079b).
Json PopupJson()
{
    const auto popup = dao::gui::CurrentPopup();
    if (!popup)
    {
        return nullptr;
    }
    Json buttons = Json::array();
    for (const auto &button : popup->buttons)
    {
        buttons.push_back(Json{{"label", Narrow(button.label)}, {"sends", Narrow(button.callback)}});
    }
    return Json{{"text", Narrow(popup->text)}, {"buttons", std::move(buttons)}};
}

namespace
{

// Who speaks a line, by the game's objects, where the file names the
// speaker by a tag ("OWNER" for the conversation's own creature, "PLAYER",
// or a creature's tag) and an agent reading it learned nothing of who
// spoke: the Tower Guard's lines all read OWNER (turn-based-agent,
// 2026-10-03, #23). The engine's own answer first, the one the camera and
// lip sync follow (DAOLib's Speaker, lib-agent's #10 that day: OWNER the
// conversation's owner, PLAYER the player, HENCH a party member, any other
// text a tag near the owner). Without a conversation to ask, the player's
// lines are the hero's (GetPCSpeaker) and a tag is the nearest creature
// that has it; OWNER then is null, no script command giving it
// (GetLastSpeaker, 439, has no function).
Json SpeakerJson(const dao::dialogue::Line &line, const dao::dialogue::Conversation *conversation)
{
    if (conversation != nullptr)
    {
        if (const auto who = conversation->Speaker(line.index); who && *who != kInvalidObject)
        {
            return Brief(*who);
        }
    }
    std::wstring speaker = line.speaker;
    std::ranges::transform(speaker, speaker.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (line.kind == dao::dialogue::Line::Kind::Player || speaker == L"PLAYER")
    {
        const ObjectId pc = api::GetPCSpeaker();
        return Brief(pc != kInvalidObject ? pc : api::GetHero());
    }
    if (speaker.empty() || speaker == L"OWNER")
    {
        return nullptr;
    }
    const ObjectId nearest = api::UT_GetNearestObjectByTag(api::GetHero(), line.speaker, 1);
    return nearest != kInvalidObject ? Brief(nearest) : Json(nullptr);
}

// Whom a line is spoken to, as the file names the listener (often empty)
// and as the engine resolves it (DAOLib's Listener). For an empty one the
// engine falls back to whoever began the conversation, BeginConversation's
// oInitiator (the character the player controls, for a creature told to
// talk to itself), as addr-lib-agent read it; the owner is its oTarget
// (DAOLib ecb1f60's notes, 2026-10-03, #23). Not yet seen in play. Null
// without a conversation to ask.
Json ListenerJson(const dao::dialogue::Line &line, const dao::dialogue::Conversation *conversation)
{
    if (conversation == nullptr)
    {
        return nullptr;
    }
    const auto who = conversation->Listener(line.index);
    return Json{{"listener", Narrow(line.listener)},
                {"creature", who && *who != kInvalidObject ? Brief(*who) : Json(nullptr)}};
}

// A line's text as the screen shows it, in plain text: the engine's own
// token replacement (DAOLib 7bab46e, ReplaceDesignerTags), which fills in
// <FirstName/> and the like that replies held unresolved (#17), and turns
// the talk table's markup into the Flash HTML the screen draws (<emp> into
// <b>); that HTML is taken out here. A token the engine does not know stays
// as it was, to be seen.
std::wstring Shown(std::wstring_view text)
{
    std::wstring shown = dao::tlk::Resolve(text);
    static const std::wregex html(LR"(</?(b|i|u|font|br|p|span)\b[^>]*>)", std::regex::icase);
    return std::regex_replace(shown, html, L"");
}

} // namespace

// A conversation's line as the screen has it: its place in the file, who
// speaks as the file writes it (a tag, or OWNER or PLAYER), and its text in
// the player's language, in the form for the hero's sex, its tokens filled
// in and its markup taken out. A line may have no text: an NPC line that
// only routes, a "continue" reply (DAOLib's Dialogue.h, seen at Highever,
// 2026-09-30).
Json LineJson(const dao::dialogue::Line &line, bool female, const dao::dialogue::Conversation *conversation)
{
    if (conversation == nullptr)
    {
        conversation = dao::dialogue::Current();
    }
    Json json = {
        {"index", line.index},
        {"speaker", Narrow(line.speaker)},
        {"by", SpeakerJson(line, conversation)},
        {"to", ListenerJson(line, conversation)},
        {"text", line.text == 0xFFFFFFFF ? std::string() : Narrow(Shown(dao::tlk::String(line.text, female)))}};
    if (line.kind == dao::dialogue::Line::Kind::Player)
    {
        json["icon"] = line.icon;
    }
    return json;
}

bool HeroIsFemale()
{
    return api::GetCreatureGender(api::GetHero()) == k::GENDER_FEMALE;
}

// The replies on the screen, each with its `place` among them: the number
// a choice names.
Json RepliesJson(std::span<const dao::dialogue::Line> replies, bool female,
                 const dao::dialogue::Conversation *conversation)
{
    Json list = Json::array();
    for (std::size_t place = 0; place < replies.size(); ++place)
    {
        Json reply = LineJson(replies[place], female, conversation);
        reply["place"] = place;
        list.push_back(std::move(reply));
    }
    return list;
}

// The room a creature stands in and whether the party has seen it: the
// game draws a room never seen black, with what stands in it (DAOLib's
// dao::world::Room, seen in Castle Cousland, 2026-10-01). Null where the
// area has no rooms to ask. Outdoors the "rooms" are the terrain's cells:
// in the Forest Clearing their numbers read 262147, 196611 and the like,
// `seen` in view everywhere, and none ever loaded, the party's own included
// (the same day), so `shown` goes by `seen` alone (DAOLib 66d5567). `wanted`
// and `load` are the game's own two states, by name where DAOLib names
// them and by number where it does not.
Json RoomJson(ObjectId id)
{
    using dao::world::Room;
    const auto room = dao::world::RoomOf(id);
    if (!room)
    {
        return nullptr;
    }
    const Json seen =
        Label(room->seen, {{Room::kNeverSeen, "never"}, {Room::kInView, "in view"}, {Room::kSeenBefore, "before"}});
    const Json wanted =
        Label(room->state, {{Room::kDropped, "dropped"}, {Room::kWanted, "kept"}, {Room::kWantedNow, "now"}});
    const Json load =
        Label(room->load, {{Room::kLoading, "loading"}, {Room::kLoaded, "loaded"}, {Room::kNotLoaded, "not loaded"}});
    return Json{{"number", room->number}, {"seen", seen}, {"shown", room->Shown()}, {"wanted", wanted}, {"load", load}};
}

Json Summary(ObjectId id, ObjectId hero, const std::set<ObjectId> &party)
{
    Json creature = Brief(id);
    creature["side"] = Side(id, hero, party);
    creature["position"] = VectorJson(api::GetPosition(id));
    creature["dead"] = api::IsDead(id) != 0;
    creature["health"] = Depletable(id, k::PROPERTY_DEPLETABLE_HEALTH);
    creature["manaStamina"] = Depletable(id, k::PROPERTY_DEPLETABLE_MANA_STAMINA);
    creature["template"] = Narrow(api::GetResRef(id)); // what stage spawn takes; the tag is the instance's
    // `side` is the hero's view and has three values; the group is the
    // game's own, and says more: a creature of group 2 fighting the hostile
    // ones read "neutral" (computer-use-agent, 2026-10-01). 0 is the
    // player's, 1 the hostile one, 2 friendly, as the game sets them.
    creature["group"] = api::GetGroupId(id);
    creature["combatState"] = api::GetCombatState(id);
    creature["attackTarget"] = Brief(api::GetAttackTarget(id));
    creature["room"] = RoomJson(id);
    creature["command"] = CommandJson(id);
    return creature;
}

// An effect's duration type by name. script.ldf names 0 to 3 and 6; 4 is
// an equipped item's effects, as seen in play on Aedan's rings and belt
// (2026-09-30; DAOLib 80d5f01 notes it at CGameEffect::DurationType).
Json DurationJson(dao::script::EffectDurationType type)
{
    if (static_cast<std::int32_t>(type) == 4)
    {
        return "4 (equipped item)";
    }
    return Named(type);
}

// An ability by id and its name in the player's language ("Fireball");
// null for none. The names come from the data tables, loaded by the main
// menu (dao/Abilities.h).
Json AbilityJson(std::int32_t ability)
{
    if (ability <= 0)
    {
        return nullptr;
    }
    Json answer = {{"id", ability}};
    if (const auto name = dao::abilities::Name(ability))
    {
        answer["name"] = Narrow(*name);
    }
    // How it is aimed, from the ability table (abi_base): its TargetType
    // bits, its area (AOE_Type, 0 none), its range. Fireball's TargetType
    // has every bit; its area with a range is what makes it aimed at a
    // place (#35).
    answer["targetType"] = api::GetM2DAInt(k::TABLE_ABILITIES_SPELLS, L"TargetType", ability);
    answer["aoe"] = api::GetM2DAInt(k::TABLE_ABILITIES_SPELLS, L"AOE_Type", ability);
    answer["range"] = api::GetM2DAInt(k::TABLE_ABILITIES_SPELLS, L"Range", ability);
    return answer;
}

bool AimedAtAPlace(std::int32_t ability)
{
    return ability > 0 && api::GetM2DAInt(k::TABLE_ABILITIES_SPELLS, L"AOE_Type", ability) != 0 &&
           api::GetM2DAInt(k::TABLE_ABILITIES_SPELLS, L"Range", ability) > 0;
}

// The abilities a creature acts with, and each one's cooldown: those on its
// quickbar and those its tactics use (no script command lists a creature's
// abilities whole). A cooldown is in seconds of game time, null for an
// ability the creature has no record for.
Json Abilities(ObjectId id)
{
    constexpr std::int32_t kQuickslots = 60;
    constexpr std::int32_t kTactics = 50;
    std::set<std::int32_t> found;
    for (std::int32_t slot = 0; slot < kQuickslots; ++slot)
    {
        if (const auto ability = api::GetQuickslot(id, slot); ability > 0)
        {
            found.insert(ability);
        }
    }
    const auto tactics = std::min(api::GetNumTactics(id), kTactics);
    for (std::int32_t index = 0; index < tactics; ++index)
    {
        if (const auto ability = api::GetTacticCommandParam(id, index);
            ability > 0 && api::HasAbility(id, ability) != 0)
        {
            found.insert(ability);
        }
    }
    Json abilities = Json::array();
    for (const auto ability : found)
    {
        Json entry = AbilityJson(ability);
        // `available` is the scripts' IsAbilityAvailable, which says only
        // that the ability can be learned (DAOLib 7bab46e): kept as it was.
        const bool available = api::IsAbilityAvailable(id, ability) != 0;
        const auto cooldown = dao::abilities::GetCooldown(id, ability);
        entry["available"] = available;
        entry["cooldown"] = cooldown ? Json{{"remaining", cooldown->remaining}, {"total", cooldown->total}} : Json();
        UseInto(entry, id, ability, available && (!cooldown || cooldown->remaining <= 0));
        abilities.push_back(std::move(entry));
    }
    return abilities;
}

// Whether a creature can use an ability now, by the engine's own check:
// the one the quickbar greys a button by and a player's order runs first,
// which weighs the cost, the cooldown, the conditions and what holds it
// (DAOLib 7bab46e, CanUse). The scripts' IsAbilityAvailable says only that
// it can be learned: it read ready for a 40-stamina Mighty Blow at 12
// stamina, and the order did nothing (turn-based-agent, 2026-10-03, #11).
// `ready` the engine's answer (`fallback` when it gives none), `why` its
// reason when not, and `cost` in mana or stamina, fatigue included.
void UseInto(Json &entry, ObjectId id, std::int32_t ability, bool fallback)
{
    const auto use = dao::abilities::CanUse(id, ability);
    entry["ready"] = use ? use->usable : fallback;
    if (use && !use->usable)
    {
        if (auto why = Narrow(use->Reason()); !why.empty())
        {
            entry["why"] = std::move(why);
        }
    }
    if (const auto cost = dao::abilities::Cost(id, ability); cost && *cost > 0)
    {
        entry["cost"] = *cost;
    }
}

Json Effects(ObjectId id)
{
    Json effects = Json::array();
    const auto array = api::GetEffects(id);
    for (std::size_t i = 0; i < array.Size(); ++i)
    {
        const auto *effect = array.Effect(i);
        if (effect == nullptr)
        {
            continue;
        }
        const bool timed = effect->DurationMs() > 0;
        effects.push_back(Json{
            {"effectId", effect->Id()},
            {"type", Named(effect->Type())},
            {"durationType", DurationJson(effect->DurationType())},
            {"creator", Brief(effect->Creator())},
            {"ability", AbilityJson(effect->AbilityId())},
            {"duration", timed ? Json(effect->Duration()) : Json(nullptr)},
            {"remaining", timed ? Json(effect->Remaining()) : Json(nullptr)},
        });
    }
    return effects;
}

std::set<ObjectId> PartyOf(ObjectId hero)
{
    if (hero == kInvalidObject)
    {
        return {};
    }
    const auto ids = Objects(api::GetPartyList(hero));
    return {ids.begin(), ids.end()};
}

DeathWatch &Deaths()
{
    static DeathWatch deaths;
    return deaths;
}

} // namespace eluvian::game
