// Ghilana's tools over what the files say: data tables merged as the game
// merges them (table), talk tables both ways (strings), every file of a
// type searched by its fields (query), and what a resource names and what
// names it (references).
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/FileJson.h"
#include "ghilana/Text.h"

#include <dao/core/GdaFile.h>
#include <dao/core/Gff3.h>
#include <dao/core/Gff4.h>
#include <dao/core/M2da.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace eluvian::ghilana::detail
{

namespace
{

namespace m2da = dao::core::m2da;

ToolSpec Spec(std::string name, std::string description, Json properties, const std::vector<std::string> &required = {})
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
    {
        schema["required"] = required;
    }
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Listener, true};
}

void Put(Json &out, const char *key, const Json &value)
{
    if (!value.is_null())
    {
        out[key] = value;
    }
}

double Since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::vector<std::string> StringList(const Json &value)
{
    std::vector<std::string> out;
    if (value.is_string())
    {
        out.push_back(value.get<std::string>());
    }
    else if (value.is_array())
    {
        for (const auto &item : value)
        {
            if (item.is_string())
            {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

// A file's fields as JSON, GFF3 or GFF4, with its format; null for another
// kind of file.
Json FieldsOf(std::span<const std::byte> bytes, const Limits &limits)
{
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "GFF V4.", 7) == 0)
    {
        if (const auto file = dao::core::gff4::File::Parse(bytes))
        {
            return Gff4ToJson(*file, limits);
        }
        return nullptr;
    }
    if (bytes.size() >= 8 &&
        (std::memcmp(bytes.data() + 4, "V3.2", 4) == 0 || std::memcmp(bytes.data() + 4, "V3.3", 4) == 0))
    {
        if (const auto file = dao::core::gff3::File::Parse(bytes))
        {
            return Gff3ToJson(*file, limits);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- table

std::optional<std::size_t> FindWorksheet(const std::vector<m2da::Worksheet> &sheets, const std::string &asked)
{
    std::int64_t number = 0;
    const auto *end = asked.data() + asked.size();
    if (const auto [at, error] = std::from_chars(asked.data(), end, number); error == std::errc() && at == end)
    {
        for (std::size_t i = 0; i < sheets.size(); ++i)
        {
            if (sheets[i].merged && sheets[i].id == number)
            {
                return i;
            }
        }
        return std::nullopt;
    }
    const std::string lower = Lower(asked);
    for (std::size_t i = 0; i < sheets.size(); ++i)
    {
        const std::string prefix = Utf8(sheets[i].prefix);
        if (prefix == lower || prefix == lower + "_" || Lower(Utf8(sheets[i].label)) == lower)
        {
            return i;
        }
    }
    return std::nullopt;
}

// The table asked for: its worksheet if it has one, and its files' names.
struct TableFiles
{
    std::optional<std::size_t> sheet;
    std::vector<std::string> names;
};

// The worksheet a file asked by its name stands for: its own table if it is
// one (aip_archer_dwarf is part of aip_archer's and its own), else the first
// it is part of.
std::optional<std::size_t> SheetOfFile(const std::vector<m2da::Worksheet> &sheets, const std::string &stem)
{
    const auto of = m2da::WorksheetsOf(sheets, Wide(stem));
    const auto own = std::ranges::find_if(of, [&](std::size_t i) { return Utf8(sheets[i].prefix) == stem; });
    if (own != of.end())
    {
        return *own;
    }
    return of.empty() ? std::nullopt : std::optional<std::size_t>(of.front());
}

TableFiles FindTable(const GameFiles &files, const std::vector<m2da::Worksheet> &sheets, const std::string &asked)
{
    std::string stem = Lower(asked);
    if (stem.ends_with(".gda"))
    {
        stem.resize(stem.size() - 4);
    }
    TableFiles found;
    // A file's name, with its extension or without, is its file's table first: 2da_base's older
    // single table BITM_base took "bitm_base" from bitm_base.gda's table, BITM_, and found no
    // file of its own (paragon-agent, 2026-10-17, #152).
    const bool isFile = files.Find(stem + ".gda") != nullptr;
    found.sheet = isFile ? SheetOfFile(sheets, stem) : FindWorksheet(sheets, asked);
    if (!found.sheet)
    {
        if (isFile)
        {
            found.names.push_back(stem + ".gda");
        }
        return found;
    }
    for (const auto &[name, copies] : files.Resources())
    {
        if (Extension(name) != "gda")
        {
            continue;
        }
        const auto of = m2da::WorksheetsOf(sheets, Wide(Stem(name)));
        if (std::ranges::find(of, *found.sheet) != of.end())
        {
            found.names.push_back(name);
        }
    }
    return found;
}

// A table's files read, as the game takes them (DAOLib's dev/FILES.md,
// "Merged tables"): of each name the copy the game takes, the copies it
// shadows said, a file the game has no copy of left out with why; the parts
// in the order the game lists its files (by source, then by name), for
// m2da::Merge.
struct PartsRead
{
    std::vector<TablePart> parts;
    Json unreadable = Json::array();
    Json shadowed = Json::array();
    Json unused = Json::array();
};

PartsRead ReadParts(const GameFiles &files, const std::vector<std::string> &names)
{
    PartsRead read;
    std::vector<dao::core::install::NamedCopy> taken;
    for (const auto &name : names)
    {
        const auto &copies = *files.Find(name);
        const Copy *game = GameFiles::Taken(copies);
        if (game == nullptr)
        {
            read.unused.push_back(
                Json{{"file", name}, {"why", "the game has no copy of it: " + files.WhyUnused(copies.front())}});
            continue;
        }
        for (std::size_t i = 1; i < copies.size(); ++i)
        {
            read.shadowed.push_back(Json{{"file", name}, {"copy", files.Describe(copies[i])}});
        }
        taken.push_back({name, *game});
    }
    files.Installed().SortFiles(taken);
    bool unreadableBefore = false;
    for (const auto &[name, copy] : taken)
    {
        std::string why;
        const auto bytes = files.Read(copy, &why);
        auto table = bytes ? dao::core::GdaFile::Parse(*bytes) : std::nullopt;
        if (!table)
        {
            read.unreadable.push_back(Json{{"file", name},
                                           {"copy", files.Describe(copy)},
                                           {"why", bytes ? "not a GDA table ghilana reads" : why}});
            unreadableBefore = true;
            continue;
        }
        read.parts.push_back(TablePart{name, files.Describe(copy), std::move(*table), unreadableBefore});
    }
    return read;
}

std::optional<std::size_t> ColumnNamed(const m2da::Merged &merged, const std::string &name)
{
    for (std::size_t c = 0; c < merged.columns.size(); ++c)
    {
        if (Lower(ColumnName(merged.columns[c].hash)) == Lower(name))
        {
            return c;
        }
    }
    return std::nullopt;
}

// Which rows and columns a call wants of a merged table. The ids a sorted
// vector: a std::set's move may ask for memory in MSVC's library, which
// makes the struct's move one that may throw.
struct RowFilter
{
    std::vector<std::int64_t> ids;
    std::vector<std::pair<std::size_t, Json>> where;
    std::vector<std::size_t> shown;
    bool overridesOnly = false; // only IDs files give different rows
    bool dropped = false;       // the rows the game drops too

    [[nodiscard]] bool Wants(const m2da::Row &row) const
    {
        if (!dropped && !row.taken)
        {
            return false;
        }
        if (!ids.empty() && (!row.id || !std::ranges::binary_search(ids, *row.id)))
        {
            return false;
        }
        if (overridesOnly && !row.conflict)
        {
            return false;
        }
        return std::ranges::all_of(
            where, [&row](const auto &w) { return CellMatches(CellJson(row.cells[w.first]), w.second); });
    }
};

std::vector<std::int64_t> IdsOf(const Args &args)
{
    std::vector<std::int64_t> ids;
    if (!args.Has("id"))
    {
        return ids;
    }
    const Json &id = args.Raw().at("id");
    for (const auto &one : id.is_array() ? id : Json::array({id}))
    {
        if (!one.is_number_integer())
        {
            throw ToolError("table: `id` is a row's ID, or an array of them");
        }
        ids.push_back(one.get<std::int64_t>());
    }
    std::ranges::sort(ids);
    return ids;
}

RowFilter FilterOf(const Args &args, const m2da::Merged &merged)
{
    RowFilter filter;
    filter.ids = IdsOf(args);
    filter.overridesOnly = args.Bool("overrides_only", false);
    filter.dropped = args.Bool("dropped", false);
    if (const Json &w = args.Raw().value("where", Json()); w.is_object())
    {
        for (const auto &[column, value] : w.items())
        {
            const auto found = ColumnNamed(merged, column);
            if (!found)
            {
                std::string known;
                for (const auto &c : merged.columns)
                {
                    known += known.empty() ? "" : ", ";
                    known += ColumnName(c.hash);
                }
                std::string said = "table: no column \"" + column + "\"; its columns are ";
                said += known;
                throw ToolError(said);
            }
            filter.where.emplace_back(*found, value);
        }
    }
    const auto wanted = StringList(args.Raw().value("columns", Json()));
    for (std::size_t c = 0; c < merged.columns.size(); ++c)
    {
        const auto named = [&](const std::string &w) { return Lower(w) == Lower(ColumnName(merged.columns[c].hash)); };
        if (wanted.empty() || std::ranges::any_of(wanted, named))
        {
            filter.shown.push_back(c);
        }
    }
    return filter;
}

// For each ID, the files whose different rows of it the game drops.
std::map<std::int64_t, std::vector<std::string>> DroppedFiles(const m2da::Merged &merged,
                                                              const std::vector<TablePart> &parts)
{
    std::map<std::int64_t, std::vector<std::string>> out;
    for (const auto &row : merged.rows)
    {
        if (!row.taken && row.id)
        {
            out[*row.id].push_back(parts[row.part].file);
        }
    }
    return out;
}

Json RowJson(const m2da::Merged &merged, const std::vector<TablePart> &parts, const m2da::Row &row,
             const std::vector<std::size_t> &shown, const std::map<std::int64_t, std::vector<std::string>> &dropped)
{
    Json out = Json::object();
    for (const std::size_t c : shown)
    {
        out[ColumnName(merged.columns[c].hash)] = CellJson(row.cells[c]);
    }
    const TablePart &part = parts[row.part];
    out["_file"] = part.file;
    out["_source"] = part.from["source"];
    out["_in"] = part.from.contains("archive") ? part.from["archive"] : part.from["file"];
    if (!row.alike.empty())
    {
        out["_files"] = row.alike.size() + 1;
    }
    if (!row.taken)
    {
        out["_dropped"] = true;
    }
    else if (const auto over = row.id ? dropped.find(*row.id) : dropped.end(); over != dropped.end())
    {
        out["_over"] = over->second;
    }
    if (part.afterUnreadable)
    {
        out["_unsure"] = true;
    }
    return out;
}

Json TableHead(const m2da::Merged &merged, const std::vector<TablePart> &parts, const std::vector<std::size_t> &shown)
{
    Json fileList = Json::array();
    for (const auto &part : parts)
    {
        fileList.push_back(Json{{"file", part.file}, {"copy", part.from}, {"rows", part.table.Rows().size()}});
    }
    Json columns = Json::array();
    for (const std::size_t c : shown)
    {
        columns.push_back(Json{{"name", ColumnName(merged.columns[c].hash)}, {"type", ColumnType(merged.columns[c])}});
    }
    return Json{{"files", std::move(fileList)}, {"columns", std::move(columns)}};
}

// What the answer says of how the rows were decided: the IDs one file's row
// overrides others' of, the files the game takes no row from, and the files
// that could not be read.
void Decided(const m2da::Merged &merged, PartsRead &read, Json &out)
{
    if (merged.conflicts > 0)
    {
        out["overrides"] = {{"ids", merged.conflicts},
                            {"how", "When two files give the same ID, the game keeps the row from the file it reads "
                                    "first: files are ordered by source, then by name within a source. `_over` names "
                                    "the files whose rows were dropped; "
                                    "`dropped` shows those rows."}};
    }
    Json skipped = Json::array();
    for (const std::size_t p : merged.skipped)
    {
        skipped.push_back(read.parts[p].file);
    }
    if (!skipped.empty())
    {
        out["skipped"] = {{"files", std::move(skipped)},
                          {"why", "their first column is not ID; the game takes no "
                                  "row from them"}};
    }
    for (auto [key, list] : {std::pair{"shadowed", &read.shadowed}, std::pair{"not_in_game", &read.unused}})
    {
        if (!list->empty())
        {
            out[key] = std::move(*list);
        }
    }
    if (!read.unreadable.empty())
    {
        out["unreadable"] = {{"files", std::move(read.unreadable)},
                             {"note", "their rows are not here, so `count` is of the rows ghilana could read and "
                                      "the game's table has more (on the GOG install, Projectiles' 67 here are 76 "
                                      "in the game); a row marked _unsure, from a file after one of them in the "
                                      "game's order, may be one the game drops for theirs"}};
    }
}

Json Table(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "table");
    const Json &tableArg = args.Raw().at("table");
    const std::string asked = tableArg.is_string() ? tableArg.get<std::string>() : tableArg.dump();
    const Page page(args, 100, 10000);
    const auto start = std::chrono::steady_clock::now();
    Json indexed = ghilana.Indexed();
    const auto &sheets = ghilana.Worksheets();
    const TableFiles table = FindTable(ghilana.Files(), sheets, asked);
    if (table.names.empty())
    {
        throw ToolError("table: no table \"" + asked +
                        "\": give a table's number (m2da_base.gda's ID, as the scripts' TABLE_* constants), its "
                        "worksheet prefix (\"abi\"), its label (\"Abilities\") or a GDA file's name (\"abi_base\"); "
                        "`resources` with type gda lists the files");
    }
    PartsRead read = ReadParts(ghilana.Files(), table.names);
    const auto &parts = read.parts;
    const m2da::Merged merged = MergeParts(parts);
    const RowFilter filter = FilterOf(args, merged);
    const auto dropped = DroppedFiles(merged, parts);
    Json rows = Json::array();
    std::size_t count = 0;
    for (const auto &row : merged.rows)
    {
        if (filter.Wants(row) && page.Takes(count++, rows.size()))
        {
            rows.push_back(RowJson(merged, parts, row, filter.shown, dropped));
        }
    }

    Json out = Json::object();
    Put(out, "index", indexed);
    if (table.sheet)
    {
        const m2da::Worksheet &sheet = sheets[*table.sheet];
        out["table"] = {{"id", sheet.id},
                        {"label", Utf8(sheet.label)},
                        {"prefix", Utf8(sheet.prefix)},
                        {"listed_in", sheet.merged ? "m2da_base.gda" : "2da_base.gda"},
                        {"single", sheet.Single()}};
    }
    out.update(TableHead(merged, parts, filter.shown));
    Decided(merged, read, out);
    page.Answer(out, count, "rows", std::move(rows));
    out["ms"] = static_cast<std::int64_t>(Since(start));
    return out;
}

// ---------------------------------------------------------------- strings

Json StringsById(const std::vector<LoadedTalkTable> &tables, const Json &id)
{
    Json found = Json::array();
    for (const auto &one : id.is_array() ? id : Json::array({id}))
    {
        if (!one.is_number_integer() || one.get<std::int64_t>() < 0)
        {
            throw ToolError("strings: `id` is a string id (a strref), or an array of them");
        }
        const auto wanted = static_cast<std::uint32_t>(one.get<std::int64_t>());
        Json hits = Json::array();
        for (const auto &table : tables)
        {
            if (const auto text = table.table.String(wanted))
            {
                hits.push_back(Json{{"text", Utf8(*text)}, {"table", table.name}, {"source", table.from["source"]}});
            }
        }
        Json item = {{"id", wanted}};
        if (hits.empty())
        {
            item["text"] = nullptr;
            item["why"] = "no talk table of this language has it";
        }
        else
        {
            item["text"] = hits[0]["text"];
            item["table"] = hits[0]["table"];
        }
        if (hits.size() > 1)
        {
            item["in"] = std::move(hits);
            item["note"] = "more than one talk table has this id; which the game shows is unread";
        }
        found.push_back(std::move(item));
    }
    return found;
}

void StringsByText(const std::vector<LoadedTalkTable> &tables, const std::string &text, bool exact, const Page &page,
                   Json &out)
{
    const std::string lowerText = Lower(text);
    Json found = Json::array();
    std::size_t count = 0;
    for (const auto &table : tables)
    {
        for (const auto &[id, wide] : table.table.Strings())
        {
            const std::string utf8 = Utf8(wide);
            const std::string lower = Lower(utf8);
            const bool match = exact ? lower == lowerText : lower.find(lowerText) != std::string::npos;
            if (match && page.Takes(count++, found.size()))
            {
                found.push_back(Json{{"id", id}, {"text", utf8}, {"table", table.name}});
            }
        }
    }
    page.Answer(out, count, "strings", std::move(found));
}

Json Strings(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "strings");
    const std::string language = Lower(args.String("language", ghilana.Files().Where().language));
    const bool female = args.Bool("female", false);
    const Page page(args, 50, 5000);
    Json indexed = ghilana.Indexed();
    const auto &tables = ghilana.TalkTables(language, female);
    if (tables.empty())
    {
        throw ToolError("strings: no talk table of language \"" + language + "\" (files named <name>_" + language +
                        ".tlk); `resources` with type tlk lists them");
    }
    Json out = Json::object();
    Put(out, "index", indexed);
    out["language"] = language;
    if (args.Has("id"))
    {
        out["strings"] = StringsById(tables, args.Raw().at("id"));
        return out;
    }
    const std::string text = args.String("text", "");
    if (!text.empty())
    {
        StringsByText(tables, text, args.Bool("exact", false), page, out);
        return out;
    }
    Json tableNames = Json::array();
    for (const auto &table : tables)
    {
        tableNames.push_back(
            Json{{"name", table.name}, {"source", table.from["source"]}, {"strings", table.table.Size()}});
    }
    out["tables"] = std::move(tableNames);
    out["note"] = "give `id` to read strings, or `text` to find them";
    return out;
}

// ---------------------------------------------------------------- query

// A field's value found in a file, and where: its path with each list's
// element by index ("StageCameraList[2].StageCameraLookingAtType"), and
// whether a list was gone through to reach it.
struct Found
{
    const Json *value = nullptr;
    std::string path;
    bool listed = false;
};

std::string Joined(const std::string &at, const std::string &key)
{
    return at.empty() ? key : at + "." + key;
}

// The values at a dotted path below `node`, each key without case. A list
// on the way is each of its elements: a stage's cameras are a list, and
// StageCameraList.StageCameraLookingAtType matched none of 497 stages
// though 2p_200_cullen.stg has one of type 1 (2026-10-19, #162). A list
// the path ends at is the list's value.
void AtPath(const Json &node, std::string_view path, const std::string &at, bool listed, std::vector<Found> &out)
{
    if (path.empty())
    {
        out.push_back(Found{&node, at, listed});
        return;
    }
    if (node.is_array())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
        {
            AtPath(node[i], path, at + "[" + std::to_string(i) + "]", true, out);
        }
        return;
    }
    if (!node.is_object())
    {
        return;
    }
    const auto dot = path.find('.');
    const std::string key = Lower(path.substr(0, dot));
    const std::string_view rest = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
    for (auto it = node.begin(); it != node.end(); ++it)
    {
        if (Lower(it.key()) == key)
        {
            AtPath(it.value(), rest, Joined(at, it.key()), listed, out);
            return;
        }
    }
}

// The values of a field: at a dotted path ("Appearance Data.Appearance_Type"),
// each key without case; a bare name at the top, or else in the nearest
// structs below that have it, breadth first, a list's elements among them.
// A creature's Appearance_Type is in its "Appearance Data"
// (cir320cr_ogre_boss.utc, 2026-10-12), and the plan's own query names it
// bare; a stage's StageCameraLookingAtType is in each camera of its list
// (#162).
std::vector<Found> At(const Json &root, std::string_view path)
{
    std::vector<Found> found;
    if (path.find('.') != std::string_view::npos)
    {
        AtPath(root, path, "", false, found);
        return found;
    }
    std::vector<Found> level{Found{&root, "", false}};
    while (!level.empty() && found.empty())
    {
        std::vector<Found> next;
        for (const Found &node : level)
        {
            AtPath(*node.value, path, node.path, node.listed, found);
            for (auto it = node.value->begin(); node.value->is_object() && it != node.value->end(); ++it)
            {
                if (it.value().is_object())
                {
                    next.push_back(Found{&it.value(), Joined(node.path, it.key()), node.listed});
                }
                for (std::size_t i = 0; it.value().is_array() && i < it.value().size(); ++i)
                {
                    if (it.value()[i].is_object())
                    {
                        next.push_back(
                            Found{&it.value()[i], Joined(node.path, it.key()) + "[" + std::to_string(i) + "]", true});
                    }
                }
            }
        }
        level = std::move(next);
    }
    return found;
}

// Whether a field's value is what a call asked: CellMatches, a localised
// string's strref for a number, any of an array.
bool FieldMatches(const Json &value, const Json &wanted)
{
    if (wanted.is_array())
    {
        return std::ranges::any_of(wanted, [&value](const Json &one) { return FieldMatches(value, one); });
    }
    if (value.is_object() && value.contains("strref") && !wanted.is_object())
    {
        return CellMatches(value["strref"], wanted);
    }
    return CellMatches(value, wanted);
}

// Whether a file has what `where` asks, each field matching where any of
// its values does; and, in `matched`, for a field reached through a list,
// the paths of the elements' values that matched.
bool Matches(const Json &json, const Json &where, Json &matched)
{
    for (const auto &w : where.items())
    {
        Json paths = Json::array();
        bool any = false;
        for (const Found &found : At(json, w.key()))
        {
            if (FieldMatches(*found.value, w.value()))
            {
                any = true;
                if (found.listed)
                {
                    paths.push_back(found.path);
                }
            }
        }
        if (!any)
        {
            return false;
        }
        if (!paths.empty())
        {
            matched[w.key()] = std::move(paths);
        }
    }
    return true;
}

// What group_by counts a match under: the field's value, a localised
// string's id; through a list, each element's value, so a stage of three
// cameras counts three times; with none, "null".
std::vector<std::string> GroupKeys(const Json &json, const std::string &field)
{
    std::vector<std::string> keys;
    for (const Found &found : At(json, field))
    {
        const Json &value = *found.value;
        const Json &key = value.is_object() && value.contains("strref") ? value["strref"] : value;
        keys.push_back(key.is_string() ? key.get<std::string>() : key.dump());
    }
    if (keys.empty())
    {
        keys.emplace_back("null");
    }
    return keys;
}

// The fields asked for: each one's value, or through a list every
// element's, as an array.
Json Picked(const Json &json, const std::vector<std::string> &fields)
{
    Json picked = Json::object();
    for (const auto &field : fields)
    {
        const auto found = At(json, field);
        if (found.empty())
        {
            picked[field] = nullptr;
        }
        else if (found.size() == 1 && !found.front().listed)
        {
            picked[field] = *found.front().value;
        }
        else
        {
            Json values = Json::array();
            for (const Found &one : found)
            {
                values.push_back(*one.value);
            }
            picked[field] = std::move(values);
        }
    }
    return picked;
}

// A query's run over every copy of a type: what it read, matched and counted.
struct QueryRun
{
    std::size_t read = 0;
    std::size_t unreadable = 0;
    std::size_t otherFormat = 0;
    std::size_t count = 0;
    Json matches = Json::array();
    std::map<std::string, std::size_t> groups;
};

Json QueryAnswer(QueryRun &run, const std::string &type, const std::string &groupBy, const Page &page)
{
    Json out = {{"type", type}, {"files_read", run.read}};
    if (run.unreadable > 0)
    {
        out["unreadable"] = {{"files", run.unreadable}, {"why", "in enciphered add-in archives; not searched"}};
    }
    if (run.otherFormat > 0)
    {
        out["not_gff"] = run.otherFormat;
    }
    page.Answer(out, run.count, "matches", std::move(run.matches));
    if (!groupBy.empty())
    {
        std::vector<std::pair<std::string, std::size_t>> sorted(run.groups.begin(), run.groups.end());
        std::ranges::stable_sort(sorted, std::greater<>(), &std::pair<std::string, std::size_t>::second);
        Json grouped = Json::array();
        for (const auto &[value, n] : sorted)
        {
            grouped.push_back(Json{{"value", value}, {"count", n}});
        }
        out["groups"] = std::move(grouped);
    }
    if (run.read == 0 && run.unreadable == 0)
    {
        out["note"] =
            "no file of type \"" + type + "\" was read; the type is a resource's extension (utc, uti, plo, dlg)";
    }
    return out;
}

// What a query asks of each file.
struct QueryAsk
{
    const Json &where;
    const std::string &groupBy;
    const std::vector<std::string> &fields;
    Limits limits;
};

// One copy read, matched and counted.
void QueryCopy(const GameFiles &files, const std::string &name, const Copy &copy, const QueryAsk &ask, const Page &page,
               QueryRun &run)
{
    const auto bytes = files.Read(copy);
    const Json json = bytes ? FieldsOf(*bytes, ask.limits) : Json(nullptr);
    run.unreadable += bytes ? 0 : 1;
    run.otherFormat += bytes && json.is_null() ? 1 : 0;
    if (json.is_null())
    {
        return;
    }
    ++run.read;
    Json matched = Json::object();
    if (!Matches(json, ask.where, matched))
    {
        return;
    }
    if (!ask.groupBy.empty())
    {
        for (const auto &key : GroupKeys(json, ask.groupBy))
        {
            ++run.groups[key];
        }
    }
    if (page.Takes(run.count++, run.matches.size()))
    {
        Json item = {{"name", name}, {"copy", files.Describe(copy)}};
        if (!matched.empty())
        {
            item["matched"] = std::move(matched);
        }
        if (!ask.fields.empty())
        {
            item["fields"] = Picked(json, ask.fields);
        }
        run.matches.push_back(std::move(item));
    }
}

Json Query(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "query");
    const std::string type = Lower(args.String("type"));
    const Page page(args, 50, 5000);
    const std::string groupBy = args.String("group_by", "");
    const auto fields = StringList(args.Raw().value("fields", Json()));
    const Json where = args.Raw().value("where", Json::object());
    if (!where.is_object())
    {
        throw ToolError(R"(query: `where` is an object of field to value: {"Appearance_Type": 80})");
    }
    const auto start = std::chrono::steady_clock::now();
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto source = SourceArg(files, args, "query");
    const Limits limits{static_cast<int>(args.Integer("depth", 6, 1, 32)),
                        static_cast<std::size_t>(args.Integer("items", 100, 1, 100000))};
    QueryRun run;
    const QueryAsk ask{where, groupBy, fields, limits};
    const bool every = args.Choice("copies", {"game", "all"}, "game") == "all";
    for (const auto &[name, copies] : files.Resources())
    {
        if (Extension(name) != type)
        {
            continue;
        }
        for (const Copy &copy : copies)
        {
            const bool game = &copy == GameFiles::Taken(copies);
            if ((every || game) && (!source || copy.source == *source))
            {
                QueryCopy(files, name, copy, ask, page, run);
            }
        }
    }
    Json out = Json::object();
    Put(out, "index", indexed);
    out.update(QueryAnswer(run, type, groupBy, page));
    out["ms"] = static_cast<std::int64_t>(Since(start));
    return out;
}

// ---------------------------------------------------------------- references

// Every value in a file's JSON with where it is.
void Walk(const Json &node, const std::string &path,
          const std::function<void(const std::string &, const Json &)> &visit)
{
    if (node.is_object())
    {
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            Walk(it.value(), path.empty() ? it.key() : path + "." + it.key(), visit);
        }
    }
    else if (node.is_array())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
        {
            Walk(node[i], path + "[" + std::to_string(i) + "]", visit);
        }
    }
    else
    {
        visit(path, node);
    }
}

