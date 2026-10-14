// Ghilana's tools over the files as files: what is installed and where
// (environment), what resources there are and their copies (resources,
// conflicts), one read (resource, texture) or written out (extract).
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "core/Base64.h"
#include "ghilana/FileJson.h"
#include "ghilana/Png.h"
#include "ghilana/Text.h"

#include <dao/core/Dds.h>
#include <dao/core/Fingerprint.h>
#include <dao/core/GdaFile.h>
#include <dao/core/Gff3.h>
#include <dao/core/Gff4.h>
#include <dao/core/TalkTable.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

namespace eluvian::ghilana::detail
{

namespace
{

ToolSpec Spec(std::string name, std::string description, Json properties, bool readOnly = true)
{
    return ToolSpec{std::move(name), std::move(description),
                    Json{{"type", "object"}, {"properties", std::move(properties)}}, Thread::Listener, readOnly};
}

void Put(Json &out, const char *key, const Json &value)
{
    if (!value.is_null())
    {
        out[key] = value;
    }
}

const Json kCopyProperties = {
    {"source",
     {{"type", "string"},
      {"description", "Which copy, by its source as answers name it (\"core\", \"addin:dao_prc_cp_1\", "
                      "\"plugin:mymod\"). Default: the copy the game takes."}}},
    {"archive",
     {{"type", "string"},
      {"description", "Which copy, by its archive's path (or a loose file's) as answers give it."}}},
};

// ---------------------------------------------------------------- environment

Json AddInJson(const AddIn &item)
{
    Json out = {{"uid", item.uid}};
    Put(out, "title", item.title.empty() ? Json(nullptr) : Json(item.title));
    Put(out, "name", item.name.empty() ? Json(nullptr) : Json(item.name));
    Put(out, "extends", item.extends.empty() ? Json(nullptr) : Json(item.extends));
    Put(out, "priority", item.priority ? Json(*item.priority) : Json(nullptr));
    Put(out, "enabled", item.enabled ? Json(*item.enabled) : Json(nullptr));
    return out;
}

Json Fingerprint(const fs::path &game)
{
    std::ifstream exe(game / L"bin_ship" / L"DAOrigins.exe", std::ios::binary);
    std::vector<std::byte> head(4096);
    exe.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(std::max<std::streamsize>(exe.gcount(), 0)));
    const auto print = dao::core::ReadFingerprint(head);
    if (!print)
    {
        return nullptr;
    }
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "0x%08x", print->timeDateStamp);
    char entry[16];
    std::snprintf(entry, sizeof entry, "0x%08x", print->entryPointRva);
    Json out = {{"link_stamp", stamp}, {"entry_rva", entry}};
    // DAOLib's dev/BUILDS.md: GOG's and Steam's share the link stamp, Steam's
    // entry is SteamStub's; EA's has its own stamp.
    if (print->timeDateStamp == 0x4eb19d53)
    {
        out["build"] = "1.05.13263.0, GOG's code (GOG's, or Steam's wrapped)";
    }
    else if (print->timeDateStamp == 0x52164c37)
    {
        out["build"] = "1.05.13263.0, EA's";
    }
    return out;
}

// DAOLib's loader and its plugins, if installed.
Json DaolibJson(const fs::path &game)
{
    std::error_code error;
    const fs::path bin = game / L"bin_ship";
    Json loader = nullptr;
    for (const wchar_t *name : {L"dinput8.dll", L"version.dll"})
    {
        if (loader.is_null() && fs::exists(bin / name, error))
        {
            loader = Utf8(name);
        }
    }
    Json plugins = Json::array();
    for (fs::directory_iterator it(bin / L"daolib" / L"plugins", error), end; !error && it != end; it.increment(error))
    {
        if (it->is_directory(error))
        {
            plugins.push_back(Utf8(it->path().filename().wstring()));
        }
    }
    return Json{{"loader", loader}, {"plugins", std::move(plugins)}};
}

Json UnreadableJson(const GameFiles &files)
{
    std::size_t enciphered = 0;
    std::set<std::string> sources;
    for (const auto &archive : files.Archives())
    {
        if (archive.header.Encryption() != 0)
        {
            ++enciphered;
            sources.insert(files.Sources()[archive.source].id);
        }
    }
    return Json{{"enciphered_archives", enciphered},
                {"sources", sources},
                {"why",
                 "The add-ins' ERF V2.2 archives are enciphered under a password the game holds and ghilana cannot "
                 "read; their resources are listed by name, and their bytes are only had through the running game "
                 "(Eluvian)."}};
}

