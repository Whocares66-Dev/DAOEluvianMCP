#include "ghilana/Ghilana.h"

#include "ghilana/Text.h"

#include <cstdint>
#include <fstream>

namespace eluvian::ghilana
{

namespace
{

// The paths, with the index's directories kept in ghilana's cache folder.
Paths Cached(Paths paths, const fs::path &cache)
{
    if (!cache.empty())
    {
        paths.cache = cache / L"index";
    }
    return paths;
}

} // namespace

Ghilana::Ghilana(Paths paths, std::vector<Install> installs, fs::path cache, std::string version)
    : files_(Cached(std::move(paths), cache)), installs_(std::move(installs)), cache_(std::move(cache)),
      version_(std::move(version))
{
}

Json Ghilana::Indexed()
{
    if (!files_.Index())
    {
        return nullptr;
    }
    const auto &stats = files_.Stats();
    return Json{{"built", true},
                {"ms", static_cast<std::int64_t>(stats.ms)},
                {"archives", stats.archives},
                {"copies", stats.copies},
                {"resources", stats.resources},
                {"read", stats.read},
                {"kept", stats.kept},
                {"note", "The index of every archive's directory was made now, once; later calls use it. `kept` "
                         "came from the cache on disk, unchanged since an earlier ghilana read them; `read`, "
                         "from the archives."}};
}

const std::vector<LoadedTalkTable> &Ghilana::TalkTables(const std::string &language, bool female)
{
    files_.Index();
    const std::string suffix = "_" + Lower(language) + (female ? "_f" : "") + ".tlk";
    const std::scoped_lock guard(lock_);
    const auto found = talk_.find(suffix);
    if (found != talk_.end())
    {
        return found->second;
    }
    std::vector<LoadedTalkTable> tables;
    for (const auto &[name, copies] : files_.Resources())
    {
        if (!name.ends_with(suffix))
        {
            continue;
        }
        // "core_en-us.tlk" ends with "_en-us.tlk" and "core_en-us_f.tlk" does
        // not, so only `female` reads the _f ones. Of a talk table's copies,
        // the first, the one the game takes; it is marked unused where the
        // language asked is not the index's, so that is not asked of it.
        const Copy &copy = GameFiles::Choose(copies);
        const auto bytes = files_.Read(copy);
        auto table = bytes ? dao::core::TalkTableFile::Parse(*bytes) : std::nullopt;
        if (table)
        {
            tables.push_back(LoadedTalkTable{name, files_.Describe(copy), std::move(*table)});
        }
    }
    return talk_.emplace(suffix, std::move(tables)).first->second;
}

const std::vector<dao::core::m2da::Worksheet> &Ghilana::Worksheets()
{
    files_.Index();
    const std::scoped_lock guard(lock_);
    if (!worksheets_)
    {
        worksheets_ = ReadWorksheets(files_);
    }
    return *worksheets_;
}

void RegisterTools(Bench &bench, Ghilana &ghilana)
{
    detail::RegisterFileTools(bench, ghilana);
    detail::RegisterDataTools(bench, ghilana);
    detail::RegisterModelTools(bench, ghilana);
    detail::RegisterTextureTools(bench, ghilana);
    detail::RegisterModTools(bench, ghilana);
    detail::RegisterTemplateTools(bench, ghilana);
    detail::RegisterLevelTools(bench, ghilana);
}

std::string Instructions()
{
    return "Ghilana reads Dragon Age: Origins' files on disk, with or without the game running: its archives, data "
           "tables, talk tables, templates, conversations and textures. It never writes into the game's folders. "
           "Start with `environment` (the install served, its add-ins and override folders, what cannot be read). "
           "`resources` finds a resource's name by a word; `resource` reads one; `conflicts` says which sources have "
           "a resource and which copy the game takes; `table` reads a data table merged as the game merges it; "
           "`strings` turns string ids to text and back; `query` searches every template or file of a type by its "
           "fields; `level`, `atmosphere`, `cutscene` and `stage` decode a level's rooms, models and lights, its sky, "
           "a "
           "cutscene's actions and a stage's cameras. A resource read by its name is the copy the game takes, in the "
           "game's own order (read in the "
           "exe), for the campaign, language and texture quality ghilana was started with. The running game is "
           "Eluvian's server, not this one.";
}

namespace detail
{

Json PageProperties(int defaultLimit, int mostLimit)
{
    return Json{{"limit",
                 {{"type", "integer"},
                  {"minimum", 1},
                  {"maximum", mostLimit},
                  {"description", "The most to return. Default " + std::to_string(defaultLimit) + "."}}},
                {"offset",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"description", "How many to skip first, to page through a long answer. Default 0."}}}};
}