// A file's content as fields: GFF's, or a table's rows as {column: value}.
Json ContentOf(std::span<const std::byte> bytes)
{
    if (const auto table = dao::core::GdaFile::Parse(bytes))
    {
        Json rows = Json::array();
        for (const auto &cells : table->Rows())
        {
            Json row = Json::object();
            for (std::size_t c = 0; c < table->Columns().size(); ++c)
            {
                row[ColumnName(table->Columns()[c].hash)] = CellJson(cells[c]);
            }
            rows.push_back(std::move(row));
        }
        return Json{{"rows", std::move(rows)}};
    }
    return FieldsOf(bytes, Limits{32, SIZE_MAX});
}

// The resources a text names, with or without its extension; not `self`,
// as a template's tag is often its own name.
std::vector<std::string> Named(const GameFiles &files, const std::string &text, const std::string &self)
{
    std::vector<std::string> named;
    if (files.Find(text) != nullptr)
    {
        if (text != self)
        {
            named.push_back(text);
        }
        return named;
    }
    const std::string prefix = text + ".";
    for (auto it = files.Resources().lower_bound(prefix);
         it != files.Resources().end() && it->first.starts_with(prefix); ++it)
    {
        if (it->first != self)
        {
            named.push_back(it->first);
        }
    }
    return named;
}

