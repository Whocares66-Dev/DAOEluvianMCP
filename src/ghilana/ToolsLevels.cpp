// Ghilana's tools over a level and what plays in it (#163), each of DAOLib's
// core readers put in an answer: a level's rooms, the models they place and
// the rooms' lights (level, dao/core/Level.h), its sun, sky, fog, clouds and
// frame effects (atmosphere, the .arl's), a cutscene's actors and their
// actions with its curves sampled (cutscene, Cutscene.h), and a stage's places
// and cameras (stage, Stage.h). Until then an agent ran DAOParagonEngine's
// daolib-sys examples for these, a compile each time and only in that
// repository (paragon-agent, 2026-10-19).
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/Text.h"

#include <dao/core/Conversation.h>
#include <dao/core/Cutscene.h>
#include <dao/core/Gff4.h>
#include <dao/core/Level.h>
#include <dao/core/Model.h>
#include <dao/core/Stage.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eluvian::ghilana::detail
{

namespace
{

namespace gff4 = dao::core::gff4;
namespace level = dao::core::level;
namespace cutscene = dao::core::cutscene;
namespace stage = dao::core::stage;
namespace model = dao::core::model;

ToolSpec Spec(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    schema["required"] = required;
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Listener, true};
}

// A file of the game's, read, and where its copy is.
struct Loaded
{
    std::vector<std::byte> bytes;
    Json from;
};

// The game's copy of a file; nothing, with why, if it has none or it cannot be read.
std::optional<Loaded> Load(const GameFiles &files, const std::string &name, std::string &why)
{
    const auto *copies = files.Find(name);
    if (copies == nullptr || copies->empty())
    {
        why = "the game has no " + name;
        return std::nullopt;
    }
    const Copy &copy = GameFiles::Choose(*copies);
    auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        return std::nullopt;
    }
    return Loaded{std::move(*bytes), Chosen(files, name, *copies, copy)};
}

// The game's copy of the file a call names; a ToolError, with where to look, if there is none.
Loaded Required(Ghilana &ghilana, const std::string &tool, const std::string &name, const std::string &look)
{
    (void)ghilana.Indexed();
    std::string why;
    auto loaded = Load(ghilana.Files(), name, why);
    if (!loaded)
    {
        throw ToolError(tool + ": " + why + "; " + look);
    }
    return std::move(*loaded);
}

// A name as a call gives it, in lower case, with `extension` if it has none.
std::string Named(const std::string &name, const std::string &extension)
{
    std::string out = Lower(name);
    if (Extension(out).empty())
    {
        out += "." + extension;
    }
    return out;
}

Json Place(const std::array<float, 3> &position, const std::array<float, 4> &orientation)
{
    return {{"position", position}, {"orientation", orientation}};
}

// ---------------------------------------------------------------- level

// A room of the level's layout and its definition, read if the game has it.
struct LoadedRoom
{
    const level::LevelRoom *listed = nullptr;
    std::optional<level::Room> room;
    std::string why;
};

std::vector<LoadedRoom> RoomsOf(const GameFiles &files, const level::LevelLayout &layout)
{
    std::vector<LoadedRoom> out;
    out.reserve(layout.rooms.size());
    for (const level::LevelRoom &listed : layout.rooms)
    {
        LoadedRoom loaded{&listed, std::nullopt, {}};
        const std::string name = Lower(Utf8(listed.file)) + ".rml";
        if (const auto file = Load(files, name, loaded.why))
        {
            const auto parsed = gff4::File::Parse(file->bytes);
            loaded.room = parsed ? level::RoomOf(*parsed) : std::nullopt;
            if (!loaded.room)
            {
                loaded.why = name + " is no room definition DAOLib reads";
            }
        }
        out.push_back(std::move(loaded));
    }
    return out;
}

// A room's lights are models it places like the rest: its layout, <room>_ll, and in 1,448 rooms
// a <room>_lc beside it (dao/core/Level.h), as DAOParagonEngine's level.rs reads them.
bool IsLayout(const level::Placed &placed)
{
    const std::string file = Lower(Utf8(placed.file));
    return file.ends_with("_ll") || file.ends_with("_lc");
}

