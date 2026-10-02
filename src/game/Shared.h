// What the game's tools share: the answers each gives of a creature (its
// summary, its room, its effects and abilities, the command it carries out),
// the checks before any of them touches the game, the watch on deaths, and
// each tool's own registration, which GameTools.cpp calls. One file per
// tool: Inspect, Script, Order, Stage, Screenshot, Game, Input, Camera, Dialogue
// and Screen. Everything here is on the game's thread.
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
#include <utility>
#include <vector>

namespace eluvian::game
{

namespace api = dao::script::api;
namespace k = dao::script::constants;
using dao::kInvalidObject;
using dao::ObjectId;

inline constexpr std::string_view kCreatureHint = "a creature, as inspect kind=creatures lists them";

// Refuses while the game is starting or an area loads.
void RequireWorld(const Watch &watch, std::string_view tool);

// An object by id, name and tag; null for none.
Json Brief(ObjectId id);
std::vector<ObjectId> Objects(const dao::script::Array &array);
float Distance(const dao::Vector &a, const dao::Vector &b);
Json Depletable(ObjectId id, std::int32_t property);
std::string Side(ObjectId id, ObjectId hero, const std::set<ObjectId> &party);
std::set<ObjectId> PartyOf(ObjectId hero);

Json PathJson(const dao::world::WalkPath &path, const dao::Vector &from, bool waypoints);
const dao::Command *CurrentCommand(ObjectId id);
Json CommandJson(ObjectId id);
bool IsMove(dao::core::CommandType type);
Json PathNow(ObjectId id, const dao::Vector &from, bool waypoints);

Json PopupJson();
Json LineJson(const dao::dialogue::Line &line, bool female);
bool HeroIsFemale();
Json RepliesJson(std::span<const dao::dialogue::Line> replies, bool female);

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
Json AbilityJson(std::int32_t ability);
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
                it = watched_.erase(it);
                continue;
            }
            it = now - it->second.since > std::chrono::minutes(1) ? watched_.erase(it) : std::next(it);
        }
    }

  private:
    struct Entry
    {
        ObjectId killer = kInvalidObject;
        std::chrono::steady_clock::time_point since;
    };
    std::map<ObjectId, Entry> watched_;
};

DeathWatch &Deaths();

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
void AddCharacter(Bench &bench, const Watch &watch); // inspect's character, equipment, inventory and item

} // namespace eluvian::game