bool IsStrref(const std::string &path, const Json &value)
{
    if (!value.is_number_integer() || !Lower(path).ends_with("strref"))
    {
        return false;
    }
    const auto id = value.get<std::int64_t>();
    return id >= 0 && id != std::int64_t{0xFFFFFFFF};
}

// What a file's content names: resources, and string ids.
void Uses(const GameFiles &files, const std::string &name, const Json &content, std::size_t limit, Json &out)
{
    Json uses = Json::array();
    Json strrefs = Json::array();
    std::set<std::string> seen;
    Walk(content, "", [&](const std::string &path, const Json &value) {
        if (IsStrref(path, value))
        {
            if (strrefs.size() < limit)
            {
                strrefs.push_back(Json{{"field", path}, {"id", value}});
            }
            return;
        }
        const std::string text = value.is_string() ? Lower(value.get<std::string>()) : "";
        if (text.empty() || text.size() > 64 || seen.contains(text) || uses.size() >= limit)
        {
            return;
        }
        if (auto named = Named(files, text, name); !named.empty())
        {
            seen.insert(text);
            uses.push_back(Json{{"field", path}, {"value", value}, {"resources", std::move(named)}});
        }
    });
    out["uses"] = std::move(uses);
    if (!strrefs.empty())
    {
        out["strrefs"] = std::move(strrefs);
        out["strrefs_note"] = "`strings` with these ids gives their text";
    }
}

