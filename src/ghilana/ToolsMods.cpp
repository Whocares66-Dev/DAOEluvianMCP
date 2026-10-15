// Ghilana's tool for a mod's files made one (#140): packed as the game's own
// archive (.erf), for a packages\core\override folder, or as a .dazip, the
// add-in package the game's DAUpdater installs, through DAOLib's core
// (dao/core/Erf.h and DaZip.h, DAOLib #122). The files are the caller's,
// such as texture_import's and model_import's.
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/Text.h"

#include <dao/core/DaZip.h>
#include <dao/core/Erf.h>
#include <dao/core/TalkTable.h>

#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

namespace eluvian::ghilana::detail
{

namespace
{

namespace erf = dao::core::erf;
namespace dazip = dao::core::dazip;

struct Given
{
    std::string name; // the file's name, lower case
    std::vector<std::byte> bytes;
};

// The files `files` names, read whole: full paths, one name each.
std::vector<Given> ReadFiles(const Args &args)
{
    const Json &raw = args.Raw();
    const Json *files = raw.contains("files") ? &raw["files"] : nullptr;
    if (files == nullptr || !files->is_array() || files->empty())
    {
        throw ToolError("mod_pack: `files` is a list of the files' full paths");
    }
    std::vector<Given> out;
    std::set<std::string> names;
    for (const Json &file : *files)
    {
        if (!file.is_string())
        {
            throw ToolError("mod_pack: `files` is a list of the files' full paths");
        }
        const fs::path path = fs::path(Wide(file.get<std::string>()));
        if (!path.is_absolute())
        {
            throw ToolError("mod_pack: " + file.get<std::string>() + " is no full path");
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            throw ToolError("mod_pack: cannot open " + PathText(path));
        }
        const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        Given given{Lower(PathText(path.filename())), std::vector<std::byte>(chars.size())};
        std::memcpy(given.bytes.data(), chars.data(), chars.size());
        if (!names.insert(given.name).second)
        {
            throw ToolError("mod_pack: two files are named " + given.name +
                            "; the game finds a file by its name alone");
        }
        out.push_back(std::move(given));
    }
    return out;
}

Json PackErf(const Args &args, const fs::path &to, const std::vector<Given> &files)
{
    std::string name = Lower(args.String("name", "mod.erf"));
    if (Extension(name).empty())
    {
        name += ".erf";
    }
    std::vector<erf::Item> items;
    items.reserve(files.size());
    for (const Given &file : files)
    {
        items.push_back({Wide(file.name), file.bytes});
    }
    const auto bytes = erf::Write(items);
    if (bytes.empty())
    {
        throw ToolError("mod_pack: DAOLib wrote no archive: a name is longer than the 31 units an archive keeps, or "
                        "has a character it cannot");
    }
    const fs::path path = to / fs::path(Wide(name));
    WriteFile(path, bytes, "mod_pack");
    return {{"written", PathText(path)},
            {"into_the_game", "put it in Documents\\BioWare\\Dragon Age\\packages\\core\\override (a folder of its own "
                              "there is fine); the game reads every .erf in an override folder, its files ahead of the "
                              "game's archives. Take it out to undo"}};
}

Json PackDazip(const Args &args, const fs::path &to, const std::vector<Given> &files)
{
    dazip::AddIn addIn;
    addIn.uid = args.String("uid");
    for (const char c : addIn.uid)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_')
        {
            throw ToolError(
                "mod_pack: `uid` is letters, digits and _ only: the add-in's folder name and the game's key "
                "for it");
        }
    }
    addIn.name = args.String("title", addIn.uid);
    addIn.title = addIn.name;
    addIn.description = args.String("description", "");
    addIn.publisher = args.String("publisher", "");
    addIn.version = args.String("version", "1.0");
    addIn.priority = static_cast<int>(args.Integer("priority", 1000, 0, 1000000));
    // Each file in the add-in's core\override, where the game reads an
    // add-in's files; a talk table of its own names in core\data	alktables
    // (lib-agent, DAOLib #122: the add-in loader asks for
    // <uid>_c_<language>.tlk).
    std::vector<dazip::AddInFile> given;
    given.reserve(files.size());
    for (const Given &file : files)
    {
        const bool talk = Extension(file.name) == "tlk";
        given.push_back({(talk ? "core/data/talktables/" : "core/override/") + file.name, file.bytes});
    }
    const auto bytes = dazip::Package(addIn, given);
    if (bytes.empty())
    {
        throw ToolError("mod_pack: DAOLib wrote no package");
    }
    const fs::path path = to / fs::path(Wide(addIn.uid + ".dazip"));
    WriteFile(path, bytes, "mod_pack");
    return {{"written", PathText(path)},
            {"uid", addIn.uid},
            {"into_the_game",
             "install it with the game's bin_ship\\DAUpdater.exe (or open the .dazip with it): the add-in goes into "
             "Documents\\BioWare\\Dragon Age\\AddIns\\" +
                 addIn.uid +
                 " and Settings\\AddIns.xml, and the game's Downloadable Content screen lists it, to be enabled or "
                 "disabled there. To undo, remove its entry from AddIns.xml and its folder"}};
}

Json Pack(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "mod_pack");
    const std::string format = args.Choice("format", {"erf", "dazip"});
    const fs::path to = OutputFolder(ghilana, args, "mod_pack");
    const std::vector<Given> files = ReadFiles(args);
    Json out = format == "erf" ? PackErf(args, to, files) : PackDazip(args, to, files);
    Json names = Json::array();
    for (const Given &file : files)
    {
        names.push_back(file.name);
    }
    out["files"] = std::move(names);
    out["format"] = format;
    return out;
}