Json SourceJson(const Source &source)
{
    Json s = {{"id", source.id}, {"kind", source.kind}, {"folder", source.folder}};
    if (source.base == Base::Documents)
    {
        s["in"] = "documents";
    }
    s["archives"] = source.archives;
    s["loose"] = source.loose;
    s["files"] = source.files;
    if (source.unreadable > 0)
    {
        s["unreadable"] = source.unreadable;
    }
    if (source.settings)
    {
        s["settings"] = AddInJson(*source.settings);
    }
    else if (source.kind == "addin" || source.kind == "offer")
    {
        s["settings"] = "none: AddIns.xml (offers.xml) does not list it, so the game does not load it";
    }
    return s;
}

// The add-ins and offers the settings list with no folder of theirs.
Json ListedWithoutFolder(const GameFiles &files)
{
    Json missing = Json::array();
    for (const auto &[kind, list] :
         {std::pair{"addin:", &files.Installed().AddIns()}, std::pair{"offer:", &files.Installed().Offers()}})
    {
        for (const auto &item : *list)
        {
            if (!files.SourceOf(kind + Lower(item.uid)))
            {
                missing.push_back(AddInJson(item));
            }
        }
    }
    return missing;
}

Json Environment(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "environment");
    const bool detail = args.Bool("sources", true);
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const Paths &where = files.Where();
    Json out = {{"server", {{"name", "ghilana"}, {"version", ghilana.Version()}}}};
    Put(out, "index", indexed);
    out["game"] = {{"folder", PathText(where.game)}, {"exe", Fingerprint(where.game)}};
    if (where.game.empty())
    {
        out["game"]["note"] = "no install found on this machine; start ghilana with --game <the folder with bin_ship>";
    }
    Json installs = Json::array();
    for (const auto &install : ghilana.Installs())
    {
        std::error_code error;
        installs.push_back(Json{{"store", install.store},
                                {"folder", PathText(install.folder)},
                                {"served", fs::equivalent(install.folder, where.game, error)}});
    }
    out["installs"] = std::move(installs);
    out["documents"] = PathText(where.documents);
    Put(out, "instance", where.instance.empty() ? Json(nullptr) : Json(where.instance));
    out["language"] = where.language;
    out["cache"] = PathText(ghilana.Cache());
    out["daolib"] = DaolibJson(where.game);
    out["unreadable"] = UnreadableJson(files);
    if (detail)
    {
        Json sources = Json::array();
        for (const auto &source : files.Sources())
        {
            sources.push_back(SourceJson(source));
        }
        out["sources"] = std::move(sources);
    }
    if (Json missing = ListedWithoutFolder(files); !missing.empty())
    {
        out["listed_without_folder"] = std::move(missing);
    }
    const auto &set = files.Installed().Set();
    // The packages folders the campaign's .cif loads ahead of the core
    // (Awakening's core_ep1), where the game reads them (DAOLib #113, #129).
    out["game_state"] = {{"campaign", set.campaign.empty() ? Json(nullptr) : Json(set.campaign)},
                         {"packages", files.Installed().Packages()},
                         {"language", set.language},
                         {"textures", set.textures},
                         {"note", "what ghilana takes the game to be doing, which decides some of the copies it has "
                                  "(--campaign, --language, --textures)"}};
    out["precedence"] = "the game's: of a resource's copies it uses the one with the lowest priority, as the game "
                        "does. Every answer lists the copies in that order and says which one the game uses.";
    std::size_t unsettled = 0;
    for (const auto &[name, copies] : files.Resources())
    {
        const Copy *taken = GameFiles::Taken(copies);
        unsettled += taken != nullptr && !taken->settled && copies.size() > 1 ? 1 : 0;
    }
    if (unsettled > 0)
    {
        out["unsettled"] = {{"resources", unsettled},
                            {"why", "AddIns.xml has an add-in disabled or with no folder; whether it takes a place "
                                    "in the game's count of add-ins is not known, and which copy the game takes of "
                                    "these resources rests on it"}};
    }
    return out;
}

// ---------------------------------------------------------------- resources

std::vector<std::string> Words(const std::string &text)
{
    std::vector<std::string> words;
    std::size_t at = 0;
    const std::string lower = Lower(text);
    while (at < lower.size())
    {
        const auto end = std::min(lower.find(' ', at), lower.size());
        if (end > at)
        {
            words.push_back(lower.substr(at, end - at));
        }
        at = end + 1;
    }
    return words;
}