// Every file of the types asked with a field naming `name`.
void UsedBy(const GameFiles &files, const std::string &name, const std::vector<std::string> &types, std::size_t limit,
            Json &out)
{
    const std::string stem = Stem(name);
    std::set<std::string> wanted;
    for (const auto &t : types)
    {
        wanted.insert(Lower(t));
    }
    Json usedBy = Json::array();
    std::size_t scanned = 0;
    std::size_t unreadable = 0;
    for (const auto &[other, copies] : files.Resources())
    {
        if (!wanted.contains(Extension(other)) || other == name)
        {
            continue;
        }
        if (const Copy *copy = GameFiles::Taken(copies))
        {
            const auto bytes = files.Read(*copy);
            unreadable += bytes ? 0 : 1;
            if (!bytes)
            {
                continue;
            }
            ++scanned;
            std::vector<std::string> fields;
            Walk(ContentOf(*bytes), "", [&](const std::string &path, const Json &value) {
                if (value.is_string() &&
                    (Lower(value.get<std::string>()) == name || Lower(value.get<std::string>()) == stem))
                {
                    fields.push_back(path);
                }
            });
            if (!fields.empty() && usedBy.size() < limit)
            {
                usedBy.push_back(Json{{"name", other}, {"copy", files.Describe(*copy)}, {"fields", std::move(fields)}});
            }
        }
    }
    out["used_by"] = std::move(usedBy);
    out["used_by_scanned"] = scanned;
    if (unreadable > 0)
    {
        out["used_by_unreadable"] = unreadable;
    }
}

