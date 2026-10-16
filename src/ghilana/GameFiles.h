// The game's files as ghilana's tools ask for them: DAOLib's index of an
// install (dao/core/Install.h: the sources, every archive's directory, every
// copy of every resource in the order the game looks for it, a copy's
// bytes), built on the first call that wants it, and what ghilana adds over
// it: where a copy is, why the game does not have it and why it cannot be
// read, in an answer's words, and the copies a call names.
//
// The order is the game's, read in the exe and seen in play (DAOLib's
// dev/FILES.md, 2026-10-12): of a resource's copies the game takes the
// first, if it has it at all. Ghilana guessed until then.
#pragma once

#include "core/Tool.h"

#include <dao/core/Install.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eluvian::ghilana
{

namespace fs = std::filesystem;

using dao::core::AddIn;
using dao::core::install::Base;
using dao::core::install::Copy;
using dao::core::install::kLoose;
using dao::core::install::Source;

struct Paths
{
    fs::path game;      // the install, the folder that holds bin_ship
    fs::path documents; // Documents\BioWare\Dragon Age, or an instance's
    std::string language = "en-us";
    std::string instance; // empty for the game's own Documents
    // The plugins folder the game was started with, read in place of
    // <game>\bin_ship\daolib\plugins, as the loader does (game_launch's
    // `plugins`, DAOLIB_PLUGINS; DAOLib #143). Empty for the game's own: a
    // test that stages its plugins and deploys nothing has the game take
    // the staged overrides (elgara-agent, 2026-10-16, #145).
    fs::path plugins;
    // Where the index keeps the archives' directories between processes,
    // each read again only when its archive changes (DAOLib #144): a
    // --call is a process of its own, which read 10,612 archives in 5 s
    // each time (elgara-agent, 2026-10-16, #146). Empty for none.
    fs::path cache;
    // What the game is taken to be doing, which decides some of the copies
    // it has (dao::core::install::Options): the campaign loaded, as AddIns.xml
    // names one ("Single Player", "DAO_PRC_EP_1" for Awakening; empty for
    // the main menu), and the texture quality ("high" or "medium").
    std::string campaign = "Single Player";
    std::string textures = "high";
};

class GameFiles
{
  public:
    explicit GameFiles(Paths paths);

    [[nodiscard]] const Paths &Where() const
    {
        return paths_;
    }

    // Builds the index the first time it is asked, from the disk; true if
    // this call built it (and waited). Everything below wants it built.
    bool Index();
    [[nodiscard]] const dao::core::install::Index &Installed() const
    {
        return *index_;
    }
    [[nodiscard]] const dao::core::install::Stats &Stats() const
    {
        return index_->Measured();
    }
    [[nodiscard]] const std::vector<Source> &Sources() const
    {
        return index_->Sources();
    }
    [[nodiscard]] const std::vector<dao::core::install::ArchiveRecord> &Archives() const
    {
        return index_->Archives();
    }
    [[nodiscard]] const std::map<std::string, std::vector<Copy>, std::less<>> &Resources() const
    {
        return index_->Resources();
    }
    [[nodiscard]] const std::vector<Copy> *Find(std::string_view name) const
    {
        return index_->Find(name);
    }
    [[nodiscard]] std::optional<std::uint32_t> SourceOf(std::string_view id) const
    {
        return index_->SourceOf(id);
    }
    [[nodiscard]] bool Readable(const Copy &copy) const
    {
        return index_->Readable(copy);
    }

    // Why a copy cannot be read, for an answer; empty if it can be.
    [[nodiscard]] std::string WhyUnreadable(const Copy &copy) const;
    // A copy's bytes; nothing if it cannot be read, with why in `why`.
    std::optional<std::vector<std::byte>> Read(const Copy &copy, std::string *why = nullptr) const;
    // Where a copy is, as answers say it: {"source", "archive" (or "file" for a
    // loose one), "size", "priority" (the game's)}, "unused" where the game does
    // not have it and why, "settled": false where its place rests on what was
    // not seen, and "readable": false with "why" where it cannot be read.
    [[nodiscard]] Json Describe(const Copy &copy) const;

    // The copy the game takes of a resource's copies (Find's, in the game's
    // order): the first, if the game has it; null if it has none of them.
    [[nodiscard]] static const Copy *Taken(const std::vector<Copy> &copies);
    // The copy a tool reads of copies in the game's order: the one the game
    // takes, else the first.
    [[nodiscard]] static const Copy &Choose(const std::vector<Copy> &copies)
    {
        return copies.front();
    }
    // Why the game does not have a copy, in an answer's words; empty if it has.
    [[nodiscard]] std::string WhyUnused(const Copy &copy) const;

    // The copies of a name, or of the one copy a call named by `source`
    // and `archive` (or a loose file's path); a ToolError for none, that
    // says what there is.
    [[nodiscard]] std::vector<Copy> Select(std::string_view tool, std::string_view name, std::string_view source,
                                           std::string_view archive) const;

  private:
    [[nodiscard]] std::string NoSuchResource(std::string_view tool, std::string_view name) const;
    [[nodiscard]] bool IsIn(const Copy &copy, std::string_view source, const std::string &archiveKey) const;

    Paths paths_;
    std::once_flag indexed_;
    std::unique_ptr<dao::core::install::Index> index_;
};

} // namespace eluvian::ghilana