Json RoomJson(const LoadedRoom &loaded)
{
    Json out = {{"id", loaded.listed->id},
                {"file", Utf8(loaded.listed->file)},
                {"lower", loaded.listed->lower},
                {"upper", loaded.listed->upper}};
    if (!loaded.room)
    {
        out["missing"] = loaded.why;
        return out;
    }
    const level::Room &room = *loaded.room;
    out["at"] = room.at;
    out["turn"] = room.turn;
    out["sees"] = room.sees;
    if (!room.connects.empty())
    {
        out["connects"] = room.connects;
    }
    out["models"] = std::ranges::count_if(room.models, [](const auto &m) { return !IsLayout(m); });
    out["trees"] = room.trees.size();
    return out;
}

// What a model's mesh nodes draw with: each part's material object (.mao), by its hierarchy.
Json MaterialsOf(const GameFiles &files, const std::string &modelName)
{
    std::string why;
    const auto file = Load(files, modelName + ".mmh", why);
    const auto parsed = file ? gff4::File::Parse(file->bytes) : std::nullopt;
    const auto tree = parsed ? model::HierarchyOf(*parsed) : std::nullopt;
    if (!tree)
    {
        return Json{{"missing", why.empty() ? modelName + ".mmh is no model DAOLib reads" : why}};
    }
    Json out = Json::array();
    for (const model::Node &node : tree->nodes)
    {
        if (node.mesh)
        {
            out.push_back(Json{{"node", Utf8(node.name)},
                               {"part", Utf8(node.mesh->part)},
                               {"chunk", Utf8(node.mesh->chunk)},
                               {"material", Utf8(node.mesh->material)}});
        }
    }
    return out;
}

Json PlacedJson(std::int32_t roomId, const level::Room &room, const level::Placed &placed)
{
    Json lightmaps = Json::array();
    for (const level::LightmapPart &part : placed.parts)
    {
        lightmaps.push_back(
            Json{{"part", Utf8(part.part)}, {"atlas", Utf8(part.atlas)}, {"offset_scale", part.offsetScale}});
    }
    return {{"room", roomId},
            {"model", Utf8(placed.file)},
            {"at", level::InLevel(room, placed, {0, 0, 0})},
            {"turn", level::Then(placed.turn, room.turn)},
            {"scale", placed.scale},
            {"lightmaps", std::move(lightmaps)}};
}

struct Lights
{
    Json lamps = Json::array();
    Json ambients = Json::array();
    Json probes = Json::array();
};

// A layout's lights, each in the level's space: the layout is placed in its room as a model is.
void AddLights(const GameFiles &files, std::int32_t roomId, const level::Room &room, const level::Placed &placed,
               Lights &out)
{
    const std::string name = Lower(Utf8(placed.file));
    std::string why;
    const auto file = Load(files, name + ".mmh", why);
    const auto parsed = file ? gff4::File::Parse(file->bytes) : std::nullopt;
    if (!parsed)
    {
        return;
    }
    const level::Layout layout = level::LayoutOf(*parsed);
    for (const level::Lamp &lamp : layout.lamps)
    {
        Json item = {{"room", roomId},
                     {"layout", name},
                     {"name", Utf8(lamp.name)},
                     {"at", level::InLevel(room, placed, lamp.at)},
                     {"colour", lamp.colour},
                     {"radius", lamp.radius},
                     {"static", lamp.isStatic},
                     {"affect_domain", lamp.affectDomain},
                     {"can_be_occluded", lamp.canBeOccluded}};
        if (lamp.variation != 0)
        {
            item["flicker"] = Json{{"variation", lamp.variation},
                                   {"period", lamp.period},
                                   {"period_delta", lamp.periodDelta},
                                   {"variation_tint", lamp.useVariationTint}};
        }
        out.lamps.push_back(std::move(item));
    }
    if (layout.ambient)
    {
        out.ambients.push_back(Json{{"room", roomId},
                                    {"layout", name},
                                    {"name", Utf8(layout.ambient->name)},
                                    {"colour", layout.ambient->colour}});
    }
    for (const level::Probe &probe : layout.probes)
    {
        out.probes.push_back(Json{{"room", roomId},
                                  {"layout", name},
                                  {"name", Utf8(probe.name)},
                                  {"at", level::InLevel(room, placed, probe.at)},
                                  {"cube", Utf8(probe.cube)},
                                  {"irradiance_file", Utf8(probe.irradianceFile)},
                                  {"baked", std::ranges::any_of(probe.irradiance, [](float f) { return f != 0; })}});
    }
}