Json References(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "references");
    const std::string name = Lower(args.String("name"));
    const auto usedByTypes = StringList(args.Raw().value("used_by_types", Json()));
    const auto limit = static_cast<std::size_t>(args.Integer("limit", 100, 1, 5000));
    const auto start = std::chrono::steady_clock::now();
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select("references", name, args.String("source", ""), args.String("archive", ""));
    const Copy &copy = GameFiles::Choose(copies);
    std::string why;
    const auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        throw ToolError("references: \"" + name + "\" cannot be read: " + why);
    }
    const Json content = ContentOf(*bytes);
    if (content.is_null())
    {
        throw ToolError("references: \"" + name + "\" is no GFF file or data table; references reads those");
    }
    Json out = Chosen(files, name, *files.Find(name), copy);
    Put(out, "index", indexed);
    Uses(files, name, content, limit, out);
    if (usedByTypes.empty())
    {
        out["used_by"] = "not searched: give `used_by_types` (\"utc\", \"dlg\", \"gda\"...) to search every file of "
                         "those types for this name";
    }
    else
    {
        UsedBy(files, name, usedByTypes, limit, out);
    }
    out["ms"] = static_cast<std::int64_t>(Since(start));
    return out;
}

} // namespace

void RegisterDataTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();

    Json tableProperties = {
        {"table",
         {{"type", {"string", "integer"}},
          {"description", "The table: its number (m2da_base.gda's ID, as the scripts' TABLE_* constants: 1 is "
                          "Abilities), its worksheet prefix (\"abi\"), its label (\"Abilities\"), or a GDA file's "
                          "name (\"abi_base\")."}}},
        {"id",
         {{"type", {"integer", "array"}},
          {"items", {{"type", "integer"}}},
          {"description", "Only the rows of this ID, or these IDs."}}},
        {"where",
         {{"type", "object"},
          {"description", R"(Only rows whose columns have these values: {"Label": "Flurry"}. Text without case.)"}}},
        {"columns",
         {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these columns. Default: all."}}},
        {"overrides_only",
         {{"type", "boolean"},
          {"description", "Only the IDs that files give different rows, the row the game keeps of each (with "
                          "`dropped`, the others too). Default false."}}},
        {"dropped",
         {{"type", "boolean"},
          {"description", "Also the rows the game drops: a later file's row of an ID an earlier file has, marked "
                          "_dropped. Default false: the table as the game has it."}}}};
    tableProperties.update(PageProperties(100, 10000));
    tools.Add(Spec("table",
                   "Reads one of the game's data tables merged the way the game merges it (M2DA). The table is "
                   "built from every GDA file whose name starts with the table's prefix, from every source (for "
                   "example the core's abi_base.gda and a mod's abi_*.gda), using the copy of each file the game "
                   "uses. Files are ordered by source, then by name; when two files give the same ID, the game "
                   "keeps the row from the first. Each row says which file and source it comes from, and `_over` "
                   "lists the files whose rows for that ID it replaces. Filter with `id` or `where`; use "
                   "`overrides_only` to see only what mods and add-ins change; page with `limit`/`offset`.",
                   std::move(tableProperties), {"table"}),
              [&ghilana](const Json &args) -> ToolResult { return Table(ghilana, args); });

    Json stringsProperties = {
        {"id",
         {{"type", {"integer", "array"}},
          {"items", {{"type", "integer"}}},
          {"description", "A string id (strref), or an array of them: their text."}}},
        {"text",
         {{"type", "string"}, {"description", "Text to find, without case: the ids of the strings holding it."}}},
        {"exact", {{"type", "boolean"}, {"description", "`text` is the whole string, not part of it. Default false."}}},
        {"language",
         {{"type", "string"}, {"description", "en-us, fr-fr, de-de, ... Default: the server's (--language)."}}},
        {"female",
         {{"type", "boolean"},
          {"description", "The talk tables a female player's character is spoken to with (_f). Default false."}}}};
    stringsProperties.update(PageProperties(50, 5000));
    tools.Add(Spec("strings",
                   "Turns string ids (strrefs, as templates, tables and conversations hold them) into the game's "
                   "text, and text into string ids, across every talk table of a language: the core's, the "
                   "campaign's and each add-in's. With neither, lists the talk tables read.",
                   std::move(stringsProperties)),
              [&ghilana](const Json &args) -> ToolResult { return Strings(ghilana, args); });

    Json queryProperties = {
        {"type",
         {{"type", "string"},
          {"description", "The files' extension: utc (creatures), uti (items), utp, plo, dlg, ..."}}},
        {"where",
         {{"type", "object"},
          {"description", "Fields and the values they must have: {\"Appearance_Type\": 80}. A bare name is "
                          "found at the top or in the nearest structs below that have it, a list's elements among "
                          "them; a dotted path names one (\"Appearance Data.Appearance_Type\"), and through a list "
                          "(\"StageCameraList.StageCameraLookingAtType\") each element. A field matches where any "
                          "of its values does, and a match says which elements in `matched`. A localised name "
                          "matches its string id; an array matches any of its values. Default: every file."}}},
        {"fields",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"description", "Fields to answer each match with; one through a list answers every element's value."}}},
        {"group_by",
         {{"type", "string"},
          {"description", "A field: count the matches by its value; through a list, each element's value."}}},
        {"source", {{"type", "string"}, {"description", "Only copies in this source."}}},
        {"copies",
         {{"type", "string"},
          {"enum", Json::array({"game", "all"})},
          {"description", "game (default): of each resource the copy the game takes; all: every copy, the ones it "
                          "shadows and does not have too."}}},
        {"depth",
         {{"type", "integer"}, {"minimum", 1}, {"description", "How deep nested fields are read. Default 6."}}},
        {"items",
         {{"type", "integer"}, {"minimum", 1}, {"description", "The most elements of a list read. Default 100."}}}};
    queryProperties.update(PageProperties(50, 5000));
    tools.Add(Spec("query",
                   "Searches every resource of a type across the install by its fields: every creature template "
                   "with Appearance_Type 80, every item of a base type. GFF3 templates (.utc, .uti, .utp, .utt, "
                   ".are) and GFF4 files (.plo, .dlg, ...) are read: of each resource the copy the game takes, or "
                   "every copy with `copies` all. Answers the matches with the fields asked and the copy each is "
                   "in, a count of all, and with `group_by` the counts by a field's value. Copies in enciphered "
                   "add-in archives are counted, not searched. `resource` on a match shows its every field.",
                   std::move(queryProperties), {"type"}),
              [&ghilana](const Json &args) -> ToolResult { return Query(ghilana, args); });

    Json referencesProperties = {
        {"name", {{"type", "string"}, {"description", "The resource, with its extension: \"ogre_default.utc\"."}}},
        {"used_by_types",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"description", "Search every file of these types for what names this resource: [\"utc\", \"dlg\"]. "
                          "Default: none, which only answers what it names."}}},
        {"source", {{"type", "string"}, {"description", "Which copy, by its source."}}},
        {"archive", {{"type", "string"}, {"description", "Which copy, by its archive's path."}}},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"description", "The most of each list. Default 100."}}}};
    tools.Add(Spec("references",
                   "What a resource names and what names it. `uses`: every field of the resource (a template, a "
                   "conversation, a plot, a data table's cells) whose text is another resource's name, with or "
                   "without its extension, and the string ids it holds. `used_by`: with `used_by_types`, every "
                   "file of those types with a field naming this resource. Follows a creature to its "
                   "conversation and scripts, a table row to its models.",
                   std::move(referencesProperties), {"name"}),
              [&ghilana](const Json &args) -> ToolResult { return References(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
