// What the game's tools share: the answers each gives of a creature (its
// summary, its room, its effects and abilities, the command it carries out),
// the checks before any of them touches the game, the watch on deaths, and
// each tool's own registration, which GameTools.cpp calls. One file per
// tool: Inspect, Script, Order, Stage, Screenshot, Game, Input, Camera, Dialogue
// and Screen; Character and Gui hold reads of their own. Everything
// here is on the game's thread.
#pragma once

#include "core/Args.h"
#include "core/Bench.h"
#include "game/Convert.h"
#include "game/Health.h"

#include "dao/Abilities.h"
#include "dao/CameraMoves.h"
#include "dao/ClientExoApp.h"
#include "dao/CommandEvents.h"
#include "dao/Dialogue.h"
#include "dao/Events.h"
#include "dao/Frame.h"
#include "dao/GameModes.h"
#include "dao/Gui.h"
#include "dao/Log.h"
#include "dao/Messages.h"
#include "dao/Objects.h"
#include "dao/Orders.h"
#include "dao/Party.h"
#include "dao/Saves.h"
#include "dao/ScriptCall.h"
#include "dao/Tlk.h"
#include "dao/World.h"
#include "dao/generated/ScriptApi.h"
#include "dao/generated/ScriptConstants.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace eluvian::game
{

// A value's name from a short table, or the value itself where the table has
// none: for the engine's enumerations, which a value of a later build may
// not be in. Read where nested ?: chains were (#77).
template <typename T>
Json Label(T value, std::initializer_list<std::pair<std::type_identity_t<T>, std::string_view>> names)
{
    for (const auto &[known, name] : names)
    {
        if (value == known)
        {
            return Json(std::string(name));
        }
    }
    if constexpr (std::is_enum_v<T>)
    {
        return Json(static_cast<std::underlying_type_t<T>>(value));
    }
    else
    {
        return Json(value);
    }
}

namespace api = dao::script::api;
namespace k = dao::script::constants;
using dao::kInvalidObject;
using dao::ObjectId;

inline constexpr std::string_view kCreatureHint = "a creature, as creatures lists them";

// Refuses while the game is starting or an area loads.
void RequireWorld(const Watch &watch, std::string_view tool);

// An object by id, name and tag; null for none.
Json Brief(ObjectId id);
// A placeable's state controller (StateCnt_Door, StateCnt_Container, ...)
// and its state in a word as the controller names it (#44); null where it
// names none.
std::string PlaceableController(ObjectId id);
Json PlaceableStateWord(ObjectId id);
std::vector<ObjectId> Objects(const dao::script::Array &array);
float Distance(const dao::Vector &a, const dao::Vector &b);
Json Depletable(ObjectId id, std::int32_t property);
std::string Side(ObjectId id, ObjectId hero, const std::set<ObjectId> &party);
std::set<ObjectId> PartyOf(ObjectId hero);

// An equipment slot's name (INVENTORY_SLOT_*), as inventory names it:
// "main_hand", "chest", ...; empty for none.
std::string_view EquipSlotName(std::int32_t slot);

Json PathJson(const dao::world::WalkPath &path, const dao::Vector &from, bool waypoints);
const dao::Command *CurrentCommand(ObjectId id);
Json CommandJson(ObjectId id);
bool IsMove(dao::core::CommandType type);
Json PathNow(ObjectId id, const dao::Vector &from, bool waypoints);

Json PopupJson();

// The text in the message box's field, if the box up has one.
std::optional<std::wstring> PopupField();
// An item in brief (name, tag, template, base type, stack, value, plot), or
// in full as item gives it (Character.cpp).
Json ItemJson(ObjectId item, bool full);

// The game's loot panel (Gui.cpp): the container whose panel is up, if one
// is; the panel as gui gives it, null with none; and a press
// of one of its buttons by its place in kLootButtons, answering what came
// of it. Throws ToolError when no panel is up.
inline constexpr std::array<std::string_view, 2> kLootButtons = {"Take All", "Close"};
std::optional<ObjectId> LootPanelContainer();
Json LootPanelJson();
Json PressLootButton(Bench &bench, std::size_t index);

// The data tree's reads the panels share (Gui.cpp): an object id the tree
// holds in decimal; the creature the interface has selected; the items an
// inventory holds, its backpack's.
std::optional<ObjectId> TreeId(std::wstring_view path);
ObjectId Selected();
std::vector<ObjectId> ItemsIn(ObjectId container);

// The store screen (Store.cpp): its store object if one is up, and the
// store as gui gives it, null with none.
std::optional<ObjectId> StoreUp();
Json StoreJson();

Json LineJson(const dao::dialogue::Line &line, bool female, const dao::dialogue::Conversation *conversation = nullptr);
bool HeroIsFemale();
Json RepliesJson(std::span<const dao::dialogue::Line> replies, bool female,
                 const dao::dialogue::Conversation *conversation = nullptr);

Json RoomJson(ObjectId id);
// A creature as every listing and every stage answer gives it.
Json Summary(ObjectId id, ObjectId hero, const std::set<ObjectId> &party);

// A script constant by its name ("EFFECT_TYPE_ROOT"); its number when
// nothing names it, as a script's own numbers are not.
template <class E> Json Named(E value)
{
    const auto name = dao::script::Name(value);
    return name.empty() ? Json(static_cast<std::int32_t>(value)) : Json(std::string(name));
}

Json DurationJson(dao::script::EffectDurationType type);
// `ready`, `why` and `cost` of an ability for a creature, into its entry.
void UseInto(Json &entry, ObjectId id, std::int32_t ability, bool fallback);
Json AbilityJson(std::int32_t ability);
// Whether an ability is an area aimed at a place (an area and a range):
// Fireball ordered at a creature failed COMMAND_FAILED_PATH_ACTION_REQUIRED
// 41 times, and at the creature's position cast (#35).
bool AimedAtAPlace(std::int32_t ability);
Json Abilities(ObjectId id);
Json Effects(ObjectId id);

// Deaths, watched as well as heard. A creature that is hurt (a damage or
// an attack's impact) or staged is looked at every frame until it is dead,
// gone, or a minute passes with no more harm, and its death is
// eluvian.creature.died with the last one who hurt it. The engine's own
// EVENT_TYPE_DEATH was not heard at all before DAOLib bc081ea; since, it
// comes as eluvian.script.death 1.5 to 2.5 s after this, once the death
// has played out (play, 2026-09-30). So this is the moment of the death,
// and that its end. On the game's thread only, as every caller is.
class DeathWatch
{
  public:
    void Note(ObjectId id, ObjectId harmer)
    {
        if (id == kInvalidObject)
        {
            return;
        }
        Entry &entry = watched_[id];
        if (harmer != kInvalidObject)
        {
            entry.killer = harmer;
        }
        entry.since = std::chrono::steady_clock::now();
    }

    void Tick(Bench &bench)
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = watched_.begin(); it != watched_.end();)
        {
            const ObjectId id = it->first;
            if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
            {
                it = watched_.erase(it); // gone from the world
                continue;
            }
            if (api::IsDead(id) != 0)
            {
                bench.Events().Emit(
                    "eluvian.creature.died",
                    Json{{"id", IdJson(id)}, {"name", Narrow(api::GetName(id))}, {"killer", Brief(it->second.killer)}});
                fallen_.insert(id);
                it = watched_.erase(it);
                continue;
            }
            it = now - it->second.since > std::chrono::minutes(1) ? watched_.erase(it) : std::next(it);
        }
    }

    // The creatures seen to die since the last call: a fight's dead, which
    // stage reset takes away, and not the bodies an area held before (a
    // reset that took every dead creature took twelve of the player's
    // earlier fights' with them, 2026-10-02).
    std::set<ObjectId> TakeFallen()
    {
        return std::exchange(fallen_, {});
    }

  private:
    std::set<ObjectId> fallen_;
    struct Entry
    {
        ObjectId killer = kInvalidObject;
        std::chrono::steady_clock::time_point since;
    };
    std::map<ObjectId, Entry> watched_;
};

