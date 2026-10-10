// The game's reads, a tool each: `state` (the game itself, its message
// box), the conversation's read under `dialogue`, `placeables`, `exits`,
// `triggers`, `creatures`, `creature` in full, `surroundings`, `geometry`
// between two, `data`, `plot`, `journal`, `cursor` and `window`. They were
// kinds of one `inspect` until #110. Reads only.
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
        if (lock.contains("keyHeld"))
        {
            line += lock.value("keyHeld", false) ? " (held)" : " (not held)";
        }
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

// What stands in a walk's way: a placeable with its state and its lock, as
// placeables reads them, so a door the story keeps shut (pick level
// 10000) is told from one a use opens (#33), and where it leads if it is an
// exit, which no use on the way opens (#38).
// A placeable's lock: whether its key is required, the key's tag, the
// pick level, and whether the party carries the key, which a door whose
// key is required opens to and nothing else (#40). The party's things
// are the hero's. For a walk's blocker and for placeables alike: a
// driver read a door's lock only through the walk (#93).
Json LockJson(ObjectId id)
{
    const std::wstring key = api::GetPlaceableKeyTag(id);
    Json lock = {{"keyRequired", api::GetPlaceableKeyRequired(id) != 0},
                 {"keyTag", Narrow(key)},
                 {"pickLevel", api::GetPlaceablePickLockLevel(id)}};
    if (!key.empty())
    {
        lock["keyHeld"] = api::CountItemsByTag(api::GetHero(), key) > 0;
    }
    return lock;
}