Json Resources(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "resources");
    const std::string match = args.String("match", "");
    const std::string type = Lower(args.String("type", ""));
    const std::string sourceId = Lower(args.String("source", ""));
    const auto limit = static_cast<std::size_t>(args.Integer("limit", 100, 1, 5000));
    const auto offset = static_cast<std::size_t>(args.Integer("offset", 0, 0, INT64_MAX));
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    std::optional<std::uint32_t> source;
    if (!sourceId.empty())
    {
        source = files.SourceOf(sourceId);
        if (!source)
        {
            throw ToolError("resources: no source \"" + sourceId + "\"; `environment` lists them");
        }
    }
    const bool pattern = IsPattern(match);
    const auto words = pattern ? std::vector<std::string>{} : Words(match);
    Json found = Json::array();
    std::size_t count = 0;
    for (const auto &[name, copies] : files.Resources())
    {
        if (!type.empty() && Extension(name) != type)
        {
            continue;
        }
        if (pattern ? !Glob(match, name) : !std::ranges::all_of(words, [&name](const std::string &w) {
                return name.find(w) != std::string::npos;
            }))
        {
            continue;
        }
        if (source && std::ranges::none_of(copies, [&](const Copy &c) { return c.source == *source; }))
        {
            continue;
        }
        if (count++ < offset || found.size() >= limit)
        {
            continue;
        }
        Json where = Json::array();
        for (const Copy &copy : copies)
        {
            where.push_back(files.Describe(copy));
        }
        found.push_back(Json{{"name", name}, {"copies", std::move(where)}});
    }
    Json out = Json::object();
    Put(out, "index", indexed);
    out["count"] = count;
    out["offset"] = offset;
    out["resources"] = std::move(found);
    if (count > offset + limit)
    {
        out["more"] = "page on with offset " + std::to_string(offset + limit);
    }
    return out;
}

// ---------------------------------------------------------------- resource

std::string Hex(std::span<const std::byte> bytes, std::size_t most)
{
    std::string out;
    char two[4];
    for (std::size_t i = 0; i < bytes.size() && i < most; ++i)
    {
        std::snprintf(two, sizeof two, "%02x", static_cast<unsigned>(bytes[i]));
        out += two;
        if (i + 1 < most && i + 1 < bytes.size())
        {
            out += ' ';
        }
    }
    return out;
}

bool Starts(std::span<const std::byte> bytes, std::string_view text, std::size_t at = 0)
{
    return bytes.size() >= at + text.size() && std::memcmp(bytes.data() + at, text.data(), text.size()) == 0;
}

// Text, if the bytes are text: UTF-16 with its mark, UTF-8 (with or without
// its mark), or 8-bit text with no control characters but blanks.
std::optional<std::string> AsText(std::span<const std::byte> bytes)
{
    if (bytes.size() >= 2 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xFE})
    {
        std::wstring wide;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
        {
            wide += static_cast<wchar_t>(static_cast<unsigned>(bytes[i]) | (static_cast<unsigned>(bytes[i + 1]) << 8));
        }
        return Utf8(wide);
    }
    std::size_t start = Starts(bytes, "\xEF\xBB\xBF") ? 3 : 0;
    const std::string_view view(reinterpret_cast<const char *>(bytes.data()) + start, bytes.size() - start);
    for (const char c : view.substr(0, 4096))
    {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 && c != '\n' && c != '\r' && c != '\t')
        {
            return std::nullopt;
        }
    }
    // UTF-8 kept as it is where it is UTF-8, else read as Latin-1.
    const std::wstring wide = Wide(view);
    return Utf8(wide);
}

Json GdaContent(const dao::core::GdaFile &table, std::size_t items)
{
    Json columns = Json::array();
    Json rows = Json::array();
    for (const auto &column : table.Columns())
    {
        columns.push_back(Json{{"name", ColumnName(column.hash)}, {"type", ColumnType(column)}});
    }
    for (std::size_t r = 0; r < table.Rows().size() && r < items; ++r)
    {
        Json row = Json::object();
        for (std::size_t c = 0; c < table.Columns().size(); ++c)
        {
            row[ColumnName(table.Columns()[c].hash)] = CellJson(table.Rows()[r][c]);
        }
        rows.push_back(std::move(row));
    }
    return Json{{"columns", std::move(columns)},
                {"row_count", table.Rows().size()},
                {"rows", std::move(rows)},
                {"note", "`table` reads it merged with the other files of its table, by id or value"}};
}

Json TlkContent(std::span<const std::byte> bytes, std::size_t items)
{
    const auto table = dao::core::TalkTableFile::Parse(bytes);
    if (!table)
    {
        return Json{{"_unread", "a talk table of a version other than 0.2"}};
    }
    Json strings = Json::array();
    for (const auto &[id, text] : table->Strings())
    {
        if (strings.size() >= items)
        {
            break;
        }
        strings.push_back(Json{{"id", id}, {"text", Utf8(text)}});
    }
    return Json{{"count", table->Size()},
                {"strings", std::move(strings)},
                {"note", "`strings` looks ids and text up across every talk table of a language"}};
}