Page::Page(const Args &args, std::int64_t defaultLimit, std::int64_t mostLimit)
    : limit_(static_cast<std::size_t>(args.Integer("limit", defaultLimit, 1, mostLimit))),
      offset_(static_cast<std::size_t>(args.Integer("offset", 0, 0, INT64_MAX)))
{
}

void Page::Answer(Json &out, std::size_t count, const char *key, Json matches) const
{
    out["count"] = count;
    out["offset"] = offset_;
    out[key] = std::move(matches);
    if (count > offset_ + limit_)
    {
        out["more"] = "page on with offset " + std::to_string(offset_ + limit_);
    }
}

std::optional<std::uint32_t> SourceArg(const GameFiles &files, const Args &args, std::string_view tool)
{
    const std::string id = Lower(args.String("source", ""));
    if (id.empty())
    {
        return std::nullopt;
    }
    const auto source = files.SourceOf(id);
    if (!source)
    {
        throw ToolError(std::string(tool) + ": no source \"" + id + "\"; `environment` lists them");
    }
    return source;
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

void WriteFile(const fs::path &path, std::span<const std::byte> bytes, const std::string &tool)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        throw ToolError(tool + ": could not write " + PathText(path));
    }
}

fs::path OutputFolder(Ghilana &ghilana, const Args &args, const std::string &tool)
{
    const fs::path to = fs::path(Wide(args.String("to")));
    if (!to.is_absolute())
    {
        throw ToolError(tool + R"(: `to` is a folder's full path ("C:\\work\\models"))");
    }
    if (Inside(to, ghilana.Files().Where().game))
    {
        throw ToolError(tool + ": `to` is inside the game's folder, which ghilana never writes into; name a folder "
                               "of your own");
    }
    std::error_code error;
    fs::create_directories(to, error);
    if (error)
    {
        throw ToolError(tool + ": cannot make " + PathText(to) + ": " + error.message());
    }
    return to;
}

namespace
{

bool Same(const Copy &a, const Copy &b)
{
    return a.source == b.source && a.archive == b.archive && a.entry == b.entry;
}

} // namespace

Json Chosen(const GameFiles &files, const std::string &name, const std::vector<Copy> &copies, const Copy &copy)
{
    Json out = {{"name", name}, {"copy", files.Describe(copy)}};
    const Copy *taken = GameFiles::Taken(copies);
    if (taken != nullptr && Same(*taken, copy))
    {
        out["taken"] = true;
    }
    else if (taken != nullptr)
    {
        out["taken"] = false;
        out["the_game_takes"] = files.Describe(*taken);
    }
    else
    {
        out["taken"] = false;
        out["the_game_takes"] = nullptr;
        out["why"] = "the game has none of its copies: " + files.WhyUnused(copies.front());
    }
    if (taken != nullptr && !taken->settled)
    {
        out["order"] = "unsettled: AddIns.xml has an add-in disabled or with no folder, which may or may not take a "
                       "place in the game's count of add-ins, and the copy the game takes rests on that";
    }
    Json others = Json::array();
    for (const Copy &other : copies)
    {
        if (!Same(other, copy))
        {
            others.push_back(files.Describe(other));
        }
    }
    if (!others.empty())
    {
        out["other_copies"] = std::move(others); // in the order the game looks for it
    }
    return out;
}

} // namespace detail

} // namespace eluvian::ghilana