// A level's placed models as a call keeps them, paged, and its rooms' lights.
struct Placing
{
    Placing(const GameFiles &in, const Page &paged, std::string word, bool withMaterials)
        : files(in), page(paged), filter(std::move(word)), materials(withMaterials)
    {
    }

    const GameFiles &files;
    const Page &page;
    std::string filter;
    bool materials = false;
    std::size_t matched = 0;
    Json list = Json::array();
    std::map<std::string, Json> materialsOf; // read once a model
    Lights lights;

    void Add(std::int32_t roomId, const level::Room &room)
    {
        for (const level::Placed &placed : room.models)
        {
            if (IsLayout(placed))
            {
                AddLights(files, roomId, room, placed, lights);
                continue;
            }
            const std::string modelName = Lower(Utf8(placed.file));
            if (!filter.empty() && modelName.find(filter) == std::string::npos)
            {
                continue;
            }
            if (page.Takes(matched++, list.size()))
            {
                list.push_back(PlacedJson(roomId, room, placed));
                if (materials)
                {
                    auto [at, added] = materialsOf.try_emplace(modelName);
                    if (added)
                    {
                        at->second = MaterialsOf(files, modelName);
                    }
                    list.back()["materials"] = at->second;
                }
            }
        }
    }
};

Json LevelTool(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "level");
    const std::string name = Named(args.String("name"), "arl");
    const std::string filter = Lower(args.String("model", ""));
    const std::optional<std::int64_t> onlyRoom =
        args.Has("room") ? std::optional(args.Integer("room", 0, 0, INT32_MAX)) : std::nullopt;
    const bool materials = args.Bool("materials", false);
    const bool lights = args.Bool("lights", false);
    const Page page(args, 100, 2000);
    const Loaded arl = Required(ghilana, "level", name, "`resources` with type arl lists the levels");
    const auto file = gff4::File::Parse(arl.bytes);
    const auto layout = file ? level::LevelLayoutOf(*file) : std::nullopt;
    if (!layout)
    {
        throw ToolError("level: " + name + " is no level layout DAOLib reads; `resource` shows what it holds");
    }
    Placing placing(ghilana.Files(), page, filter, materials);
    Json out = {
        {"level", Utf8(layout->layoutName)}, {"name", Utf8(layout->name)}, {"id", layout->id}, {"layout", arl.from}};
    Json roomList = Json::array();
    for (const LoadedRoom &loaded : RoomsOf(placing.files, *layout))
    {
        roomList.push_back(RoomJson(loaded));
        if (loaded.room && (!onlyRoom || *onlyRoom == loaded.listed->id))
        {
            placing.Add(loaded.listed->id, *loaded.room);
        }
    }
    out["rooms"] = std::move(roomList);
    page.Answer(out, placing.matched, "placed", std::move(placing.list));
    Lights &found = placing.lights;
    out["lights"] =
        Json{{"lamps", found.lamps.size()}, {"ambients", found.ambients.size()}, {"probes", found.probes.size()}};
    if (lights)
    {
        out["lamps"] = std::move(found.lamps);
        out["ambients"] = std::move(found.ambients);
        out["probes"] = std::move(found.probes);
    }
    out["units"] = "metres in the level's space, Z up; a turn is a quaternion x, y, z, w";
    out["next"] = "`atmosphere` gives the level's sun, sky and fog; `model` a placed model's nodes and materials";
    return out;
}

// ---------------------------------------------------------------- atmosphere

Json FloatsJson(const std::vector<level::NamedFloats> &values)
{
    Json out = Json::object();
    for (const level::NamedFloats &value : values)
    {
        out[value.name] = value.floats.size() == 1 ? Json(value.floats[0]) : Json(value.floats);
    }
    return out;
}