// A row of one of the game's rule tables: its id, the designers' label, and
// its name in the player's language where the table has one.
Json Rule(std::int32_t table, std::int32_t row);

// An engine FSCommand, as a screen's DAExternalCommands calls it: the
// engine's handler leaves its answer in the movie's _root.fsc_retval (and a
// list's parallel second one in fsc_retval2), a list as its elements
// comma-joined. Asked of the first screen shown that has DAExternalCommands:
// the quickbar is not shown while a picker, a conversation or a full screen
// is up (turn-based-agent, 2026-10-05). Nothing if no screen took it.
struct ExternalAnswer
{
    std::string first;  // fsc_retval, as text
    std::string second; // fsc_retval2, as text; empty if none
};
std::optional<ExternalAnswer> ExternalCommand(std::string_view command, std::span<const dao::gui::Value> args);

// Another party member made the one the interface has selected, for a
// screen that acts on GUI.SelectedChar (the inventory, the tactics), and
// control given back to whom had it as the call ends, a refusal too (#86).
class SelectedFor
{
  public:
    SelectedFor(ObjectId who, ObjectId selected, std::string_view tool);
    SelectedFor(const SelectedFor &) = delete;
    SelectedFor &operator=(const SelectedFor &) = delete;
    SelectedFor(SelectedFor &&) = delete;
    SelectedFor &operator=(SelectedFor &&) = delete;
    ~SelectedFor();