Json BlockerJson(ObjectId id)
{
    Json blocker = Brief(id);
    if (id != kInvalidObject && api::GetObjectType(id) == k::OBJECT_TYPE_PLACEABLE)
    {
        blocker["state"] = api::GetPlaceableState(id);
        blocker["stateName"] = PlaceableStateWord(id);
        blocker["interactive"] = api::GetObjectInteractive(id) != 0; // nothing uses one that is not (#50)
        // What its use does: an exit's is the area transition, a door's to
        // open, whatever its template's leadsTo says (#104).
        blocker["action"] = api::GetPlaceableAction(id);
        blocker["lock"] = LockJson(id);
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

namespace
{

// Why combat mode stays with no fight left: the game leaves combat only once
// a living party member stands within 7.5 m of each fallen one (the scripts'
// CheckResurrection, RULES_RESURRECTION_DISTANCE; module_core asks again each
// second through EVENT_TYPE_DELAYED_GM_CHANGE, as turn-based-agent read them),
// and nothing said so: a playthrough's walks stopped "a fight began" for six
// minutes after one (2026-10-03, #20). Null while a party member perceives a
// hostile, or none is down.
Json CombatHold(const std::set<ObjectId> &party)
{
    constexpr float kRaiseDistance = 7.5F;
    std::vector<ObjectId> living;
    std::vector<ObjectId> fallen;
    for (const ObjectId member : party)
    {
        if (api::IsPerceivingHostiles(member) != 0)
        {
            return nullptr;
        }
        (api::IsDead(member) != 0 ? fallen : living).push_back(member);
    }
    if (fallen.empty())
    {
        return nullptr;
    }
    Json list = Json::array();
    for (const ObjectId down : fallen)
    {
        Json entry = Brief(down);
        const auto at = api::GetPosition(down);
        ObjectId nearest = kInvalidObject;
        float best = std::numeric_limits<float>::max();
        for (const ObjectId up : living)
        {
            if (const float distance = Distance(api::GetPosition(up), at); distance < best)
            {
                best = distance;
                nearest = up;
            }
        }
        entry["nearestLiving"] = nearest == kInvalidObject ? Json(nullptr) : Brief(nearest);
        entry["distance"] = nearest == kInvalidObject ? Json(nullptr) : Json(best);
        entry["near"] = nearest != kInvalidObject && best <= kRaiseDistance;
        list.push_back(std::move(entry));
    }
    return Json{{"within", kRaiseDistance},
                {"fallen", std::move(list)},
                {"why", "No hostile is in sight, but the game leaves combat only once a living party member stands "
                        "within 7.5 meters of each fallen one: walk one there (travel to the fallen member's id)."}};
}

// How many inactive ones a list left out, said when any were: the hole in a
// sliding puzzle's grid was an inactive placeable, and nothing said one had
// been left out (turn-based-agent, 2026-10-05, #65).
void SayLeftOut(Json &answer, int leftOut, const char *what)
{
    if (leftOut > 0)
    {
        answer["inactiveLeftOut"] = leftOut;
        answer["note"] = std::to_string(leftOut) + " inactive " + what +
                         " left out (the area keeps them hidden); pass includeInactive: true to list them too.";
    }
}

// The game kind's handler, registered by AddInspect.
ToolResult KindGame(Bench &bench, const Watch &watch, const Json & /*args*/)
{
    Json answer = {{"mode", ModeName(watch.Mode())}, {"cursor", bench.Events().Next()}};
    const auto *app = dao::CClientExoApp::Get();
    answer["paused"] = app != nullptr && app->IsPaused();
    answer["popup"] = PopupJson();
    RequireWorld(watch, "state");
    const ObjectId hero = api::GetHero();
    answer["hero"] = Brief(hero);
    answer["controlled"] = Brief(api::GetMainControlled());
    // Who leads the party, which a follower given control does not become: a
    // driver took the one controlled for the leader (turn-based-agent,
    // 2026-10-04, #60).
    answer["leader"] = Brief(api::GetPartyLeader());
    answer["area"] = hero == kInvalidObject ? Json(nullptr) : Brief(api::GetArea(hero));
    Json party = Json::array();
    for (const ObjectId member : PartyOf(hero))
    {
        party.push_back(Brief(member));
    }
    answer["party"] = std::move(party);
    if (watch.Mode() == static_cast<std::int32_t>(dao::GameMode::Combat))
    {
        answer["combatHold"] = CombatHold(PartyOf(hero));
    }
    Json selected = Json::array();
    for (const ObjectId member : dao::party::Selected())
    {
        selected.push_back(Brief(member));
    }
    answer["selected"] = std::move(selected);
    return answer;
}

// The data kind's handler, registered by AddInspect.
ToolResult KindData(const Watch &watch, const Json &raw)
{
    const Args args(raw, "data");
    if (watch.Mode() < 0)
    {
        throw ToolError("data: the game is still starting; health says when it runs");
    }
    const auto read = [](const std::string &path) {
        Json value = {{"path", path}};
        const auto *attribute = dao::data::Find(Wide(path));
        value["found"] = attribute != nullptr;
        if (attribute != nullptr)
        {
            using Type = dao::data::Attribute::Type;
            const auto type = attribute->Kind();
            value["type"] = Label(type, {{Type::Bool, "bool"},
                                         {Type::Int, "int"},
                                         {Type::UnsignedInt, "unsigned"},
                                         {Type::Short, "short"},
                                         {Type::Byte, "byte"},
                                         {Type::Float, "float"}});
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
            throw ToolError("data: `paths` is a list of up to 64 dotted paths");
        }
        Json values = Json::array();
        for (const auto &path : paths)
        {
            if (!path.is_string())
            {
                throw ToolError("data: each of `paths` is a string, like \"GUI.SelectedChar\"");
            }
            values.push_back(read(path.get<std::string>()));
        }
        return Json{{"values", std::move(values)}};
    }
    return read(args.String("path"));
}

// A plot named by its resource, or given by its GUID: its GUID.
std::wstring PlotGuid(const std::string &asked)
{
    std::wstring guid = api::GetPlotGUID(Wide(asked));
    if (guid.empty() && !api::GetPlotResRef(Wide(asked)).empty())
    {
        guid = Wide(asked); // given by its GUID
    }
    if (guid.empty())
    {
        throw ToolError(std::format("plot: the game has no plot \"{}\"; a plot is named by its "
                                    "resource, like bed000pt_main",
                                    asked));
    }
    return guid;
}

// One of a plot's flags: its number and name, whether the party has it
// set, and its journal line where it is.
Json PlotFlag(ObjectId party, const std::wstring &guid, std::int32_t flag, const std::wstring &name)
{
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
}

// The plot kind's handler, registered by AddInspect.
ToolResult KindPlot(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "plot");
    const Args args(raw, "plot");
    const std::string asked = args.String("plot");
    const std::wstring guid = PlotGuid(asked);
    const ObjectId party = api::GetParty(api::GetHero());
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
        throw ToolError("plot: `flag` is a flag's number or its name, as plot lists them");
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
            const bool match =
                wanted.is_string() ? Narrow(name) == wanted.get<std::string>() : wanted.get<std::int64_t>() == flag;
            if (match)
            {
                answer["flag"] = PlotFlag(party, guid, flag, name);
                return answer;
            }
            continue;
        }
        Json entry = PlotFlag(party, guid, flag, name);
        if (entry["set"].get<bool>())
        {
            set.push_back(entry["name"]);
        }
        flags.push_back(std::move(entry));
    }
    if (one)
    {
        throw ToolError(
            std::format("plot: plot \"{}\" has no flag {}; plot without `flag` lists them", asked, wanted.dump()));
    }
    answer["flags"] = std::move(flags);
    answer["set"] = std::move(set);
    return answer;
}