Json AtmosphereTool(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "atmosphere");
    const std::string name = Named(args.String("level"), "arl");
    const Loaded arl = Required(ghilana, "atmosphere", name, "`resources` with type arl lists the levels");
    const auto file = gff4::File::Parse(arl.bytes);
    const auto layout = file ? level::LevelLayoutOf(*file) : std::nullopt;
    if (!layout)
    {
        throw ToolError("atmosphere: " + name + " is no level layout DAOLib reads; `resource` shows what it holds");
    }
    Json sun = {{"direction", layout->sunDirection},
                {"colour", layout->sunColour},
                {"force_character_lighting", layout->forceCharacterLighting}};
    if (layout->sunColourOnCharacters)
    {
        sun["colour_on_characters"] = *layout->sunColourOnCharacters;
    }
    if (layout->sunOccludedOnCharacters)
    {
        sun["occluded_on_characters"] = *layout->sunOccludedOnCharacters;
    }
    Json effects = Json::array();
    for (const level::FrameEffect &effect : layout->effects)
    {
        Json parameters = Json::object();
        for (const auto &[parameter, value] : effect.parameters)
        {
            parameters[Utf8(parameter)] = value;
        }
        effects.push_back(Json{{"file", Utf8(effect.file)}, {"name", Utf8(effect.name)}, {"parameters", parameters}});
    }
    Json local = Json::array();
    for (const level::LocalWind &wind : layout->localWinds)
    {
        local.push_back(Json{{"at", wind.at}, {"file", Utf8(wind.file)}, {"radius", wind.radius}});
    }
    Json out = {{"level", Utf8(layout->layoutName)},
                {"name", Utf8(layout->name)},
                {"layout", arl.from},
                {"sky_dome", layout->skyDome.empty() ? Json() : Json(Utf8(layout->skyDome))},
                {"sun", std::move(sun)},
                {"atmosphere", FloatsJson(layout->atmosphere)},
                {"clouds", FloatsJson(layout->clouds)},
                {"frame_effects", std::move(effects)},
                {"wind", Json{{"global", Utf8(layout->globalWind)}, {"local", std::move(local)}}},
                {"tree_tint", layout->treeTint}};
    out["note"] = "the file's values, before any script's SetAtmosphere; a field DAOLib has no name for keeps its "
                  "number. Eluvian's game tools read the sky the running game draws";
    return out;
}

// ---------------------------------------------------------------- cutscene

// What an action's template is, as DAOLib's Cutscene.h reads the game's 3,158 cutscenes; null
// for the templates whose use is unread (ACTN, ASMK).
const char *KindName(const cutscene::Action &action)
{
    static const std::map<std::string, const char *, std::less<>> kNames{{"AANI", "animation"},
                                                                         {"ASLN", "spoken line"},
                                                                         {"AANF", "a line's face alone"},
                                                                         {"ACHT", "head tracking"},
                                                                         {"AJSP", "stage place"},
                                                                         {"AJSC", "stage camera"},
                                                                         {"AFBE", "frame effect"},
                                                                         {"AACC", "active camera"},
                                                                         {"ASHK", "shake"},
                                                                         {"ACSP", "sound"},
                                                                         {"ADW_", "weapons drawn"},
                                                                         {"ACVI", "visibility"},
                                                                         {"ALNK", "link to an actor"},
                                                                         {"AGOR", "gore"},
                                                                         {"APBM", "movie"},
                                                                         {"AFOG", "fog"}};
    if (action.kind == "TRAK")
    {
        if (action.type == 17)
        {
            return "position track";
        }
        return action.type == 18 ? "orientation track" : "track";
    }
    const auto found = kNames.find(action.kind);
    return found != kNames.end() ? found->second : nullptr;
}

Json CurveJson(const cutscene::Curve &curve)
{
    Json vertices = Json::array();
    for (const cutscene::Vertex &v : curve.vertices)
    {
        vertices.push_back(Json::array({v.time, v.value}));
    }
    Json transitions = Json::array();
    for (const cutscene::Transition &t : curve.transitions)
    {
        transitions.push_back(Json{{"type", t.type},
                                   {"control1", Json::array({t.control1.time, t.control1.value})},
                                   {"control2", Json::array({t.control2.time, t.control2.value})}});
    }
    return {{"base", curve.base}, {"vertices", std::move(vertices)}, {"transitions", std::move(transitions)}};
}