Json Gff4Content(std::span<const std::byte> bytes, const Limits &limits, std::string &format)
{
    const auto file = dao::core::gff4::File::Parse(bytes);
    if (!file)
    {
        format = "gff4";
        return Json{{"_unread", "a GFF4 header DAOLib's reader refuses (shared strings, or not the PC's)"}};
    }
    if (file->Type() == "G2DA")
    {
        if (const auto table = dao::core::GdaFile::Parse(bytes))
        {
            format = "gda";
            return GdaContent(*table, limits.items);
        }
    }
    if (file->Type() == "TLK") // the type, its trailing blank dropped
    {
        format = "tlk";
        return TlkContent(bytes, limits.items);
    }
    format = "gff4:" + Lower(std::string(file->Type()));
    return Gff4ToJson(*file, limits);
}

// What a DDS file holds, as DAOLib's core reads it without decoding
// (dds::Describe, DAOLib #111): its format, size and pictures, counted from
// the bytes, since the header's count is wrong in the game's own files
// (beckmann.dds holds 10 where its header says 1). Asked by the shaders'
// session for a texture's format and mips (#130).
Json DdsJson(const dao::core::dds::Description &dds)
{
    static constexpr std::array<std::string_view, 3> kShapes{"flat", "cube", "volume"};
    Json answer = {{"format", dds.format},
                   {"width", dds.width},
                   {"height", dds.height},
                   {"mips", dds.pictures},
                   {"shape", kShapes.at(static_cast<std::size_t>(dds.shape))}};
    if (dds.headerPictures != dds.pictures)
    {
        answer["header_mips"] = dds.headerPictures;
    }
    if (!dds.decodes)
    {
        answer["decodes"] = false;
    }
    return answer;
}

Json DdsContent(std::span<const std::byte> bytes)
{
    const auto dds = dao::core::dds::Describe(bytes);
    if (!dds)
    {
        return Json{{"note", "a DDS file whose header gives no size"}};
    }
    Json answer = DdsJson(*dds);
    answer["note"] = "`texture` gives the picture as an image";
    return answer;
}

Json TextContent(const std::string &text, std::size_t chars)
{
    if (text.size() > chars)
    {
        return Json{{"text", text.substr(0, chars)},
                    {"cut", "the first " + std::to_string(chars) + " of " + std::to_string(text.size()) +
                                " bytes; `chars` asks for more"}};
    }
    return Json{{"text", text}};
}

Json ReadContent(std::span<const std::byte> bytes, const Args &args, std::string &format)
{
    const int depth = static_cast<int>(args.Integer("depth", 12, 1, 64));
    const auto items = static_cast<std::size_t>(args.Integer("items", 200, 1, 100000));
    const Limits limits{depth, items};
    if (Starts(bytes, "GFF V4.0") || Starts(bytes, "GFF V4.1"))
    {
        return Gff4Content(bytes, limits, format);
    }
    if (bytes.size() >= 8 && (Starts(bytes, "V3.2", 4) || Starts(bytes, "V3.3", 4)))
    {
        if (const auto file = dao::core::gff3::File::Parse(bytes))
        {
            format = "gff3:" + Lower(file->FileType());
            return Gff3ToJson(*file, limits);
        }
    }
    if (Starts(bytes, "DDS "))
    {
        format = "dds";
        return DdsContent(bytes);
    }
    if (const auto text = AsText(bytes))
    {
        format = "text";
        return TextContent(*text, static_cast<std::size_t>(args.Integer("chars", 20000, 100, 2000000)));
    }
    format = "binary";
    return Json{{"head", Hex(bytes, 64)}, {"note", "a format ghilana does not read; `extract` writes it out"}};
}

Json Resource(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "resource");
    const std::string name = args.String("name");
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select("resource", name, args.String("source", ""), args.String("archive", ""));
    const auto &all = *files.Find(name);
    const Copy &copy = GameFiles::Choose(copies);
    Json out = Chosen(files, Lower(name), all, copy);
    Put(out, "index", indexed);
    std::string why;
    const auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        const bool another = std::ranges::any_of(all, [&files](const Copy &c) { return files.Readable(c); });
        throw ToolError(
            "resource: \"" + name + "\" cannot be read: " + why +
            (another ? ". Another of its copies can be read: name it with `source`; `conflicts` lists them" : ""));
    }
    std::string format;
    Json content = ReadContent(*bytes, args, format);
    out["format"] = format;
    out["content"] = std::move(content);
    return out;
}

