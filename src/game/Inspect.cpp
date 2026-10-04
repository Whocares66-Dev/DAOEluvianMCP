// `inspect`: the game read, by `kind`: the game itself, its message box,
// the conversation on the screen, the placeables, the creatures, one
// creature in full, and the geometry between two. Reads only; plugins add
// kinds of their own through the C ABI.
#include "game/Shared.h"

#include "core/Placeables.h"

#include "game/Capture.h"

#include "dao/Data.h"

namespace eluvian::game
{

namespace
{

// Where an area transition leads. The scripts keep it as local strings on
// the placeable or the trigger (placeable_h.nss, trigger_core.nss; read by
// lib-agent, 2026-10-01). A door that chooses its floor when it is used
// sets them then, so a read before can be empty or the last use's; an exit
// to the world map names the world map as its area.
Json LeadsTo(ObjectId id, bool trigger)
{
    // A placeable that leads nowhere holds the template's "none" there
    // (every chest and door of the ruins but the two ways out, 2026-10-02).
    auto area = Narrow(api::GetLocalString(id, trigger ? L"TRIGGER_AT_DEST_AREA_TAG" : L"PLC_AT_DEST_AREA_TAG"));
    if (area.empty() || area == "none")
    {
        return nullptr;
    }
    return Json{{"area", std::move(area)},
                {"waypoint", Narrow(api::GetLocalString(id, trigger ? L"TRIGGER_AT_DEST_TAG" : L"PLC_AT_DEST_TAG"))}};
}

// One quest of the journal, from the data tree its window is bound to:
// Journal.PlotList.<i> for those under way, Journal.CompletedPlots.<i> for
// those done, Journal.ActivePlot for the one marked (lib-agent, from the
// constructors of Journal and JournalEntry and the paths in journal.gfx,
// 2026-10-01). A list has no length to ask: its members are walked until
// one has no ResRef. Name and Description come in the player's language.
Json JournalEntry(const std::wstring &at)
{
    if (dao::data::Find(at + L"ResRef") == nullptr)
    {
        return nullptr;
    }
    const auto text = [&at](std::wstring_view field) {
        const auto *attribute = dao::data::Find(at + std::wstring(field));
        return attribute != nullptr ? Narrow(attribute->Text()) : std::string();
    };
    // Completed and Updated are read as text, "0" or "1": their number was
    // 0 for both in play (2026-10-02).
    const auto yes = [&text](std::wstring_view field) { return text(field) == "1"; };
    Json entry = {{"plot", text(L"ResRef")}, {"name", text(L"Name")}};
    // The entry showing marks its objective <emp>so</emp> and breaks lines
    // with \r\n ("<emp>Investigate the cave</emp>\r\nYou've found ..."): the
    // marked part is given apart, and the text without the marks.
    const std::string shown = text(L"Description");
    std::string plain;
    std::string objective;
    bool marked = false;
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
        if (shown.compare(i, 5, "<emp>") == 0 || shown.compare(i, 6, "</emp>") == 0)
        {
            marked = shown[i + 1] != '/';
            i = shown.find('>', i);
            continue;
        }
        if (shown[i] == '\r')
        {
            continue;
        }
        plain += shown[i];
        if (marked)
        {
            objective += shown[i];
        }
    }
    entry["description"] = std::move(plain);
    if (!objective.empty())
    {
        entry["objective"] = std::move(objective);
    }
    for (const auto &[key, field] : {std::pair{"group", L"PlotGroup"}, std::pair{"parent", L"ParentPlot"},
                                     std::pair{"area", L"AreaTag"}, std::pair{"offer", L"OfferId"}})
    {
        if (auto value = text(field); !value.empty())
        {
            entry[key] = std::move(value);
        }
    }
    entry["updated"] = yes(L"Updated");
    entry["completed"] = yes(L"Completed");
    return entry;
}

Json JournalList(std::wstring_view list)
{
    constexpr int kMostEntries = 512;
    Json entries = Json::array();
    for (int i = 0; i < kMostEntries; ++i)
    {
        Json entry = JournalEntry(std::format(L"Journal.{}.{}.", list, i));
        if (entry.is_null())
        {
            break;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

namespace
{

// Brief reads (#16): a thing a line, what a player wants of it on arriving,
// for an agent that pays for every field of every creature on every look:
// thirty creatures in full were a page of JSON, and the playthrough's
// driver had two verbs that only boiled them down (turn-based-agent,
// 2026-10-03).
std::string Metres(double metres)
{
    return std::format("{:.1f} m", metres);
}

std::string CreatureLine(const Json &c)
{
    std::string line =
        std::format("{} {} [{}] {}", c.value("id", ""), c.value("name", ""), c.value("tag", ""), c.value("side", ""));
    const Json health = c.value("health", Json::object());
    line += std::format(", {:.0f}/{:.0f} hp", health.value("current", 0.0), health.value("max", 0.0));
    if (c.value("dead", false))
    {
        line += ", dead";
    }
    return line + ", " + Metres(c.value("distance", 0.0));
}

std::string PlaceableLine(const Json &p)
{
    std::string line = std::format("{} {} [{}] {}, state {}", p.value("id", ""), p.value("name", ""),
                                   p.value("tag", ""), Metres(p.value("distance", 0.0)), p.value("state", 0));
    const Json lock = p.value("lock", Json::object());
    // A pick level is how hard its lock would be to pick, not that it is
    // locked: an open cave entrance in the forest has one (seen in play,
    // 2026-10-03). The state says whether it is shut.
    if (lock.value("pickLevel", 0) > 0)
    {
        line += std::format(", pick {}", lock.value("pickLevel", 0));
    }
    if (lock.value("keyRequired", false))
    {
        line += ", key " + lock.value("keyTag", std::string());
    }
    if (!p.value("interactive", true))
    {
        line += ", not interactive";
    }
    if (p.contains("leadsTo"))
    {
        line += " -> " + p.at("leadsTo").value("area", std::string());
    }
    return line;
}

// A placeable's state in a word, as its state controller names it (#44):
// the controller read as the scripts read it (core_h.nss,
// GetPlaceableStateCntTable). Null where the controller names none.
constexpr std::int32_t kTablePlaceableTypes = 93;

Json PlaceableStateWord(ObjectId id)
{
    const std::string controller =
        Narrow(api::GetM2DAString(kTablePlaceableTypes, L"StateController", api::GetAppearanceType(id)));
    const auto word = PlaceableStateName(controller, api::GetPlaceableState(id));
    return word.empty() ? Json(nullptr) : Json(std::string(word));
}

// What stands in a walk's way: a placeable with its state and its lock, as
// kind=placeables reads them, so a door the story keeps shut (pick level
// 10000) is told from one a use opens (#33), and where it leads if it is an
// exit, which no use on the way opens (#38).
Json BlockerJson(ObjectId id)
{
    Json blocker = Brief(id);
    if (id != kInvalidObject && api::GetObjectType(id) == k::OBJECT_TYPE_PLACEABLE)
    {
        blocker["state"] = api::GetPlaceableState(id);
        blocker["stateName"] = PlaceableStateWord(id);
        blocker["interactive"] = api::GetObjectInteractive(id) != 0; // nothing uses one that is not (#50)
        const std::wstring key = api::GetPlaceableKeyTag(id);
        blocker["lock"] = Json{{"keyRequired", api::GetPlaceableKeyRequired(id) != 0},
                               {"keyTag", Narrow(key)},
                               {"pickLevel", api::GetPlaceablePickLockLevel(id)}};
        // Whether the party carries the key: a door whose key is required
        // opens to nothing else (#40). The party's things are the hero's.
        if (!key.empty())
        {
            blocker["lock"]["keyHeld"] = api::CountItemsByTag(api::GetHero(), key) > 0;
        }
        if (Json leads = LeadsTo(id, false); !leads.is_null())
        {
            blocker["leadsTo"] = std::move(leads);
        }
    }
    return blocker;
}

std::string ExitLine(const Json &e)
{
    std::string line = std::format(
        "{} {} [{}] {} to {}, {}", e.value("id", ""), e.value("name", ""), e.value("tag", ""), e.value("kind", ""),
        e.value(Json::json_pointer("/leadsTo/area"), std::string()), Metres(e.value("distance", 0.0)));
    if (e.value("blocker", false))
    {
        line += ", a blocker: its use shows a line and leaves nothing";
    }
    return e.value("active", true) ? line : line + ", inactive";
}

Json Lines(const Json &things, std::string (*line)(const Json &))
{
    Json lines = Json::array();
    for (const auto &thing : things)
    {
        lines.push_back(line(thing));
    }
    return lines;
}

} // namespace

void AddInspect(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(ToolSpec{
        "inspect",
        "Read the game: its state, the creatures in the area, one creature in full, the geometry between "
        "two. Creatures are named by id (\"0x7d9\", as the logs write them); pass those ids to "
        "every other tool.",
        Json{{"type", "object"},
             {"properties",
              {{"id", {{"description", "kind=creature and kind=geometry: the creature; default the one controlled"}}},
               {"target", {{"description", "kind=geometry: the creature to measure to"}}},
               {"point", {{"description", "kind=geometry: the point to measure to, {x, y, z} or [x, y, z]"}}},
               {"path", {{"type", "string"}, {"description", "kind=data: a dotted path in the data tree"}}},
               {"paths", {{"type", "array"}, {"description", "kind=data: several paths, answered together"}}},
               {"members",
                {{"type", "boolean"},
                 {"description", "kind=data: the list at `path`: its members' paths and how many"}}},
               {"full", {{"type", "boolean"}, {"description", "kind=inventory: each item in full"}}},
               {"plot", {{"type", "string"}, {"description", "kind=plot: the plot's resource, like bed000pt_main"}}},
               {"flag", {{"description", "kind=plot: one flag, by number or name"}}},
               {"completed", {{"type", "boolean"}, {"description", "kind=journal: list the quests done too"}}},
               {"waypoints", {{"type", "boolean"}, {"description", "kind=geometry: list the paths' points"}}},
               {"walk",
                {{"type", "boolean"},
                 {"description", "kind=geometry: also search the path the creature would walk there: its length, "
                                 "or what is in the way"}}},
               {"trace",
                {{"type", "boolean"},
                 {"description", "kind=geometry with walk: the search's own record (each slice's answer, a line "
                                 "for a log), for whoever reads the pathfinder"}}},
               {"near",
                {{"description", "kind=creatures: measure distances from this creature; default "
                                 "the one controlled"}}},
               {"radius",
                {{"type", "number"}, {"description", "kind=creatures: only those within this many metres of `near`"}}},
               {"side",
                {{"type", "string"},
                 {"enum", {"party", "hostile", "neutral"}},
                 {"description", "kind=creatures: only this side"}}},
               {"includeDead", {{"type", "boolean"}, {"description", "kind=creatures: default true"}}},
               {"includeInactive",
                {{"type", "boolean"},
                 {"description", "kind=creatures: creatures the area keeps hidden; default false"}}},
               {"brief",
                {{"type", "boolean"},
                 {"description", "kind=creatures, placeables, exits: a thing a line (`lines`) instead of in full"}}},
               {"interactive",
                {{"type", "boolean"}, {"description", "kind=placeables: only those that can be used (true)"}}},
               {"tag",
                {{"type", "string"}, {"description", "kind=triggers: only those whose tag holds this (any case)"}}},
               {"ints",
                {{"type", "array"},
                 {"description", "kind=triggers: local int names to read on each trigger, as its scripts set "
                                 "them (GetLocalInt)"}}}}}},
        Thread::Frame, true});

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"game",
                 "The game mode, pause, the area, the hero, the creature controlled, the party and who of it is "
                 "selected in the party bar, the game's message box if one is up (`popup`: its text and "
                 "buttons), and the event cursor."},
        [&bench, &watch](const Json &) -> ToolResult {
            Json answer = {{"mode", ModeName(watch.Mode())}, {"cursor", bench.Events().Next()}};
            const auto *app = dao::CClientExoApp::Get();
            answer["paused"] = app != nullptr && app->IsPaused();
            answer["popup"] = PopupJson();
            RequireWorld(watch, "inspect");
            const ObjectId hero = api::GetHero();
            answer["hero"] = Brief(hero);
            answer["controlled"] = Brief(api::GetMainControlled());
            answer["area"] = hero == kInvalidObject ? Json(nullptr) : Brief(api::GetArea(hero));
            Json party = Json::array();
            for (const ObjectId member : PartyOf(hero))
            {
                party.push_back(Brief(member));
            }
            answer["party"] = std::move(party);
            Json selected = Json::array();
            for (const ObjectId member : dao::party::Selected())
            {
                selected.push_back(Brief(member));
            }
            answer["selected"] = std::move(selected);
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"data",
                 "The game's data tree, which its interface is bound to: the value at a dotted `path` "
                 "(\"GUI.SelectedChar\", \"Map.ShowAllLabels\", \"GUIPopupLayer.DialogBoxText\"), as `text` "
                 "and as `number`, and its `type`; or several at once with `paths`. `found` is false for a path "
                 "the tree has not. What the interface shows of the game is read here without a picture. "
                 "`members: true` reads the list at `path` (a container's Container.Inventory, the level-up "
                 "screen's chains): `count` and each member's path, to read the same way. "
                 "Reading only. Answers in any mode."},
        [&watch](const Json &raw) -> ToolResult {
            const Args args(raw, "inspect");
            if (watch.Mode() < 0)
            {
                throw ToolError("inspect: the game is still starting; health says when it runs");
            }
            const auto read = [](const std::string &path) {
                Json value = {{"path", path}};
                const auto *attribute = dao::data::Find(Wide(path));
                value["found"] = attribute != nullptr;
                if (attribute != nullptr)
                {
                    using Type = dao::data::Attribute::Type;
                    const auto type = attribute->Kind();
                    value["type"] = type == Type::Bool          ? "bool"
                                    : type == Type::Int         ? "int"
                                    : type == Type::UnsignedInt ? "unsigned"
                                    : type == Type::Short       ? "short"
                                    : type == Type::Byte        ? "byte"
                                    : type == Type::Float       ? "float"
                                                                : "other";
                    value["text"] = Narrow(attribute->Text());
                    value["number"] = attribute->Number();
                }
                return value;
            };
            // A list, as the screens read one (DAOLib 5e10b31, Members): each
            // member's path, to read the same way (#5's level-up chains).
            if (args.Bool("members", false))
            {
                const auto path = args.String("path");
                const auto count = dao::data::MemberCount(Wide(path));
                if (!count)
                {
                    return Json{{"path", path}, {"found", false}};
                }
                Json members = Json::array();
                for (const auto &member : dao::data::Members(Wide(path)))
                {
                    members.push_back(Narrow(member));
                }
                return Json{{"path", path}, {"found", true}, {"count", *count}, {"members", std::move(members)}};
            }
            if (args.Has("paths"))
            {
                const auto &paths = args.Raw().at("paths");
                if (!paths.is_array() || paths.size() > 64)
                {
                    throw ToolError("inspect: `paths` is a list of up to 64 dotted paths");
                }
                Json values = Json::array();
                for (const auto &path : paths)
                {
                    if (!path.is_string())
                    {
                        throw ToolError("inspect: each of `paths` is a string, like \"GUI.SelectedChar\"");
                    }
                    values.push_back(read(path.get<std::string>()));
                }
                return Json{{"values", std::move(values)}};
            }
            return read(args.String("path"));
        });

    // A plot is a quest's or a story's set of flags, named by its resource
    // (the toolset's plot file: bed000pt_main, gen00pt_party). The scripts'
    // own commands read it (seen in play 2026-10-01: GetPlotGUID gives the
    // GUID the others take, "" for no such plot; GetPlotFlagName a flag's
    // name, "" past the last; GetPartyPlotFlag whether it is set). A flag
    // is read without asking for the plot's script (nCallScript 0), which
    // the flags from 256 on are computed by and which may do things. Two of
    // those read set all the same on the hero's main plot, so the game
    // answers something for them; whether it is what the script would say
    // was not looked into.
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"plot",
                 "The state of a quest or story: the flags of a plot, named by its resource in `plot` "
                 "(\"bed000pt_main\", \"gen00pt_party\"; the names are the game's plot files, and a mod's own). "
                 "Answers its `name` as the journal shows it and `flags`, each with `flag`, `name` and `set`, "
                 "with the `journal` text of a set one that has any; `set` lists the names of those set. "
                 "`flag` (a number or a name) asks for one. Flags numbered from 256 are computed by the plot's "
                 "script, which a read here does not run: take those as the game's answer without it. "
                 "Reading only."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const std::string asked = args.String("plot");
            std::wstring guid = api::GetPlotGUID(Wide(asked));
            if (guid.empty() && !api::GetPlotResRef(Wide(asked)).empty())
            {
                guid = Wide(asked); // given by its GUID
            }
            if (guid.empty())
            {
                throw ToolError(std::format("inspect: the game has no plot \"{}\"; a plot is named by its "
                                            "resource, like bed000pt_main",
                                            asked));
            }
            const ObjectId party = api::GetParty(api::GetHero());
            const auto flagJson = [&](std::int32_t flag, const std::wstring &name) {
                Json entry = {{"flag", flag}, {"name", Narrow(name)}};
                const bool set = api::GetPartyPlotFlag(party, guid, flag, 0) != 0;
                entry["set"] = set;
                if (set)
                {
                    if (auto journal = Narrow(api::GetPlotSummary(guid, flag)); !journal.empty())
                    {
                        entry["journal"] = std::move(journal);
                    }
                }
                return entry;
            };
            Json answer = {{"plot", Narrow(api::GetPlotResRef(guid))}, {"guid", Narrow(guid)}};
            if (const auto entry = api::GetPlotEntryName(guid); entry > 0)
            {
                if (auto name = Narrow(dao::tlk::String(static_cast<std::uint32_t>(entry))); !name.empty())
                {
                    answer["name"] = std::move(name);
                }
            }
            // The flags are numbered from 0 with gaps (a plot's main flags, then
            // its defined ones from 256), so every number is asked.
            constexpr std::int32_t kMostFlags = 512;
            const bool one = args.Has("flag");
            const Json wanted = one ? args.Raw().at("flag") : Json(nullptr);
            if (one && !wanted.is_number_integer() && !wanted.is_string())
            {
                throw ToolError("inspect: `flag` is a flag's number or its name, as kind=plot lists them");
            }
            Json flags = Json::array();
            Json set = Json::array();
            for (std::int32_t flag = 0; flag < kMostFlags; ++flag)
            {
                const std::wstring name = api::GetPlotFlagName(guid, flag);
                if (name.empty())
                {
                    continue;
                }
                if (one)
                {
                    const bool match = wanted.is_string() ? Narrow(name) == wanted.get<std::string>()
                                                          : wanted.get<std::int64_t>() == flag;
                    if (match)
                    {
                        answer["flag"] = flagJson(flag, name);
                        return answer;
                    }
                    continue;
                }
                Json entry = flagJson(flag, name);
                if (entry["set"].get<bool>())
                {
                    set.push_back(entry["name"]);
                }
                flags.push_back(std::move(entry));
            }
            if (one)
            {
                throw ToolError(std::format("inspect: plot \"{}\" has no flag {}; kind=plot without `flag` lists them",
                                            asked, wanted.dump()));
            }
            answer["flags"] = std::move(flags);
            answer["set"] = std::move(set);
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"cursor",
                 "What the game says is under the real mouse cursor now, in explore and combat mode: `object`, "
                 "the one a click would act on (null over none), the `ground` point under it and whether it is "
                 "`walkable`, and `hits`, everything the cursor's ray meets, nearest first. After moving the "
                 "real mouse, ask before clicking: a click beside a creature is a move order. The game picks "
                 "again 15 times a second. No `hits` at all, not even ground, means the game cast no ray: the "
                 "cursor is over a piece of the interface (a creature's name label among them) or outside "
                 "the picture."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto *explore = dao::CClientExoApp::CurrentExplore();
            if (explore == nullptr)
            {
                throw ToolError("inspect: the cursor picks in explore and combat mode only (health says the mode)");
            }
            const auto object = explore->CursorObject();
            const auto ground = explore->CursorGround();
            Json hits = Json::array();
            for (const auto &hit : explore->CursorHits())
            {
                if (hits.size() == 8)
                {
                    break;
                }
                hits.push_back(Json{{"kind", hit.kind == dao::world::Hit::Kind::Object   ? "object"
                                             : hit.kind == dao::world::Hit::Kind::Ground ? "ground"
                                                                                         : "nothing"},
                                    {"object", Brief(hit.object)},
                                    {"position", VectorJson(hit.position)}});
            }
            return Json{{"object", object ? Brief(*object) : Json(nullptr)},
                        {"ground", ground ? VectorJson(*ground) : Json(nullptr)},
                        {"walkable", explore->CursorGroundWalkable()},
                        {"hits", std::move(hits)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"window",
                 "The game's window on the desktop, for a test that must use the real mouse: `client`, its "
                 "picture's top left corner in desktop pixels and its size, and whether it is minimised or the "
                 "`foreground` window (the one the mouse and keys go to). A place in a screenshot, as a fraction "
                 "of the picture, times the size, plus the corner, is where to click. Answers in any mode."},
        [](const Json &) -> ToolResult {
            const auto place = GameWindowPlace();
            if (!place.found)
            {
                throw ToolError("inspect: the game has no window yet");
            }
            return Json{{"client", {{"x", place.x}, {"y", place.y}, {"width", place.width}, {"height", place.height}}},
                        {"minimised", place.minimised},
                        {"foreground", place.foreground}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{
            "dialogue",
            "The conversation on the screen (health's mode is dialog while one is): the `line` spoken last, "
            "with its speaker as the file writes it, `by` (the creature the game resolves it to) and `to` (whom it "
            "is spoken to) and text, the conversation's `owner`, and the `replies` the player may choose from now, "
            "in their order, "
            "each with its `place`, text and icon. No replies while the other speaks. The same comes as "
            "events: eluvian.dialogue.line, .replies and .chosen. `cutscene` and `movie` say whether one is playing. "
            "The dialogue tool answers."},
        [&watch](const Json &) -> ToolResult {
            RequireWorld(watch, "inspect");
            const auto *conversation = dao::dialogue::Current();
            Json answer = {{"active", conversation != nullptr},
                           {"cutscene", dao::dialogue::InCutscene()},
                           {"movie", dao::dialogue::InMovie()}};
            if (conversation == nullptr)
            {
                answer["note"] = "no conversation is on the screen";
                return answer;
            }
            const bool female = HeroIsFemale();
            const auto current = conversation->CurrentLine();
            const auto line = current ? conversation->LineAt(*current) : std::nullopt;
            answer["line"] = line ? LineJson(*line, female, conversation) : Json(nullptr);
            answer["replies"] = RepliesJson(dao::dialogue::Replies(), female, conversation);
            // Whose conversation it is (DAOLib's Owner, #23).
            const ObjectId owner = conversation->Owner();
            answer["owner"] = owner != kInvalidObject ? Brief(owner) : Json(nullptr);
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"placeables",
                 "The doors, chests, levers and other placeables in the area (or within `radius` of `near`), "
                 "nearest first: id, name, tag, template, position, distance, `state` (the placeable's own "
                 "number: a door's or a chest's changes when it opens), whether it is active and can be "
                 "interacted with, what its tooltip says, and its lock: whether it needs a key, the key's tag, "
                 "and the level to pick it; for a way out of the area, `leadsTo` (the area's tag and the "
                 "waypoint's there). order action=use uses one. `interactive: true` keeps only those that can be "
                 "used; `brief: true` answers a line each (`lines`) instead."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(around);
            const auto radius = args.Number("radius", -1);
            const bool includeInactive = args.Bool("includeInactive", false);
            const auto interactive =
                args.Has("interactive") ? std::optional{args.Bool("interactive", true)} : std::nullopt;
            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                if (api::GetObjectType(id) != k::OBJECT_TYPE_PLACEABLE ||
                    (interactive && (api::GetObjectInteractive(id) != 0) != *interactive))
                {
                    continue;
                }
                const bool active = api::GetObjectActive(id) != 0;
                const auto position = api::GetPosition(id);
                const float distance = Distance(from, position);
                if ((!includeInactive && !active) || (radius >= 0 && distance > radius))
                {
                    continue;
                }
                Json placeable = Brief(id);
                placeable["template"] = Narrow(api::GetResRef(id));
                placeable["position"] = VectorJson(position);
                placeable["distance"] = distance;
                placeable["state"] = api::GetPlaceableState(id);
                placeable["stateName"] = PlaceableStateWord(id);
                placeable["active"] = active;
                placeable["interactive"] = api::GetObjectInteractive(id) != 0;
                placeable["popup"] = Narrow(api::GetPlaceablePopupText(id));
                placeable["lock"] = Json{{"keyRequired", api::GetPlaceableKeyRequired(id) != 0},
                                         {"keyTag", Narrow(api::GetPlaceableKeyTag(id))},
                                         {"pickLevel", api::GetPlaceablePickLockLevel(id)}};
                if (auto leads = LeadsTo(id, false); !leads.is_null())
                {
                    placeable["leadsTo"] = std::move(leads);
                }
                found.emplace_back(distance, std::move(placeable));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json placeables = Json::array();
            for (auto &[distance, placeable] : found)
            {
                placeables.push_back(std::move(placeable));
            }
            if (args.Bool("brief", false))
            {
                return Json{
                    {"near", Brief(around)}, {"count", placeables.size()}, {"lines", Lines(placeables, PlaceableLine)}};
            }
            return Json{{"near", Brief(around)}, {"count", placeables.size()}, {"placeables", std::move(placeables)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"surroundings",
                 "One look around, a thing a line: the `creatures`, the placeables that can be used (`usable`: "
                 "chests, doors, bodies' remains, with their state and lock) and the `exits` within `radius` "
                 "(default 30 m) of `near` (default the creature controlled), nearest first, each as "
                 "kind=creatures, placeables and exits give it with `brief: true`. The read on arriving "
                 "anywhere; inspect a thing's kind in full for more."},
        [&bench, &watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const double radius = args.Number("radius", 30);
            // The other kinds' own handlers, on this thread: one read of the
            // area as they read it, each in brief.
            const auto look = [&bench, &raw, radius](const char *kind, Json call) {
                call["kind"] = kind;
                call["brief"] = true;
                call["radius"] = radius;
                if (raw.contains("near"))
                {
                    call["near"] = raw.at("near");
                }
                return bench.Tools().Resolve("inspect", call)->handler(call).data;
            };
            const Json creatures = look("creatures", Json::object());
            const Json usable = look("placeables", Json{{"interactive", true}});
            const Json exits = look("exits", Json::object());
            return Json{{"near", creatures.value("near", Json())},
                        {"radius", radius},
                        {"creatures", creatures.value("lines", Json::array())},
                        {"usable", usable.value("lines", Json::array())},
                        {"exits", exits.value("lines", Json::array())}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"exits",
                 "The ways out of the area: every door or other placeable and every trigger that takes the "
                 "party to another area, nearest first, with `leadsTo` (the area's tag and the waypoint's "
                 "there), its kind, position, distance and whether it is active. A placeable is used with "
                 "order action=use; a trigger is walked into (travel to its position). Some doors choose where "
                 "they lead only when used, so their `leadsTo` may be empty or the last use's. Going through "
                 "one loads the other area, and the game may autosave. `radius` keeps those within it; `brief: "
                 "true` answers a line each (`lines`)."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(around);
            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                const auto type = api::GetObjectType(id);
                if (type != k::OBJECT_TYPE_PLACEABLE && type != k::OBJECT_TYPE_TRIGGER)
                {
                    continue;
                }
                Json leads = LeadsTo(id, type == k::OBJECT_TYPE_TRIGGER);
                if (leads.is_null())
                {
                    continue;
                }
                const auto position = api::GetPosition(id);
                const float distance = Distance(from, position);
                if (const auto radius = args.Number("radius", -1); radius >= 0 && distance > radius)
                {
                    continue;
                }
                Json exit = Brief(id);
                exit["kind"] = type == k::OBJECT_TYPE_TRIGGER ? "trigger" : "placeable";
                exit["leadsTo"] = std::move(leads);
                exit["position"] = VectorJson(position);
                exit["distance"] = distance;
                exit["active"] = api::GetObjectActive(id) != 0;
                // A text blocker has a destination as an exit does, but its
                // use shows a line instead of leaving: "Return Path"
                // (wmt_rand_enc_text_blocker) in random encounters and at
                // Lothering's south end, taken for the way to the world map
                // (turn-based-agent, 2026-10-03, #32). Its tag says what it is.
                if (exit.value("tag", std::string()).find("blocker") != std::string::npos)
                {
                    exit["blocker"] = true;
                }
                found.emplace_back(distance, std::move(exit));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json exits = Json::array();
            for (auto &[distance, exit] : found)
            {
                exits.push_back(std::move(exit));
            }
            if (args.Bool("brief", false))
            {
                return Json{
                    {"area", Brief(api::GetArea(around))}, {"count", exits.size()}, {"lines", Lines(exits, ExitLine)}};
            }
            return Json{{"area", Brief(api::GetArea(around))}, {"count", exits.size()}, {"exits", std::move(exits)}};
        });

    // The area's triggers, which no other kind lists: the Gauntlet's bridge
    // turns on twelve tile triggers, found by script GetNearestObject and
    // read one GetLocalInt at a time (turn-based-agent, 2026-10-04, #49). A
    // puzzle's state lives in a trigger's locals, under names its scripts
    // choose, so the names are asked for.
    bench.Tools().AddKind(
        "inspect",
        KindSpec{"triggers",
                 "The area's triggers, nearest first: each one's `id`, `tag`, `name`, `position`, `distance`, "
                 "whether it is `active`, and `leadsTo` for one that takes the party elsewhere (kind=exits lists "
                 "those alone). A trigger is walked into, not used: travel or order move to its position. "
                 "`radius` and `near` keep those within reach of a creature, `tag` those whose tag holds it; "
                 "`ints` (names) reads those local ints on each, as its scripts set them (a puzzle's state, a "
                 "switch's number)."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            std::vector<std::wstring> ints;
            if (args.Has("ints"))
            {
                const Json &names = args.Raw().at("ints");
                if (!names.is_array() || names.size() > 32)
                {
                    throw ToolError("inspect: `ints` is a list of up to 32 local int names");
                }
                for (const auto &name : names)
                {
                    if (!name.is_string())
                    {
                        throw ToolError("inspect: each of `ints` is a local's name, a string");
                    }
                    ints.push_back(Wide(name.get<std::string>()));
                }
            }
            std::string want = args.Has("tag") ? args.String("tag") : std::string();
            std::ranges::transform(want, want.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            const auto from = api::GetPosition(around);
            const double radius = args.Number("radius", -1);
            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                if (api::GetObjectType(id) != k::OBJECT_TYPE_TRIGGER)
                {
                    continue;
                }
                const auto position = api::GetPosition(id);
                const float distance = Distance(from, position);
                if (radius >= 0 && distance > radius)
                {
                    continue;
                }
                Json trigger = Brief(id);
                std::string tag = trigger.value("tag", std::string());
                std::ranges::transform(tag, tag.begin(), [](char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
                if (!want.empty() && tag.find(want) == std::string::npos)
                {
                    continue;
                }
                trigger["position"] = VectorJson(position);
                trigger["distance"] = distance;
                trigger["active"] = api::GetObjectActive(id) != 0;
                if (Json leads = LeadsTo(id, true); !leads.is_null())
                {
                    trigger["leadsTo"] = std::move(leads);
                }
                if (!ints.empty())
                {
                    Json locals = Json::object();
                    for (const auto &name : ints)
                    {
                        locals[Narrow(name)] = api::GetLocalInt(id, name);
                    }
                    trigger["ints"] = std::move(locals);
                }
                found.emplace_back(distance, std::move(trigger));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json triggers = Json::array();
            for (auto &[distance, trigger] : found)
            {
                triggers.push_back(std::move(trigger));
            }
            return Json{
                {"area", Brief(api::GetArea(around))}, {"count", triggers.size()}, {"triggers", std::move(triggers)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"journal",
                 "The quests in the journal: `active`, the one marked, and `quests`, those under way, each with "
                 "`plot` (its resource: inspect kind=plot with it gives the flags), `name`, `description` and its "
                 "`objective` as the journal shows them, its group, area, and whether it was `updated` or is "
                 "`completed`, and `parent` for a quest within another (one done may stay listed here, with "
                 "`completed` true). `completed: true` "
                 "lists the quests done too; otherwise only their count. Reading only."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            Json done = JournalList(L"CompletedPlots");
            Json answer = {{"active", JournalEntry(L"Journal.ActivePlot.")},
                           {"quests", JournalList(L"PlotList")},
                           {"completedCount", done.size()}};
            if (args.Bool("completed", false))
            {
                answer["completed"] = std::move(done);
            }
            return answer;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"creatures",
                 "Every creature in the area (or within `radius` of `near`), nearest first: id, name, tag, side "
                 "(party, hostile, neutral: the hero's view), its `group` (the game's own: 0 the player's, 1 "
                 "hostile, 2 friendly), position, distance, dead, health and mana or stamina, combat state, "
                 "whom it attacks, its `room` (number; `seen` by the party: never, which the game draws "
                 "black with what stands in it, in view, or before; `shown`; and how much the game wants "
                 "the room and whether it is in memory), and the command it is carrying out. `brief: true` "
                 "answers a line each (`lines`: id, name, tag, side, health, dead, distance) instead."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId hero = api::GetHero();
            const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
            if (around == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `near` was given; is a game loaded?");
            }
            const auto from = api::GetPosition(around);
            const auto radius = args.Number("radius", -1);
            const auto side =
                args.Has("side") ? std::optional{args.Choice("side", {"party", "hostile", "neutral"})} : std::nullopt;
            const bool includeDead = args.Bool("includeDead", true);
            const bool includeInactive = args.Bool("includeInactive", false);
            const auto party = PartyOf(hero);

            std::vector<std::pair<float, Json>> found;
            for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
            {
                if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE ||
                    (!includeInactive && api::GetObjectActive(id) == 0))
                {
                    continue;
                }
                const float distance = Distance(from, api::GetPosition(id));
                if (radius >= 0 && distance > radius)
                {
                    continue;
                }
                Json creature = Summary(id, hero, party);
                if ((side && creature["side"] != *side) || (!includeDead && creature["dead"] == true))
                {
                    continue;
                }
                creature["distance"] = distance;
                found.emplace_back(distance, std::move(creature));
            }
            std::ranges::sort(found, {}, &std::pair<float, Json>::first);
            Json creatures = Json::array();
            for (auto &[distance, creature] : found)
            {
                creatures.push_back(std::move(creature));
            }
            if (args.Bool("brief", false))
            {
                return Json{
                    {"near", Brief(around)}, {"count", creatures.size()}, {"lines", Lines(creatures, CreatureLine)}};
            }
            return Json{{"near", Brief(around)}, {"count", creatures.size()}, {"creatures", std::move(creatures)}};
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"creature",
                 "One creature in full: its summary (with the command it is carrying out), facing, appearance, the "
                 "abilities on its quickbar and in its tactics with each one's cooldown (seconds left, and of how "
                 "many), its "
                 "movement's path (`beingWalked` says whether it is walking it now), and every effect "
                 "on it: its type (EFFECT_TYPE_*), creator, ability (id and name) and time left (seconds; null "
                 "for effects that do not run out)."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
            if (id == kInvalidObject || api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
            {
                throw ToolError(std::format("inspect: {} is not a creature; inspect kind=creatures lists them",
                                            id == kInvalidObject ? "nothing" : FormatId(id)));
            }
            const ObjectId hero = api::GetHero();
            Json creature = Summary(id, hero, PartyOf(hero));
            creature["active"] = api::GetObjectActive(id) != 0;
            creature["facing"] = api::GetFacing(id);
            creature["appearance"] = api::GetAppearanceType(id);
            creature["effects"] = Effects(id);
            creature["abilities"] = Abilities(id);
            creature["path"] = PathNow(id, api::GetPosition(id), false);
            return creature;
        });

    bench.Tools().AddKind(
        "inspect",
        KindSpec{"geometry",
                 "Between a creature (`id`, default the one controlled) and a creature (`target`) or a point "
                 "(`point`): the straight distance, the line of sight as the engine judges it (eye height to eye "
                 "height; where it is cut, if it is; only walls and placeables block, not creatures), and the "
                 "creature's movement path with its length and what is left of it; `beingWalked` says "
                 "whether it is walking it now (`waypoints: true` lists its points). `walk: true` also searches the "
                 "path it would walk there, as the game's pathfinder finds it now: `found` with its `length`, or "
                 "why not, and for a shut door or the like in the way, `blocker`: use that first, then move. The "
                 "creature is left as it is."},
        [&watch](const Json &raw) -> ToolResult {
            RequireWorld(watch, "inspect");
            const Args args(raw, "inspect");
            const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
            if (id == kInvalidObject)
            {
                throw ToolError("inspect: no creature is controlled and no `id` was given");
            }
            const bool waypoints = args.Bool("waypoints", false);
            const auto from = api::GetPosition(id);
            Json answer = {{"from", Brief(id)}, {"position", VectorJson(from)}};

            dao::Vector to;
            dao::world::Sight sight;
            if (const auto target = args.OptionalId("target", kCreatureHint))
            {
                to = api::GetPosition(*target);
                answer["target"] = Brief(*target);
                sight = dao::world::LineOfSight(id, *target);
            }
            else if (args.Has("point"))
            {
                to = VectorOf(args.Raw().at("point"), "inspect: `point`");
                const auto eye = [](dao::Vector v) {
                    v.z += dao::world::kEyeHeight;
                    return v;
                };
                sight = dao::world::LineOfSight(eye(from), eye(to), id);
            }
            else
            {
                throw ToolError("inspect: kind=geometry needs a `target` (a creature id) or a `point`");
            }
            answer["to"] = VectorJson(to);
            answer["distance"] = Distance(from, to);
            answer["lineOfSight"] =
                Json{{"clear", sight.clear}, {"blockedAt", sight.blockedAt ? VectorJson(*sight.blockedAt) : Json()}};
            answer["path"] = PathNow(id, from, waypoints);
            // The walk there, when asked for. DAOLib's first FindPath drove the
            // creature's own search and left creatures unable to walk (in play,
            // 2026-09-30); its Pathfind since searches apart from any creature's
            // and was seen in play with the creature walking as before
            // throughout (lib-agent, 2026-10-01, 1fe01a3 on). It names what is
            // in the way of a walk, which a move's COMMAND_FAILED_PATH_ACTION_
            // REQUIRED does not: a test's walk across the Elven Ruins failed
            // ten times on a shut door it could not name. Still the caller's
            // to choose: a search is up to 30 ms of the frame.
            if (args.Bool("walk", false))
            {
                const auto search =
                    dao::world::Pathfind(id, to, args.OptionalId("target", kCreatureHint).value_or(kInvalidObject));
                Json walk = {{"found", search.path.has_value()}};
                const char *said = search.result == 1   ? (search.path ? "found" : "gave up part way")
                                   : search.result == 5 ? "none"
                                   : search.result == 6 ? "something is in the way"
                                   : search.result == 8 ? "no walkable ground there"
                                                        : "not finished in time";
                walk["result"] = said;
                // How many searches it took: one, or more for a long walk the
                // search went on with from where it gave up (DAOLib dac5e99, #19).
                walk["legs"] = search.legs;
                if (search.path)
                {
                    walk["length"] = search.path->Length();
                    walk["waypointCount"] = search.path->waypoints.size();
                    if (waypoints)
                    {
                        Json points = Json::array();
                        for (const auto &point : search.path->waypoints)
                        {
                            points.push_back(VectorJson(point));
                        }
                        walk["waypoints"] = std::move(points);
                    }
                }
                else if (search.partial)
                {
                    walk["reached"] = search.partial->waypoints.empty() ? Json(nullptr)
                                                                        : VectorJson(search.partial->waypoints.back());
                    walk["lengthSoFar"] = search.partial->Length();
                }
                if (search.result == 6)
                {
                    walk["blocker"] = BlockerJson(search.blocker);
                    walk["note"] = search.blocker == kInvalidObject
                                       ? "a door or the like stands in the way and was not found by name; inspect "
                                         "kind=placeables near `reached` lists what is there"
                                       : "a move there ends COMMAND_FAILED_PATH_ACTION_REQUIRED; order action=use "
                                         "on `blocker` first (a shut door opens), then move";
                }
                else if (!search.path && search.blocker != kInvalidObject)
                {
                    // Not the pathfinder's own word: where a search gives up
                    // part way, DAOLib names the placeable lying across the
                    // way just ahead of where it stopped (c8d7270). A strong
                    // hint, not a fact.
                    walk["blocker"] = BlockerJson(search.blocker);
                    walk["note"] = "the search gave up short of something lying across the way: likely `blocker`, "
                                   "a shut door perhaps. order action=use on it, then ask again or move";
                }
                // For DAOLib's own reading of a search that went wrong: each
                // slice's answer and the search's line for a log.
                if (args.Bool("trace", false))
                {
                    if (search.partial)
                    {
                        Json tail = Json::array();
                        const auto &points = search.partial->waypoints;
                        for (std::size_t i = points.size() > 3 ? points.size() - 3 : 0; i < points.size(); ++i)
                        {
                            tail.push_back(VectorJson(points[i]));
                        }
                        walk["partialTail"] = std::move(tail);
                        walk["partialWaypoints"] = points.size();
                    }
                    walk["answers"] = search.answers;
                    walk["slices"] = search.slices;
                    walk["trace"] = search.trace;
                }
                answer["walk"] = std::move(walk);
            }
            return answer;
        });
}

} // namespace eluvian::game