// ---------------------------------------------------------------- talk_table

// A talk table of a mod's own names: an add-in's
// core\data\talktables\<uid>_c_<language>.tlk, which the game's add-in
// loader asks for (lib-agent, DAOLib #122 and #123).
Json TalkTable(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "talk_table");
    const fs::path to = OutputFolder(ghilana, args, "talk_table");
    const std::string uid = args.String("uid");
    const std::string language = Lower(args.String("language", "en-us"));
    const Json &given = raw.contains("strings") ? raw["strings"] : Json();
    if (!given.is_object() || given.empty())
    {
        throw ToolError(R"(talk_table: `strings` is {"id": "text"}, an id a string reference)");
    }
    std::vector<std::pair<std::uint32_t, std::wstring>> strings;
    for (const auto &[id, text] : given.items())
    {
        std::uint32_t number = 0;
        const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), number);
        if (error != std::errc() || end != id.data() + id.size() || !text.is_string())
        {
            throw ToolError("talk_table: \"" + id + "\" is no string reference, or its text is no text");
        }
        strings.emplace_back(number, Wide(text.get<std::string>()));
    }
    const auto bytes = dao::core::TalkTableFile::Write(strings);
    if (bytes.empty())
    {
        throw ToolError("talk_table: DAOLib wrote no talk table");
    }
    const std::string name = uid + "_c_" + language + ".tlk";
    const fs::path path = to / fs::path(Wide(name));
    WriteFile(path, bytes, "talk_table");
    return {{"written", PathText(path)},
            {"strings", strings.size()},
            {"next", "pack it with mod_pack format dazip and the same uid (it goes to the add-in's "
                     "core/data/talktables); give a template's name the id with template_edit (LocName)"}};
}

} // namespace

void RegisterModTools(Bench &bench, Ghilana &ghilana)
{
    bench.Tools().Add(
        ToolSpec{"talk_table",
                 "Writes a talk table of a mod's own strings, for the names of its creatures and items: "
                 "<uid>_c_<language>.tlk, which the game loads for an enabled add-in of that uid. Pick string ids "
                 "no other table uses (a mod's own range, 99000000 and up). Ship it in the add-in with mod_pack, and "
                 "point a template's LocName at an id with template_edit.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"strings",
                         {{"type", "object"}, {"description", R"(Each id and its text: {"99000001": "Red Ogre"}.)"}}},
                        {"uid", {{"type", "string"}, {"description", "The add-in's uid, as mod_pack's."}}},
                        {"language", {{"type", "string"}, {"description", "Default en-us."}}},
                        {"to",
                         {{"type", "string"},
                          {"description", "The folder to write into, a full path. Never inside the game's folder."}}}}},
                      {"required", {"strings", "uid", "to"}}},
                 Thread::Listener, false},
        [&ghilana](const Json &args) -> ToolResult { return TalkTable(ghilana, args); });

    Json properties = {
        {"format",
         {{"type", "string"},
          {"enum", {"erf", "dazip"}},
          {"description", "erf: one archive as the game's own, for an override folder. dazip: an add-in package "
                          "for the game's DAUpdater, which installs it as Downloadable Content."}}},
        {"files",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"description", "The files, full paths: what texture_import and model_import wrote, or any. Each goes in "
                          "by its own name, lower case; the game finds a file by its name alone."}}},
        {"to",
         {{"type", "string"},
          {"description", "The folder to write into, a full path; made if missing. Never inside the game's folder."}}},
        {"name", {{"type", "string"}, {"description", "erf: the archive's name. Default mod.erf."}}},
        {"uid",
         {{"type", "string"},
          {"description", "dazip: the add-in's id, letters, digits and _ (\"MyName_RedOgre\"): its folder's name and "
                          "the game's key for it. Required for dazip."}}},
        {"title", {{"type", "string"}, {"description", "dazip: what the player sees. Default the uid."}}},
        {"description", {{"type", "string"}, {"description", "dazip: a line on what it does."}}},
        {"publisher", {{"type", "string"}, {"description", "dazip: who made it."}}},
        {"version", {{"type", "string"}, {"description", "dazip: default 1.0."}}},
        {"priority",
         {{"type", "integer"},
          {"description", "dazip: of two add-ins with one file, the lower priority's wins. Default 1000."}}},
    };
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    schema["required"] = {"format", "files", "to"};
    bench.Tools().Add(
        ToolSpec{"mod_pack",
                 "Packs files into one for the game: an .erf archive as the game's own, to put in an override folder, "
                 "or a .dazip add-in package that the game's DAUpdater installs as Downloadable Content, listed on its "
                 "screen to be turned on and off. Answers the file written and how it goes into the game. Writes "
                 "nothing in the game's folders.",
                 std::move(schema), Thread::Listener, false},
        [&ghilana](const Json &args) -> ToolResult { return Pack(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