// ---------------------------------------------------------------- conflicts

// A resource's copies as conflicts answers them: the copy the game takes,
// and every copy in the game's order, those the game does not have only
// with `unused`.
Json ConflictJson(const GameFiles &files, const std::string &name, const std::vector<Copy> &copies, bool unused)
{
    const Copy *taken = GameFiles::Taken(copies);
    Json out = {{"name", name}, {"taken", taken != nullptr ? files.Describe(*taken) : Json(nullptr)}};
    if (taken == nullptr)
    {
        out["why"] = "the game has none of its copies: " + files.WhyUnused(copies.front());
    }
    else if (!taken->settled)
    {
        out["order"] = "unsettled: an add-in disabled or with no folder may or may not take a place in the game's "
                       "count, and which copy it takes rests on that";
    }
    Json list = Json::array();
    for (const Copy &copy : copies)
    {
        if (unused || copy.unused == dao::core::install::Unused::No)
        {
            list.push_back(files.Describe(copy));
        }
    }
    out["copies"] = std::move(list);
    return out;
}

// Whether a resource is one conflicts lists: copies the game has (or any
// copies, with `unused`) in more than one source, or more than one in all
// with `across` false; with `source`, one of them in that source.
bool IsConflict(const std::vector<Copy> &copies, bool unused, bool across, std::optional<std::uint32_t> source)
{
    std::set<std::uint32_t> sources;
    std::size_t count = 0;
    for (const Copy &copy : copies)
    {
        if (unused || copy.unused == dao::core::install::Unused::No)
        {
            sources.insert(copy.source);
            ++count;
        }
    }
    if (count < 2 || (across && sources.size() < 2))
    {
        return false;
    }
    return !source || sources.contains(*source);
}

Json Conflicts(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "conflicts");
    const std::string name = args.String("name", "");
    const bool unused = args.Bool("unused", false);
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    Json out = Json::object();
    Put(out, "index", indexed);
    if (!name.empty())
    {
        // Every copy, the game's or not; Select's refusal says what there is, for no such resource.
        out.update(ConflictJson(files, Lower(name), files.Select("conflicts", name, "", ""), true));
        return out;
    }
    const std::string type = Lower(args.String("type", ""));
    const auto source = SourceArg(files, args, "conflicts");
    const bool across = args.Bool("across_sources", true);
    const Page page(args, 100, 5000);
    Json found = Json::array();
    std::size_t count = 0;
    std::size_t lost = 0; // of them, the source asked for loses
    for (const auto &[resource, copies] : files.Resources())
    {
        if ((!type.empty() && Extension(resource) != type) || !IsConflict(copies, unused, across, source))
        {
            continue;
        }
        const Copy *taken = GameFiles::Taken(copies);
        lost += source && (taken == nullptr || taken->source != *source) ? 1 : 0;
        if (page.Takes(count++, found.size()))
        {
            found.push_back(ConflictJson(files, resource, copies, unused));
        }
    }
    page.Answer(out, count, "conflicts", std::move(found));
    if (source)
    {
        out["lost"] = lost;
        out["lost_means"] = "of these, how many the source asked for does not win: another source's copy is the one "
                            "the game takes";
    }
    return out;
}

// ---------------------------------------------------------------- extract

std::string SafeFolderName(std::string text)
{
    for (char &c : text)
    {
        if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }
    return text;
}

bool Inside(const fs::path &folder, const fs::path &root)
{
    if (root.empty())
    {
        return false;
    }
    std::error_code error;
    const std::string f = Lower(PathText(fs::weakly_canonical(folder, error))) + "/";
    const std::string r = Lower(PathText(fs::weakly_canonical(root, error))) + "/";
    return f.starts_with(r);
}

// The names a call asked for: `names`, and those `pattern` matches.
std::vector<std::string> NamesToExtract(const Args &args, const GameFiles &files)
{
    std::vector<std::string> names;
    if (const Json &list = args.Raw().value("names", Json()); list.is_array())
    {
        for (const auto &name : list)
        {
            if (name.is_string())
            {
                names.push_back(name.get<std::string>());
            }
        }
    }
    const std::string pattern = args.String("pattern", "");
    if (names.empty() && pattern.empty())
    {
        throw ToolError("extract: give `names` (an array of resource names) or `pattern` (\"abi_*.gda\")");
    }
    const auto most = static_cast<std::size_t>(args.Integer("most", 1000, 1, 100000));
    for (const auto &[name, copies] : files.Resources())
    {
        if (pattern.empty() || !Glob(pattern, name))
        {
            continue;
        }
        if (names.size() >= most)
        {
            throw ToolError("extract: `pattern` matches more than `most` (" + std::to_string(most) +
                            ") resources; narrow it, or raise `most`");
        }
        names.push_back(name);
    }
    return names;
}