// The cursor kind's handler, registered by AddInspect.
ToolResult KindCursor(const Watch &watch, const Json & /*args*/)
{
    RequireWorld(watch, "cursor");
    const auto *explore = dao::CClientExoApp::CurrentExplore();
    if (explore == nullptr)
    {
        throw ToolError("cursor: the cursor picks in explore and combat mode only (health says the mode)");
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
        hits.push_back(Json{{"kind", Label(hit.kind, {{dao::world::Hit::Kind::Object, "object"},
                                                      {dao::world::Hit::Kind::Ground, "ground"},
                                                      {dao::world::Hit::Kind::Nothing, "nothing"}})},
                            {"object", Brief(hit.object)},
                            {"position", VectorJson(hit.position)}});
    }
    return Json{{"object", object ? Brief(*object) : Json(nullptr)},
                {"ground", ground ? VectorJson(*ground) : Json(nullptr)},
                {"walkable", explore->CursorGroundWalkable()},
                {"hits", std::move(hits)}};
}

// The window kind's handler, registered by AddInspect.
ToolResult KindWindow(const Json & /*args*/)
{
    const auto place = GameWindowPlace();
    if (!place.found)
    {
        throw ToolError("window: the game has no window yet");
    }
    return Json{{"client", {{"x", place.x}, {"y", place.y}, {"width", place.width}, {"height", place.height}}},
                {"minimised", place.minimised},
                {"foreground", place.foreground}};
}

// The dialogue kind's handler, registered by AddInspect.
ToolResult KindDialogue(const Watch &watch, const Json & /*args*/)
{
    RequireWorld(watch, "dialogue");
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
}

// The placeables kind's handler, registered by AddInspect.
ToolResult KindPlaceables(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "placeables");
    const Args args(raw, "placeables");
    const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
    if (around == kInvalidObject)
    {
        throw ToolError("placeables: no creature is controlled and no `near` was given; is a game loaded?");
    }
    const auto from = api::GetPosition(around);
    const auto radius = args.Number("radius", -1);
    const bool includeInactive = args.Bool("includeInactive", false);
    const auto interactive = args.Has("interactive") ? std::optional{args.Bool("interactive", true)} : std::nullopt;
    std::vector<std::pair<float, Json>> found;
    int leftOut = 0; // inactive ones within the other filters (#65)
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
        if (radius >= 0 && distance > radius)
        {
            continue;
        }
        if (!includeInactive && !active)
        {
            ++leftOut;
            continue;
        }
        Json placeable = Brief(id);
        placeable["template"] = Narrow(api::GetResRef(id));
        placeable["position"] = VectorJson(position);
        placeable["distance"] = distance;
        placeable["state"] = api::GetPlaceableState(id);
        placeable["stateName"] = PlaceableStateWord(id);
        const auto action = api::GetPlaceableAction(id);
        placeable["action"] = action;
        placeable["actionName"] = std::string(PlaceableActionName(action));
        placeable["active"] = active;
        placeable["interactive"] = api::GetObjectInteractive(id) != 0;
        placeable["popup"] = Narrow(api::GetPlaceablePopupText(id));
        placeable["lock"] = LockJson(id);
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
    Json answer = {{"near", Brief(around)}, {"count", placeables.size()}};
    if (args.Bool("brief", false))
    {
        answer["lines"] = Lines(placeables, PlaceableLine);
    }
    else
    {
        answer["placeables"] = std::move(placeables);
    }
    SayLeftOut(answer, leftOut, "placeables");
    return answer;
}

// The surroundings kind's handler, registered by AddInspect.
ToolResult KindSurroundings(Bench &bench, const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "surroundings");
    const Args args(raw, "surroundings");
    const double radius = args.Number("radius", 30);
    // The other reads' own handlers, on this thread: one read of the
    // area as they read it, each in brief.
    const auto look = [&bench, &raw, radius](const char *tool, Json call) {
        call["brief"] = true;
        call["radius"] = radius;
        if (raw.contains("near"))
        {
            call["near"] = raw.at("near");
        }
        return bench.Tools().Resolve(tool)->handler(call).data;
    };
    const Json creatures = look("creatures", Json::object());
    const Json usable = look("placeables", Json{{"interactive", true}});
    const Json exits = look("exits", Json::object());
    return Json{{"near", creatures.value("near", Json())},
                {"radius", radius},
                {"creatures", creatures.value("lines", Json::array())},
                {"usable", usable.value("lines", Json::array())},
                {"exits", exits.value("lines", Json::array())}};
}