// The fields of an action's own template that it has.
void AddOwnFields(const cutscene::Action &a, Json &out)
{
    const auto text = [&](const char *key, const std::wstring &value) {
        if (!value.empty())
        {
            out[key] = Utf8(value);
        }
    };
    text("animation", a.animation);
    text("blend_tree", a.blendTree);
    if (a.kind == "AANI")
    {
        out["speed"] = a.speed;
        out["start_offset"] = a.startOffset;
        out["gad"] = Json{{"play", a.playGad},
                          {"blend", a.blendGad},
                          {"extend", a.extendGad},
                          {"apply_future", a.applyFutureGads},
                          {"position_keys", a.gadPositions.size()},
                          {"orientation_keys", a.gadOrientations.size()}};
    }
    text("vo_bank", a.voBank);
    text("lipsync", a.lipsync);
    text("sound", a.sound);
    if (a.actor >= 0)
    {
        out["actor"] = a.actor;
    }
    if (a.node >= 0)
    {
        out["node"] = a.node;
    }
    text("tag", a.tag);
    text("look_at", a.lookAt);
    if (a.kind == "AJSC")
    {
        out["stage_camera_default"] = a.stageCameraDefault;
        out["stage_camera_henchman"] = a.stageCameraHenchman;
    }
    if (a.kind == "ACVI")
    {
        out["visible"] = a.visible;
    }
    if (a.kind == "ADW_")
    {
        out["draw_main"] = a.drawMain;
        out["draw_off"] = a.drawOff;
    }
    text("effect", a.effect);
    if (!a.parameters.empty())
    {
        Json parameters = Json::array();
        for (const cutscene::EffectParameter &p : a.parameters)
        {
            Json item = {{"name", Utf8(p.name)}, {"value", p.value}};
            if (p.curve >= 0)
            {
                item["curve"] = p.curve;
            }
            if (p.vector >= 0)
            {
                item["element"] = p.vector;
            }
            parameters.push_back(std::move(item));
        }
        out["parameters"] = std::move(parameters);
    }
    text("movie", a.movie);
    if (a.fog)
    {
        out["fog"] = Json{{"enabled", a.fog->enabled},
                          {"colour", a.fog->color},
                          {"intensity", a.fog->intensity},
                          {"cap", a.fog->cap},
                          {"zenith", a.fog->zenith}};
    }
}

// What a cutscene's answer is asked to hold, and the text of its lines.
struct CutAsked
{
    std::optional<std::int64_t> actor;
    std::string actorName;
    std::string kind;
    bool curves = false;
    std::optional<float> at; // on the cutscene's clock
    std::function<Json(std::uint32_t)> line;
};

Json ActionJson(const cutscene::Action &a, const CutAsked &asked)
{
    // A stop of 1e10 is "to the end" (Cutscene.h).
    Json out = {{"kind", a.kind},
                {"type", a.type},
                {"start", a.start},
                {"stop", a.stop >= 1e9F ? Json("end") : Json(a.stop)},
                {"category", a.category}};
    if (const char *what = KindName(a))
    {
        out["what"] = what;
    }
    AddOwnFields(a, out);
    if (a.text.strref != 0xFFFFFFFF && (a.kind == "ASLN" || a.kind == "AANF"))
    {
        out["line"] = asked.line(a.text.strref);
    }
    out["curves"] = a.curves.size();
    if (asked.curves && !a.curves.empty())
    {
        Json list = Json::array();
        for (const cutscene::Curve &curve : a.curves)
        {
            list.push_back(CurveJson(curve));
        }
        out["curve_keys"] = std::move(list);
    }
    if (asked.at && !a.curves.empty())
    {
        Json values = Json::array();
        for (const cutscene::Curve &curve : a.curves)
        {
            values.push_back(cutscene::ValueAt(curve, *asked.at));
        }
        out["values_at"] = std::move(values);
    }
    return out;
}