// Where all_copies writes a copy: a folder named for its source, then its
// archive's path (or its own file's folder), since one source may hold a
// resource twice (the core's high and medium textures).
fs::path CopyFolder(const GameFiles &files, const Copy &copy, const fs::path &to)
{
    const std::string &where = files.Installed().Where(copy);
    const fs::path relative(Wide(where));
    return to / fs::path(Wide(SafeFolderName(files.Sources()[copy.source].id))) /
           (copy.archive == kLoose ? relative.parent_path() : relative);
}

// What extract says of the one copy it wrote of several.
std::string ChosenNote(const std::vector<Copy> &copies)
{
    if (GameFiles::Taken(copies) == nullptr)
    {
        return "the first of copies the game has none of; `all_copies` writes each";
    }
    return "the copy the game takes, of " + std::to_string(copies.size()) + "; `all_copies` writes each";
}

// One copy written into `folder`; what to say of it, written or not.
Json WriteCopy(const GameFiles &files, const std::string &name, const Copy &copy, const fs::path &folder, bool &written)
{
    written = false;
    std::string why;
    const auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        return Json{{"name", name}, {"copy", files.Describe(copy)}, {"why", why}};
    }
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path path = folder / fs::path(Wide(Lower(name)));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!out)
    {
        return Json{{"name", name}, {"why", "could not write " + PathText(path)}};
    }
    written = true;
    return Json{{"name", Lower(name)}, {"path", PathText(path)}, {"copy", files.Describe(copy)}};
}

Json Extract(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "extract");
    const fs::path to = fs::path(Wide(args.String("to")));
    if (!to.is_absolute())
    {
        throw ToolError(R"(extract: `to` is a folder's full path ("C:\\work\\extracted"))");
    }
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    if (Inside(to, files.Where().game))
    {
        throw ToolError("extract: `to` is inside the game's folder, which ghilana never writes into; name a folder "
                        "of your own");
    }
    const bool all = args.Bool("all_copies", false);
    const auto names = NamesToExtract(args, files);
    std::error_code error;
    fs::create_directories(to, error);
    if (error)
    {
        throw ToolError("extract: cannot make " + PathText(to) + ": " + error.message());
    }
    Json written = Json::array();
    Json skipped = Json::array();
    for (const auto &name : names)
    {
        const auto *copies = files.Find(name);
        if (copies == nullptr)
        {
            skipped.push_back(Json{{"name", name}, {"why", "no such resource"}});
            continue;
        }
        for (const Copy &copy : all ? *copies : std::vector<Copy>{GameFiles::Choose(*copies)})
        {
            const fs::path folder = all ? CopyFolder(files, copy, to) : to;
            bool done = false;
            Json item = WriteCopy(files, name, copy, folder, done);
            if (done && !all && copies->size() > 1)
            {
                item["chosen"] = ChosenNote(*copies);
            }
            (done ? written : skipped).push_back(std::move(item));
        }
    }
    Json out = Json::object();
    Put(out, "index", indexed);
    out["written"] = std::move(written);
    if (!skipped.empty())
    {
        out["skipped"] = std::move(skipped);
    }
    return out;
}

// ---------------------------------------------------------------- texture