// The exits kind's handler, registered by AddInspect.
ToolResult KindExits(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "exits");
    const Args args(raw, "exits");
    const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
    if (around == kInvalidObject)
    {
        throw ToolError("exits: no creature is controlled and no `near` was given; is a game loaded?");
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
        return Json{{"area", Brief(api::GetArea(around))}, {"count", exits.size()}, {"lines", Lines(exits, ExitLine)}};
    }
    return Json{{"area", Brief(api::GetArea(around))}, {"count", exits.size()}, {"exits", std::move(exits)}};
}

// `ints`: the local int names to read on each trigger, up to 32.
std::vector<std::wstring> IntNames(const Args &args)
{
    std::vector<std::wstring> ints;
    if (args.Has("ints"))
    {
        const Json &names = args.Raw().at("ints");
        if (!names.is_array() || names.size() > 32)
        {
            throw ToolError("triggers: `ints` is a list of up to 32 local int names");
        }
        for (const auto &name : names)
        {
            if (!name.is_string())
            {
                throw ToolError("triggers: each of `ints` is a local's name, a string");
            }
            ints.push_back(Wide(name.get<std::string>()));
        }
    }
    return ints;
}

// A trigger's place, distance, whether it is active, where it leads
// and the locals asked for.
void Described(ObjectId id, const dao::Vector &position, float distance, const std::vector<std::wstring> &ints,
               Json &trigger)
{
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
}

// The triggers kind's handler, registered by AddInspect.
ToolResult KindTriggers(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "triggers");
    const Args args(raw, "triggers");
    const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
    if (around == kInvalidObject)
    {
        throw ToolError("triggers: no creature is controlled and no `near` was given; is a game loaded?");
    }
    const std::vector<std::wstring> ints = IntNames(args);
    std::string want = args.Has("tag") ? args.String("tag") : std::string();
    std::ranges::transform(want, want.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
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
        std::ranges::transform(tag, tag.begin(),
                               [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (!want.empty() && tag.find(want) == std::string::npos)
        {
            continue;
        }
        Described(id, position, distance, ints, trigger);
        found.emplace_back(distance, std::move(trigger));
    }
    std::ranges::sort(found, {}, &std::pair<float, Json>::first);
    Json triggers = Json::array();
    for (auto &[distance, trigger] : found)
    {
        triggers.push_back(std::move(trigger));
    }
    return Json{{"area", Brief(api::GetArea(around))}, {"count", triggers.size()}, {"triggers", std::move(triggers)}};
}

// The journal kind's handler, registered by AddInspect.
ToolResult KindJournal(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "journal");
    const Args args(raw, "journal");
    Json done = JournalList(L"CompletedPlots");
    Json answer = {{"active", JournalEntry(L"Journal.ActivePlot.")},
                   {"quests", JournalList(L"PlotList")},
                   {"completedCount", done.size()}};
    if (args.Bool("completed", false))
    {
        answer["completed"] = std::move(done);
    }
    return answer;
}