bool Wanted(const cutscene::Actor &actor, const CutAsked &asked)
{
    if (asked.actor && std::cmp_not_equal(*asked.actor, actor.id))
    {
        return false;
    }
    return asked.actorName.empty() || Lower(Utf8(actor.name)) == asked.actorName;
}

Json ActorJson(const cutscene::Actor &actor, const CutAsked &asked)
{
    Json out = {{"id", actor.id},
                {"name", Utf8(actor.name)},
                {"model", Utf8(actor.model)},
                {"scale", actor.scale},
                {"master", actor.master != 0},
                {"origin", Place(actor.originPosition, actor.originOrientation)},
                {"final", Place(actor.finalPosition, actor.finalOrientation)}};
    if (!actor.creature.empty())
    {
        out["creature"] = Utf8(actor.creature);
    }
    if (!actor.mappingTag.empty())
    {
        out["mapping_tag"] = Utf8(actor.mappingTag);
        out["mapping_required"] = actor.mappingRequired != 0;
    }
    if (actor.cameraTarget >= 0)
    {
        out["camera_target"] = actor.cameraTarget;
    }
    if (actor.pose >= 0)
    {
        out["pose"] = actor.pose;
    }
    if (asked.at)
    {
        const cutscene::Placement placed = cutscene::PlacementAt(actor, *asked.at);
        out["placement_at"] = Place(placed.position, placed.orientation);
    }
    Json actions = Json::array();
    for (const cutscene::Action &action : actor.actions)
    {
        if (asked.kind.empty() || Lower(action.kind) == asked.kind)
        {
            actions.push_back(ActionJson(action, asked));
        }
    }
    out["actions"] = std::move(actions);
    return out;
}

// The cutscene a call names: a .cut, or a conversation's line's, held in the line or named by it.
cutscene::Cutscene CutsceneNamed(Ghilana &ghilana, const Args &args, Json &out)
{
    if (args.Has("conversation"))
    {
        const std::string name = Named(args.String("conversation"), "dlg");
        const auto index = static_cast<std::uint32_t>(args.Integer("line", 0, 0, INT32_MAX));
        const Loaded dlg = Required(ghilana, "cutscene", name, "`resources` with type dlg lists the conversations");
        const auto conversation = dao::core::ConversationFile::Parse(dlg.bytes);
        const auto line = conversation ? conversation->Line(index) : std::nullopt;
        if (!line)
        {
            throw ToolError("cutscene: " + name + " has no line " + std::to_string(index) +
                            "; `resource` shows its lines");
        }
        out["conversation"] = dlg.from;
        out["line"] = index;
        out["line_stage"] = Utf8(line->stageName);
        out["stage_at_speaker"] = line->stageAtCurrentLocation;
        if (line->cutsceneInLine)
        {
            out["held_in_line"] = true;
            return *conversation->LineCutscene(index);
        }
        if (line->cutscene.empty())
        {
            throw ToolError("cutscene: line " + std::to_string(index) + " of " + name + " plays no cutscene");
        }
        out["held_in_line"] = false;
        return CutsceneNamed(ghilana, Args(Json{{"name", Utf8(line->cutscene)}}, "cutscene"), out);
    }
    const std::string name = Named(args.String("name"), "cut");
    const Loaded cut = Required(ghilana, "cutscene", name, "`resources` with type cut lists the cutscenes");
    const auto file = gff4::File::Parse(cut.bytes);
    auto read = file ? cutscene::CutsceneOf(*file) : std::nullopt;
    if (!read)
    {
        throw ToolError("cutscene: " + name + " is no cutscene DAOLib reads; `resource` shows what it holds");
    }
    out["cutscene"] = cut.from;
    return std::move(*read);
}