ToolResult Texture(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "texture");
    const std::string name = args.String("name");
    const auto size = static_cast<std::uint32_t>(args.Integer("size", 512, 1, 4096));
    const std::string channel = args.Choice("channel", {"rgba", "rgb", "alpha", "red", "green", "blue"}, "rgba");
    Json indexed = ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select("texture", name, args.String("source", ""), args.String("archive", ""));
    const Copy &copy = GameFiles::Choose(copies);
    std::string why;
    const auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        throw ToolError("texture: \"" + name + "\" cannot be read: " + why);
    }
    const auto image = dao::core::dds::Decode(*bytes, size);
    if (!image)
    {
        throw ToolError("texture: \"" + name +
                        "\" is no DDS picture DAOLib reads (DXT1, DXT3, DXT5, or 24 or 32 bits a pixel); "
                        "`resource` says what it is");
    }
    const std::size_t pixels = std::size_t{image->width} * image->height;
    int channels = 4;
    std::vector<std::uint8_t> out;
    if (channel == "rgba")
    {
        out = image->rgba;
    }
    else if (channel == "rgb")
    {
        channels = 3;
        out.reserve(pixels * 3);
        for (std::size_t i = 0; i < pixels; ++i)
        {
            out.insert(out.end(), image->rgba.begin() + static_cast<std::ptrdiff_t>(i * 4),
                       image->rgba.begin() + static_cast<std::ptrdiff_t>(i * 4) + 3);
        }
    }
    else
    {
        channels = 1;
        static constexpr std::array<std::string_view, 4> kChannels{"red", "green", "blue", "alpha"};
        const auto offset = static_cast<std::size_t>(std::ranges::find(kChannels, channel) - kChannels.begin());
        out.reserve(pixels);
        for (std::size_t i = 0; i < pixels; ++i)
        {
            out.push_back(image->rgba[(i * 4) + offset]);
        }
    }
    const auto png = EncodePng(image->width, image->height, channels, out);
    Json data = Chosen(files, Lower(name), *files.Find(name), copy);
    Put(data, "index", indexed);
    if (const auto dds = dao::core::dds::Describe(*bytes))
    {
        data["file"] = DdsJson(*dds);
    }
    data["width"] = image->width;
    data["height"] = image->height;
    data["channel"] = channel;
    data["png_bytes"] = png.size();
    ToolResult result(std::move(data));
    result.content.push_back(
        Json{{"type", "image"},
             {"data", Base64(std::string_view(reinterpret_cast<const char *>(png.data()), png.size()))},
             {"mimeType", "image/png"}});
    return result;
}

} // namespace

void RegisterFileTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();
    tools.Add(Spec("environment",
                   "What ghilana is serving: the game install read (and every install found on the machine), the "
                   "exe's build, the Documents folder and instance, the language, DAOLib and its plugins if "
                   "installed, and each source of resources (the core, the campaign, every add-in and offer with "
                   "whether AddIns.xml enables it, the packages a campaign loads ahead of the core such as "
                   "Awakening's core_ep1, the override folders, DAOLib plugins' overrides) with how many "
                   "archives and files it holds and how many cannot be read. Call it first. The first call reads "
                   "every archive's directory (some ten thousand) and says how long that took; later calls are "
                   "fast.",
                   Json{{"sources",
                         {{"type", "boolean"},
                          {"description", "List every source. Default true; false for the summary alone."}}}}),
              [&ghilana](const Json &args) -> ToolResult { return Environment(ghilana, args); });

    Json resourcesProperties = {
        {"match",
         {{"type", "string"},
          {"description", "Words that must all be in the name, in any case (\"ogre default\"), or a pattern with * "
                          "and ? (\"abi_*.gda\"). Default: every name."}}},
        {"type", {{"type", "string"}, {"description", "An extension: utc, uti, gda, dlg, dds, tlk... Default: all."}}},
        {"source",
         {{"type", "string"},
          {"description", "Only resources with a copy in this source (\"addin:dao_prc_cp_2\"); `environment` lists "
                          "them."}}}};
    resourcesProperties.update(PageProperties(100, 5000));
    tools.Add(Spec("resources",
                   "Finds resources by name across every source of the install: core, campaign, add-ins, offers, "
                   "override folders and DAOLib plugins' overrides. Answers each name with every copy of it (its "
                   "source, archive or file, size, and whether it can be read), sorted by name, with the count of "
                   "all matches. Use it to find a name to pass to `resource`, `texture`, `table` or Eluvian's tools "
                   "(a creature template for stage_spawn).",
                   std::move(resourcesProperties)),
              [&ghilana](const Json &args) -> ToolResult { return Resources(ghilana, args); });

    Json resourceProperties = {
        {"name", {{"type", "string"}, {"description", "The resource, with its extension: \"ogre_default.utc\"."}}},
        {"depth",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", 64},
          {"description", "How deep to follow nested structs and lists. Default 12."}}},
        {"items",
         {{"type", "integer"},
          {"minimum", 1},
          {"description", "The most elements of any one list (and rows of a table). Default 200."}}},
        {"chars",
         {{"type", "integer"}, {"minimum", 100}, {"description", "The most bytes of a text file. Default 20000."}}}};
    resourceProperties.update(kCopyProperties);
    {
        ToolSpec spec = Spec("resource", "", std::move(resourceProperties));
        spec.inputSchema["required"] = Json::array({"name"});
        spec.description =
            "Reads one resource and answers what it holds: a GFF4 file (conversation, plot, model, ...) or a GFF3 "
            "template (.utc, .uti, .utp) as JSON with its fields named, a data table as columns and rows, a talk "
            "table's strings, a texture's size and format, a text file (.mao, .xml, .nss) as text, anything else as "
            "its first bytes. Of several copies it reads the one the game takes, and says so with the others; "
            "`source` and `archive` name another. A copy in an enciphered add-in archive cannot be read here; the "
            "answer says so.";
        tools.Add(std::move(spec), [&ghilana](const Json &args) -> ToolResult { return Resource(ghilana, args); });
    }

    Json conflictsProperties = {
        {"name",
         {{"type", "string"},
          {"description", "One resource: every copy of it. Default: every resource with more than one copy."}}},
        {"source",
         {{"type", "string"},
          {"description", "Only resources one of whose copies is in this source: one add-in's or mod's conflicts."}}},
        {"type", {{"type", "string"}, {"description", "Only resources of this extension."}}},
        {"across_sources",
         {{"type", "boolean"},
          {"description", "Only resources whose copies are in more than one source. Default true; false adds a "
                          "source's own repeats (a core file and its patch)."}}},
        {"unused",
         {{"type", "boolean"},
          {"description", "Count and list copies the game does not have too (another campaign's, language's or "
                          "texture quality's, a disabled add-in's). Default false."}}}};
    conflictsProperties.update(PageProperties(100, 5000));
    tools.Add(Spec("conflicts",
                   "Says which copy of a resource the game takes, and every copy of it in the order the game looks "
                   "for it (with each copy's priority, and why the game does not have one where it does not), for "
                   "one resource (`name`) or every resource more than one source has. The order is the game's, read "
                   "in the exe, for the campaign, language and texture quality ghilana was started with. Use it to "
                   "see what an add-in or a mod overrides and what overrides it (`source`: `lost` counts those it "
                   "does not win), or where a resource comes from.",
                   std::move(conflictsProperties)),
              [&ghilana](const Json &args) -> ToolResult { return Conflicts(ghilana, args); });

    Json extractProperties = {
        {"names",
         {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Resource names, with extensions."}}},
        {"pattern", {{"type", "string"}, {"description", "A name pattern with * and ?: \"abi_*.gda\"."}}},
        {"to",
         {{"type", "string"},
          {"description", "The folder to write into, a full path; made if missing. Never inside the game's folder."}}},
        {"all_copies",
         {{"type", "boolean"},
          {"description", "Write every copy, each into a folder named for its source and then its archive's "
                          "path. Default false: the copy the game takes."}}},
        {"most",
         {{"type", "integer"},
          {"minimum", 1},
          {"description", "The most resources a pattern may match. Default 1000."}}}};
    {
        ToolSpec spec = Spec("extract",
                             "Writes resources out of the game's archives into a folder of yours, by name or "
                             "pattern, for a tool that wants the files themselves. Answers each file written with "
                             "its path and the copy it came from, and what could not be (an enciphered add-in's "
                             "copy). Writes nothing in the game's folders.",
                             std::move(extractProperties), false);
        spec.inputSchema["required"] = Json::array({"to"});
        tools.Add(std::move(spec), [&ghilana](const Json &args) -> ToolResult { return Extract(ghilana, args); });
    }

    Json textureProperties = {
        {"name", {{"type", "string"}, {"description", "The texture, with its extension: \"tb_gold_selparty.dds\"."}}},
        {"size",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", 4096},
          {"description", "The most pixels a side: the largest of the texture's pictures (mip levels) no larger. "
                          "Default 512."}}},
        {"channel",
         {{"type", "string"},
          {"enum", Json::array({"rgba", "rgb", "alpha", "red", "green", "blue"})},
          {"description", "Which channels: rgba (default), rgb without alpha, or one of them as grey (alpha as "
                          "grey shows a mask)."}}}};
    textureProperties.update(kCopyProperties);
    {
        ToolSpec spec = Spec("texture",
                             "Shows a DDS texture as a PNG image you can look at, with its size and the copy it "
                             "came from: one picture of its mip chain no larger than `size`, all its channels or "
                             "one as grey; `width` and `height` are the picture shown. `file` is what the file "
                             "holds: its `format` (DXT1, DXT3, DXT5, B8G8R8 or B8G8R8A8, the ones read), its "
                             "largest picture's size, `mips` (counted from the bytes; `header_mips` where the "
                             "header says otherwise, as the game's own files often do) and `shape` (flat, cube "
                             "or volume).",
                             std::move(textureProperties));
        spec.inputSchema["required"] = Json::array({"name"});
        tools.Add(std::move(spec), [&ghilana](const Json &args) { return Texture(ghilana, args); });
    }
}

} // namespace eluvian::ghilana::detail