    // Says the hand-over in the answer, if there was one.
    void Say(Json &answer) const;

  private:
    ObjectId before_;
    bool handed_ = false;
};

// The character the interface has selected (GUI.SelectedChar.ID), or the
// one controlled where the tree has none.
ObjectId SelectedCharacter();

DeathWatch &Deaths();

// A tool on the frame thread with its own arguments, the ones it needs
// marked required: what Read and the stage tools are built on.
inline ToolSpec Spec(std::string name, std::string description, Json properties,
                     const std::vector<std::string> &required)
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
    {
        schema["required"] = required;
    }
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Frame};
}

// A read of the game as a tool of its own: read-only, its own arguments, the
// ones it needs marked required. inspect's 27 kinds shared one tool, its
// arguments added together, a third of all the tools' listing: a tool is
// one concept (the user, 2026-10-09, #110). `listed` false for another
// tool's step (dialogue.read under dialogue).
inline ToolSpec Read(std::string name, std::string description, Json properties,
                     const std::vector<std::string> &required, bool listed = true, Thread thread = Thread::Frame)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.thread = thread;
    spec.readOnly = true;
    spec.listed = listed;
    return spec;
}

// A follower's tactic slots, as tactics' look answers them (Character.cpp).
ToolResult ReadTactics(const Watch &watch, const Json &raw);

// Each tool's registration, in its own file.
void AddInspect(Bench &bench, const Watch &watch);
void AddScript(Bench &bench, const Watch &watch);
void AddOrder(Bench &bench, const Watch &watch);
void AddStage(Bench &bench, const Watch &watch);
void AddScreenshot(Bench &bench, const Watch &watch);
void AddGame(Bench &bench, const Watch &watch);
void AddInput(Bench &bench, const Watch &watch);
void AddCamera(Bench &bench, const Watch &watch);
void AddDialogue(Bench &bench, const Watch &watch);
void AddScreen(Bench &bench, const Watch &watch);
void AddResources(Bench &bench);
void AddCharacter(Bench &bench, const Watch &watch); // character, abilities, equipment, inventory, item
void AddGui(Bench &bench, const Watch &watch);       // gui, controls
void AddStore(Bench &bench, const Watch &watch);
void AddWorldMap(Bench &bench, const Watch &watch); // worldmap
void AddTactics(Bench &bench, const Watch &watch);
void AddBoard(Bench &bench, const Watch &watch);

} // namespace eluvian::game
