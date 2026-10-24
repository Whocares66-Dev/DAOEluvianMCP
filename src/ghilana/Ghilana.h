// Ghilana: the MCP server of the tools that need no game (#128,
// dev/GHILANA.md). This is what its tools share, the game's files and what
// is read from them once, and their registration on a Bench, which
// ghilana.exe serves over stdio (core/Stdio.h), answers one --call with,
// and lists for --describe.
//
// Every tool runs on the calling thread (Thread::Listener): there is no
// frame thread and no game. A tool reads files and nothing of any process.
#pragma once

#include "core/Args.h"
#include "core/Bench.h"
#include "ghilana/Folders.h"
#include "ghilana/GameFiles.h"
#include "ghilana/Tables.h"

#include <dao/core/TalkTables.h>

#include <array>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eluvian::ghilana
{

class Ghilana
{
  public:
    // `installs` is what was found on the machine, for `environment`;
    // `paths.game` the one served. `cache` is where ghilana may keep files.
    Ghilana(Paths paths, std::vector<Install> installs, fs::path cache, std::string version);

    GameFiles &Files()
    {
        return files_;
    }
    [[nodiscard]] const std::vector<Install> &Installs() const
    {
        return installs_;
    }
    [[nodiscard]] const fs::path &Cache() const
    {
        return cache_;
    }
    [[nodiscard]] const std::string &Version() const
    {
        return version_;
    }

    // The index, built if this is the first call to want it; what the
    // building took, for the answer, when this call built it, else null.
    Json Indexed();

    // The talk tables the game searches once the campaign has loaded with a
    // hero of this sex, in its order, and an id's text as it finds it; read
    // the first time asked. DAOLib's, read in the exe (#106): until #169
    // ghilana took every table of the language by name, and only the _f
    // ones for a female hero.
    const dao::core::install::Strings &TalkStrings(bool female);

    // m2da_base.gda's and 2da_base.gda's tables, read once.
    const std::vector<dao::core::m2da::Worksheet> &Worksheets();

  private:
    GameFiles files_;
    std::vector<Install> installs_;
    fs::path cache_;
    std::string version_;
    std::mutex lock_;
    std::array<std::optional<dao::core::install::Strings>, 2> strings_; // male, female
    std::optional<std::vector<dao::core::m2da::Worksheet>> worksheets_;
};

// Registers every ghilana tool on the bench.
void RegisterTools(Bench &bench, Ghilana &ghilana);

// What initialize says to an agent: what the server is, and where to start.
std::string Instructions();

namespace detail
{
void RegisterFileTools(Bench &bench, Ghilana &ghilana);
void RegisterDataTools(Bench &bench, Ghilana &ghilana);
void RegisterModelTools(Bench &bench, Ghilana &ghilana);
void RegisterTextureTools(Bench &bench, Ghilana &ghilana);
void RegisterModTools(Bench &bench, Ghilana &ghilana);
void RegisterTemplateTools(Bench &bench, Ghilana &ghilana);
void RegisterLevelTools(Bench &bench, Ghilana &ghilana);
// The `limit` and `offset` a paged tool takes, in its schema.
Json PageProperties(int defaultLimit, int mostLimit);

// A paged answer: which of the matches to give, and how the answer says so.
class Page
{
  public:
    Page(const Args &args, std::int64_t defaultLimit, std::int64_t mostLimit);

    // Whether the match at `index` (counting every match) is given, `taken` given so far.
    [[nodiscard]] bool Takes(std::size_t index, std::size_t taken) const
    {
        return index >= offset_ && taken < limit_;
    }
    // `count`, `offset`, the matches under `key`, and `more` if there are more.
    void Answer(Json &out, std::size_t count, const char *key, Json matches) const;

  private:
    std::size_t limit_;
    std::size_t offset_;
};

// The source a call's `source` names, if it names one; a ToolError for none such.
std::optional<std::uint32_t> SourceArg(const GameFiles &files, const Args &args, std::string_view tool);
// Whether `folder` is `root` or inside it, compared without case; false for
// no root.
bool Inside(const fs::path &folder, const fs::path &root);
// The folder a call's `to` names, made: a full path, never inside the
// game's folder. A ToolError otherwise.
fs::path OutputFolder(Ghilana &ghilana, const Args &args, const std::string &tool);
// Writes a file whole; a ToolError if it cannot.
void WriteFile(const fs::path &path, std::span<const std::byte> bytes, const std::string &tool);
// A copy's name and where it is, with the copies passed over when one was
// chosen of several.
Json Chosen(const GameFiles &files, const std::string &name, const std::vector<Copy> &copies, const Copy &copy);
} // namespace detail

} // namespace eluvian::ghilana