// The creatures kind's handler, registered by AddInspect.
ToolResult KindCreatures(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "creatures");
    const Args args(raw, "creatures");
    const ObjectId hero = api::GetHero();
    const ObjectId around = args.OptionalId("near", kCreatureHint).value_or(api::GetMainControlled());
    if (around == kInvalidObject)
    {
        throw ToolError("creatures: no creature is controlled and no `near` was given; is a game loaded?");
    }
    const auto from = api::GetPosition(around);
    const auto radius = args.Number("radius", -1);
    const auto side =
        args.Has("side") ? std::optional{args.Choice("side", {"party", "hostile", "neutral"})} : std::nullopt;
    const bool includeDead = args.Bool("includeDead", true);
    const bool includeInactive = args.Bool("includeInactive", false);
    const auto party = PartyOf(hero);

    std::vector<std::pair<float, Json>> found;
    int leftOut = 0; // inactive ones within the other filters (#65)
    for (const ObjectId id : Objects(api::GetObjectsInArea(api::GetArea(around))))
    {
        if (api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
        {
            continue;
        }
        const float distance = Distance(from, api::GetPosition(id));
        if (radius >= 0 && distance > radius)
        {
            continue;
        }
        if (!includeInactive && api::GetObjectActive(id) == 0)
        {
            ++leftOut;
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
    Json answer = {{"near", Brief(around)}, {"count", creatures.size()}};
    if (args.Bool("brief", false))
    {
        answer["lines"] = Lines(creatures, CreatureLine);
    }
    else
    {
        answer["creatures"] = std::move(creatures);
    }
    SayLeftOut(answer, leftOut, "creatures");
    return answer;
}

// The creature kind's handler, registered by AddInspect.
ToolResult KindCreature(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "creature");
    const Args args(raw, "creature");
    const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
    if (id == kInvalidObject || api::GetObjectType(id) != k::OBJECT_TYPE_CREATURE)
    {
        throw ToolError(std::format("creature: {} is not a creature; creatures lists them",
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
}

// What the walk's search answered, in words.
const char *SearchResult(const dao::world::PathSearch &search)
{
    switch (search.result)
    {
    case 1:
        return search.path ? "found" : "gave up part way";
    case 5:
        return "none";
    case 6:
        return "something is in the way";
    case 8:
        return "no walkable ground there";
    default:
        return "not finished in time";
    }
}

// A walk's search as geometry answers it (#77): what it found, how
// far it got, what stands in the way, and with `trace`, DAOLib's own
// reading of it.
Json WalkJson(const dao::world::PathSearch &search, bool waypoints, bool trace)
{
    Json walk = {{"found", search.path.has_value()}};
    walk["result"] = SearchResult(search);
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
        walk["reached"] =
            search.partial->waypoints.empty() ? Json(nullptr) : VectorJson(search.partial->waypoints.back());
        walk["lengthSoFar"] = search.partial->Length();
    }
    if (search.result == 6)
    {
        walk["blocker"] = BlockerJson(search.blocker);
        walk["note"] = search.blocker == kInvalidObject
                           ? "a door or the like stands in the way and was not found by name; "
                             "placeables near `reached` lists what is there"
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
    if (trace)
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
    return walk;
}

// The geometry kind's handler, registered by AddInspect.
ToolResult KindGeometry(const Watch &watch, const Json &raw)
{
    RequireWorld(watch, "geometry");
    const Args args(raw, "geometry");
    const ObjectId id = args.OptionalId("id", kCreatureHint).value_or(api::GetMainControlled());
    if (id == kInvalidObject)
    {
        throw ToolError("geometry: no creature is controlled and no `id` was given");
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
        to = VectorOf(args.Raw().at("point"), "geometry: `point`");
        const auto eye = [](dao::Vector v) {
            v.z += dao::world::kEyeHeight;
            return v;
        };
        sight = dao::world::LineOfSight(eye(from), eye(to), id);
    }
    else
    {
        throw ToolError("geometry: needs a `target` (a creature id) or a `point`");
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
        answer["walk"] = WalkJson(search, waypoints, args.Bool("trace", false));
    }
    return answer;
}

} // namespace

void AddInspect(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("state",
             "Returns the game's current state: the `mode`, whether it is `paused`, the `area`, the `hero`, "
             "the creature you control (`controlled`), the party's `leader` (not always the one you control: a "
             "follower given control does not lead), the `party` and which members are `selected` in the "
             "party bar, the game's message box if one is open (`popup`: its text and buttons), and the "
             "event `cursor` to pass as `since` to `events` or `wait_for`.\n\n"
             "In combat with no hostile in sight, `combatHold` says why the game stays in combat: it leaves "
             "combat only once a living party member stands within 7.5 meters of each fallen one. It lists "
             "each fallen member with the nearest living one and how far away it is; walk one there.",
             Json::object(), {}),
        [&bench, &watch](const Json &args) -> ToolResult { return KindGame(bench, watch, args); });

    bench.Tools().Add(
        Read("data",
             "Reads the game's data tree, the values its interface displays. Use it to read what a screen "
             "shows without a screenshot.\n\n"
             "Pass a dotted `path` (\"GUI.SelectedChar\", \"Map.ShowAllLabels\", "
             "\"GUIPopupLayer.DialogBoxText\") to get its value as `text` and as `number`, and its `type`; or "
             "pass `paths` to read several at once (`values`). `found` is false for a path that does not "
             "exist.\n\n"
             "With `members: true`, it lists the members of the list at `path` (e.g. Container.Inventory, or "
             "the level-up screen's chains): their `count` and each member's path, which you can read the "
             "same way. Read only; works in any mode once the game has started.",
             Json{{"path", {{"type", "string"}, {"description", "A dotted path in the data tree."}}},
                  {"paths", {{"type", "array"}, {"description", "Up to 64 paths to read together."}}},
                  {"members",
                   {{"type", "boolean"},
                    {"description", "List the members of the list at `path` instead of reading its value. "
                                    "Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindData(watch, raw); });

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
    bench.Tools().Add(
        Read("plot",
             "Returns the state of a quest or story: the flags of the plot named in `plot`. Plot names are "
             "the game's plot files (and a mod's own), e.g. \"bed000pt_main\" or \"gen00pt_party\"; "
             "`resources` with type plo and `journal` list them.\n\n"
             "The answer includes the plot's `name` as the journal shows it, its `flags`, each with `flag` "
             "(number), `name`, whether it is `set`, and the `journal` text of a set flag that has one, and "
             "`set`, the names of all set flags. Pass `flag` to get just that one.\n\n"
             "Flags numbered 256 and up are normally computed by the plot's script, which this tool does not "
             "run; their values are what the game returns without it. Read only.",
             Json{{"plot", {{"type", "string"}, {"description", "The plot's name, e.g. bed000pt_main."}}},
                  {"flag", {{"description", "One flag to read, by number or name. Default: all flags."}}}},
             {"plot"}),
        [&watch](const Json &raw) -> ToolResult { return KindPlot(watch, raw); });

    bench.Tools().Add(
        Read("cursor",
             "Returns what is under the real mouse cursor right now, in explore and combat mode: `object`, "
             "what a click would act on (null if nothing), the `ground` point under it and whether it is "
             "`walkable`, and `hits`, everything the cursor's ray meets, nearest first (up to 8).\n\n"
             "After moving the real mouse, check here before clicking: a click just beside a creature is a "
             "move order. The game updates this 15 times a second. Empty `hits`, not even ground, means the "
             "cursor is over the interface (including a creature's name label) or outside the game's picture.",
             Json::object(), {}),
        [&watch](const Json &args) -> ToolResult { return KindCursor(watch, args); });

    bench.Tools().Add(
        Read("window",
             "Returns where the game's window is on the desktop, for tests that must use the real mouse: "
             "`client`, the top left corner of the game's picture in desktop pixels (`x`, `y`) and its "
             "`width` and `height`, and whether the window is `minimised` or the `foreground` window (the "
             "one that gets mouse and keyboard input).\n\n"
             "To click a spot seen in a screenshot, take its position as a fraction of the picture, multiply "
             "by the size and add the corner. Works in any mode.",
             Json::object(), {}),
        [](const Json &args) -> ToolResult { return KindWindow(args); });

    bench.Tools().Add(
        Read("dialogue.read",
             "The conversation on the screen (health's mode is dialog while one is): the `line` spoken last, "
             "with its speaker as the file writes it, `by` (the creature the game resolves it to) and `to` (whom it "
             "is spoken to) and text, the conversation's `owner`, and the `replies` the player may choose from now, "
             "in their order, "
             "each with its `place`, text and icon. No replies while the other speaks. The same comes as "
             "events: eluvian.dialogue.line, .replies and .chosen. `cutscene` and `movie` say whether one is playing. "
             "The dialogue tool answers.",
             Json::object(), {}, false),
        [&watch](const Json &args) -> ToolResult { return KindDialogue(watch, args); });

    bench.Tools().Add(
        Read(
            "placeables",
            "Lists the doors, chests, levers and other placeables in the area, nearest first. Each has its "
            "`id`, `name`, `tag`, `template`, `position` and `distance`, and:\n\n"
            "- `state`, the placeable's own state number (it changes when a door or chest opens), and "
            "`stateName`: shut, locked, open or destroyed.\n"
            "- `action` and `actionName`: what using it does now (loot, unlock, open, use, ...).\n"
            "- `active`, `interactive` (whether it can be used) and `popup`, its tooltip text.\n"
            "- `lock`: whether it needs a key (`keyRequired`), the key's tag (`keyTag`), whether the party "
            "has it (`keyHeld`), and the lock-picking level needed (`pickLevel`).\n"
            "- `leadsTo`, for a way out of the area: the destination area's tag and waypoint.\n\n"
            "Use one with `order` action=use.",
            Json{{"near",
                  {{"type", "string"},
                   {"description", "Measure distances from this creature, by id. Default: the creature you "
                                   "control."}}},
                 {"radius",
                  {{"type", "number"},
                   {"description", "Only those within this many meters of `near`. Default: "
                                   "the whole area."}}},
                 {"includeInactive",
                  {{"type", "boolean"}, {"description", "Also list placeables the area keeps hidden. Default false; then `inactiveLeftOut` says how many were left out."}}},
                 {"interactive",
                  {{"type", "boolean"},
                   {"description", "true: only those that can be used; false: only those that cannot. "
                                   "Default: both."}}},
                 {"brief",
                  {{"type", "boolean"},
                   {"description", "Return one line per placeable (`lines`) instead of full detail. "
                                   "Default false."}}}},
            {}),
        [&watch](const Json &raw) -> ToolResult { return KindPlaceables(watch, raw); });

    bench.Tools().Add(
        Read("surroundings",
             "A quick look around, one line per thing: the `creatures`, the placeables that can be used "
             "(`usable`: chests, doors, remains, with their state and lock) and the `exits` near a creature, "
             "nearest first. Each line is what `creatures`, `placeables` and `exits` return with "
             "`brief: true`. Call it on arriving anywhere; use those tools for full detail.",
             Json{{"near",
                   {{"type", "string"},
                    {"description", "Look around this creature, by id. Default: the creature you control."}}},
                  {"radius",
                   {{"type", "number"},
                    {"description", "Only things within this many meters of `near`. Default "
                                    "30."}}}},
             {}),
        [&bench, &watch](const Json &raw) -> ToolResult { return KindSurroundings(bench, watch, raw); });

    bench.Tools().Add(
        Read("exits",
             "Lists the ways out of the area: every door, other placeable or trigger that takes the party to "
             "another area, nearest first. Each has `leadsTo` (the destination area's tag and waypoint), its "
             "`kind` (placeable or trigger), `position`, `distance` and whether it is `active`.\n\n"
             "The easiest way to leave is `travel` with `exit` (an id from here) or `area`. By hand, use a "
             "placeable with `order` action=use, and walk into a trigger by moving to its position. Some "
             "doors only decide where they lead when used, so their `leadsTo` may be empty or show the last "
             "destination. Leaving loads the other area, and the game may autosave.",
             Json{{"near",
                   {{"type", "string"},
                    {"description", "Measure distances from this creature, by id. Default: the creature you "
                                    "control."}}},
                  {"radius",
                   {{"type", "number"},
                    {"description", "Only those within this many meters of `near`. Default: "
                                    "the whole area."}}},
                  {"brief",
                   {{"type", "boolean"},
                    {"description", "Return one line per exit (`lines`) instead of full detail. Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindExits(watch, raw); });

    // The area's triggers, which no other kind lists: the Gauntlet's bridge
    // turns on twelve tile triggers, found by script GetNearestObject and
    // read one GetLocalInt at a time (turn-based-agent, 2026-10-04, #49). A
    // puzzle's state lives in a trigger's locals, under names its scripts
    // choose, so the names are asked for.
    bench.Tools().Add(
        Read("triggers",
             "Lists the area's triggers, nearest first: each one's `id`, `tag`, `name`, `position`, "
             "`distance`, whether it is `active`, and `leadsTo` if it takes the party to another area "
             "(`exits` lists only those). A trigger is not used but walked into: `travel` or `order` move to "
             "its position.\n\n"
             "Pass `ints` to read local integer variables on each trigger, as its scripts set them, such as "
             "a puzzle's state or a switch's number (returned as `ints`).",
             Json{{"near",
                   {{"type", "string"},
                    {"description", "Measure distances from this creature, by id. Default: the creature you "
                                    "control."}}},
                  {"radius",
                   {{"type", "number"},
                    {"description", "Only those within this many meters of `near`. Default: "
                                    "the whole area."}}},
                  {"tag", {{"type", "string"}, {"description", "Only those whose tag contains this, in any case."}}},
                  {"ints",
                   {{"type", "array"},
                    {"description", "Names of local integer variables to read on each trigger, up to 32."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindTriggers(watch, raw); });

    bench.Tools().Add(
        Read("journal",
             "Lists the quests in the journal: `active`, the quest currently marked, and `quests`, those in "
             "progress. Each quest has its `plot` name (pass it to `plot` for its flags), its `name`, "
             "`description` and `objective` as the journal shows them, its `group` and `area`, whether it "
             "was `updated` or is `completed`, and `parent` if it is part of another quest. A finished quest "
             "may stay in `quests` with `completed` true.\n\n"
             "Finished quests are only counted (`completedCount`) unless you pass `completed: true`. Read "
             "only.",
             Json{{"completed",
                   {{"type", "boolean"}, {"description", "Also list the finished quests. Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindJournal(watch, raw); });

    bench.Tools().Add(
        Read("creatures",
             "Lists every creature in the area, nearest first. Each has its `id`, `name`, `tag`, "
             "`template`, `position`, `distance`, whether it is `dead`, its `health` and `manaStamina`, "
             "`combatState`, `attackTarget`, the `command` it is carrying out, and:\n\n"
             "- `side`: party, hostile or neutral, as seen by the creature you control (or the hero if "
             "none).\n"
             "- `group`: the game's own faction number: 0 the player's, 1 hostile, 2 friendly.\n"
             "- `room`: the room it stands in (`number`), whether the party has `seen` it (never, in view "
             "or before; a room never seen is drawn black, along with everything in it), whether it is "
             "`shown`, how much the game wants it (`wanted`: dropped, kept, now) and whether it is in memory "
             "(`load`). Null where the area has no rooms.\n\n"
             "`creature` gives one creature in full detail.",
             Json{{"near",
                   {{"type", "string"},
                    {"description", "Measure distances from this creature, by id. Default: the creature you "
                                    "control."}}},
                  {"radius",
                   {{"type", "number"},
                    {"description", "Only those within this many meters of `near`. Default: "
                                    "the whole area."}}},
                  {"side",
                   {{"type", "string"},
                    {"enum", {"party", "hostile", "neutral"}},
                    {"description", "Only creatures on this side. Default: all sides."}}},
                  {"includeDead", {{"type", "boolean"}, {"description", "Include dead creatures. Default true."}}},
                  {"includeInactive",
                   {{"type", "boolean"}, {"description", "Also list creatures the area keeps hidden. Default false; then `inactiveLeftOut` says how many were left out."}}},
                  {"brief",
                   {{"type", "boolean"},
                    {"description", "Return one line per creature (`lines`: id, name, tag, side, health, dead, "
                                    "distance) instead of full detail. Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindCreatures(watch, raw); });

    bench.Tools().Add(
        Read("creature",
             "Returns one creature in full: the summary `creatures` gives (including the `command` it is "
             "carrying out), plus:\n\n"
             "- `facing` and `appearance`.\n"
             "- `abilities`: those on its quickbar and in its tactics, each with its cooldown (seconds left, "
             "out of how many).\n"
             "- `path`: its current movement path; `beingWalked` says whether it is walking it now.\n"
             "- `effects`: every effect on it, with its type (an EFFECT_TYPE_* name), creator, ability (id "
             "and name) and time left in seconds (null for effects that do not expire).",
             Json{{"id",
                   {{"type", "string"}, {"description", "The creature, by id. Default: the creature you control."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindCreature(watch, raw); });

    bench.Tools().Add(
        Read("geometry",
             "Measures from a creature (`id`) to another creature or object (`target`) or a `point`. Returns "
             "the straight-line distance, the line of sight as the engine judges it (eye height to eye "
             "height, and where it is blocked, if it is; only walls and placeables block it, not creatures), "
             "and the creature's current movement path with its length and how much is left. "
             "`beingWalked` says whether it is walking that path now.\n\n"
             "With `walk: true`, it also asks the game's pathfinder for the path the creature would walk "
             "there now: `found` and its `length`, or why not. If a shut door or similar is in the way, "
             "`blocker` names it: use it first, then move. The creature is not moved.",
             Json{{"id",
                   {{"type", "string"},
                    {"description", "The creature to measure from, by id. Default: the creature you control."}}},
                  {"target", {{"type", "string"}, {"description", "The creature or object to measure to, by id."}}},
                  {"point", {{"description", "The point to measure to: {x, y, z} or [x, y, z]."}}},
                  {"walk",
                   {{"type", "boolean"},
                    {"description", "Also find the path the creature would walk there: its length, or what is in "
                                    "the way. Default false."}}},
                  {"trace",
                   {{"type", "boolean"},
                    {"description", "With `walk`: include the path search's internal log, for diagnosing a "
                                    "search that failed. Default false."}}},
                  {"waypoints",
                   {{"type", "boolean"}, {"description", "List the points along the paths. Default false."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return KindGeometry(watch, raw); });
}

} // namespace eluvian::game