Json CutsceneTool(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "cutscene");
    if (!args.Has("name") && !args.Has("conversation"))
    {
        throw ToolError("cutscene: give `name` (a .cut) or `conversation` and `line`");
    }
    Json out = Json::object();
    const cutscene::Cutscene cut = CutsceneNamed(ghilana, args, out);
    CutAsked asked;
    if (args.Has("actor"))
    {
        if (const Json &actor = args.Raw()["actor"]; actor.is_number_integer())
        {
            asked.actor = actor.get<std::int64_t>();
        }
        else
        {
            asked.actorName = Lower(args.String("actor"));
        }
    }
    asked.kind = Lower(args.String("kind", ""));
    asked.curves = args.Bool("curves", false);
    if (args.Has("at"))
    {
        asked.at = cutscene::FrameTime(static_cast<float>(args.Number("at")), cut.fps);
        out["clock_at"] = *asked.at;
    }
    const auto &strings = ghilana.TalkStrings(false);
    asked.line = [&strings](std::uint32_t strref) {
        Json line = {{"strref", strref}};
        if (const auto found = strings.Lookup(strref))
        {
            line["text"] = Utf8(found->text);
        }
        return line;
    };
    out["area"] = Utf8(cut.area);
    out["run_time"] = cut.runTime;
    out["fps"] = cut.fps;
    out["end_script"] = Utf8(cut.endScript);
    out["blend_tree"] = Utf8(cut.blendTree);
    if (!cut.stage.empty())
    {
        out["stage"] = Utf8(cut.stage);
    }
    out["play_until_vo_completes"] = cut.playUntilVoCompletes != 0;
    out["area_required"] = cut.areaRequired != 0;
    if (!cut.henchmanTag.empty())
    {
        out["henchman_tag"] = Utf8(cut.henchmanTag);
    }
    Json resources = Json::array();
    for (const std::wstring &resource : cut.resources)
    {
        resources.push_back(Utf8(resource));
    }
    out["resources"] = std::move(resources);
    Json actors = Json::array();
    for (const cutscene::Actor &actor : cut.actors)
    {
        if (Wanted(actor, asked))
        {
            actors.push_back(ActorJson(actor, asked));
        }
    }
    out["actors"] = std::move(actors);
    out["units"] = "seconds on the cutscene's clock; places in the stage's frame where it has a stage, else the "
                   "world's; a turn is a quaternion x, y, z, w";
    if (!cut.stage.empty() || out.contains("conversation"))
    {
        out["next"] = "`stage` with the stage's name gives its places and cameras";
    }
    return out;
}

// ---------------------------------------------------------------- stage

Json StageTool(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "stage");
    const std::string name = Named(args.String("name"), "stg");
    const Loaded stg = Required(ghilana, "stage", name, "`resources` with type stg lists the stages");
    const auto file = gff4::File::Parse(stg.bytes);
    const auto read = file ? stage::StageOf(*file) : std::nullopt;
    if (!read)
    {
        throw ToolError("stage: " + name + " is no stage DAOLib reads; `resource` shows what it holds");
    }
    Json places = Json::array();
    for (const stage::Place &place : read->places)
    {
        Json item = {{"tag", Utf8(place.tag)}, {"position", place.position}, {"orientation", place.orientation}};
        if (!place.defaultCamera.empty())
        {
            item["default_camera"] = Utf8(place.defaultCamera);
        }
        places.push_back(std::move(item));
    }
    Json cameras = Json::array();
    for (const stage::Camera &camera : read->cameras)
    {
        Json inShot = Json::array();
        for (const std::wstring &tag : camera.placesInShot)
        {
            inShot.push_back(Utf8(tag));
        }
        cameras.push_back(Json{{"tag", Utf8(camera.tag)},
                               {"position", camera.position},
                               {"orientation", camera.orientation},
                               {"fov", camera.fov},
                               {"places_in_shot", std::move(inShot)},
                               {"looking_from", Utf8(camera.lookingFrom)},
                               {"looking_at_primary", Utf8(camera.lookingAtPrimary)},
                               {"looking_at_secondary", Utf8(camera.lookingAtSecondary)},
                               {"looking_at_type", camera.lookingAtType}});
    }
    return {{"stage", stg.from},
            {"places", std::move(places)},
            {"cameras", std::move(cameras)},
            {"units", "metres in the stage's own frame, Z up; a turn is a quaternion x, y, z, w; fov in degrees"}};
}

} // namespace

void RegisterLevelTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();
    Json levelProperties = {
        {"name", {{"type", "string"}, {"description", "The level, \"lak303d\" (its .arl)."}}},
        {"model", {{"type", "string"}, {"description", "Only the placed models whose name has this word: fxe, lamp."}}},
        {"room", {{"type", "integer"}, {"description", "Only the models and lights of this room's number."}}},
        {"materials",
         {{"type", "boolean"},
          {"description", "Each placed model's material objects by part, from its hierarchy. Default false."}}},
        {"lights",
         {{"type", "boolean"},
          {"description", "List the lamps, ambient lights and probes, not only count them. Default false."}}}};
    levelProperties.update(PageProperties(100, 2000));
    tools.Add(
        Spec("level",
             "A level's layout (.arl) and its rooms (.rml): each room's number, bounds, place and the rooms seen from "
             "it; the models the rooms place, each in the level's space (place, turn, scale, its lightmaps' atlases), "
             "paged, with `model` to keep those whose name has a word, `room` one room's, and `materials` each "
             "model's material objects by part (from its .mmh); and the rooms' lights counted, listed with `lights`: "
             "lamps (colour, radius, flicker, affect domain: 2 in a room's _ll layout, 1 in its _lc), ambient lights "
             "and light probes, each in the level's space. A level's name is an area's layout, \"lak303d\".",
             levelProperties, {"name"}),
        [&ghilana](const Json &args) -> ToolResult { return LevelTool(ghilana, args); });
    tools.Add(Spec("atmosphere",
                   "A level's sky as its layout (.arl) has it: the sky dome, the sun (direction, colour, the colour on "
                   "characters), the atmosphere's scattering, fog and moon and the clouds, each value by its field's "
                   "name, the frame effects with their parameters (da_bloom, da_hsvmatrix), and the wind. These are "
                   "the file's, before a script's SetAtmosphere.",
                   {{"level", {{"type", "string"}, {"description", "The level, \"oth000d\" (its .arl)."}}}}, {"level"}),
              [&ghilana](const Json &args) -> ToolResult { return AtmosphereTool(ghilana, args); });
    tools.Add(
        Spec("cutscene",
             "A cutscene decoded: its area, length, frames a second, end script, blend tree, stage, and its actors "
             "(model, creature, the area's tag that plays it, origin and final places), each with its actions in "
             "order: kind (animation, spoken line with its text, position or orientation track, stage camera, sound, "
             "frame effect, fog...), start and stop, and the action's own fields. `curves` adds each curve's keys; "
             "`at` samples every curve and places every actor at that time, as the engine does. Name a .cut, or a "
             "conversation's line that plays one, held in the line or named by it.",
             {{"name", {{"type", "string"}, {"description", "The cutscene, \"bhmcs_intro\" (its .cut)."}}},
              {"conversation",
               {{"type", "string"}, {"description", "In place of `name`: a conversation, \"bhm100_jowan\"."}}},
              {"line", {{"type", "integer"}, {"description", "With `conversation`: the line's index in its list."}}},
              {"actor",
               {{"description", "Only this actor, by its id (a number) or its name (\"Jowan\")."},
                {"type", Json::array({"integer", "string"})}}},
              {"kind", {{"type", "string"}, {"description", "Only actions of this template: AANI, TRAK."}}},
              {"curves",
               {{"type", "boolean"}, {"description", "Each curve's vertices and transitions. Default false."}}},
              {"at",
               {{"type", "number"},
                {"description", "Seconds into the cutscene: every curve's value and every actor's place then."}}}},
             {}),
        [&ghilana](const Json &args) -> ToolResult { return CutsceneTool(ghilana, args); });
    tools.Add(Spec("stage",
                   "A stage (.stg) a cutscene or a conversation's line is set on: its places (tag, position, "
                   "orientation, default camera) and its cameras (position, orientation, field of view, the places in "
                   "shot, and what it looks from and at, with its LookingAtType), in the stage's own frame.",
                   {{"name", {{"type", "string"}, {"description", "The stage, \"jowan_attack\" (its .stg)."}}}},
                   {"name"}),
              [&ghilana](const Json &args) -> ToolResult { return StageTool(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
