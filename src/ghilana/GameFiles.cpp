#include "ghilana/GameFiles.h"

#include "ghilana/Text.h"

namespace eluvian::ghilana
{

GameFiles::GameFiles(Paths paths) : paths_(std::move(paths))
{
}

bool GameFiles::Index()
{
    bool built = false;
    std::call_once(indexed_, [this, &built] {
        dao::core::install::Options options;
        options.campaign = paths_.campaign;
        options.language = paths_.language;
        options.textures = paths_.textures;
        index_ = dao::core::install::Index::Build(paths_.game, paths_.documents, options);
        built = true;
    });
    return built;
}

std::string GameFiles::WhyUnreadable(const Copy &copy) const
{
    if (Readable(copy))
    {
        return {};
    }
    const auto &header = Archives()[copy.archive].header;
    if (header.Encryption() != 0)
    {
        return "its archive is enciphered (ERF V2.2, the game's Blowfish), which ghilana cannot read: the password "
               "is the game's and unread. The running game reads it: Eluvian's in-game tools";
    }
    return "its archive is compressed (ERF V2.2, compression " + std::to_string(header.Compression()) +
           "), which no archive of the game's is and ghilana does not read";
}

std::optional<std::vector<std::byte>> GameFiles::Read(const Copy &copy, std::string *why) const
{
    auto bytes = index_->Read(copy);
    if (!bytes && why != nullptr)
    {
        *why = Readable(copy) ? "it could not be read whole from " + index_->Where(copy) + " (changed since the index?)"
                              : WhyUnreadable(copy);
    }
    return bytes;
}

Json GameFiles::Describe(const Copy &copy) const
{
    const Source &source = Sources()[copy.source];
    Json out = {{"source", source.id}};
    out[copy.archive == kLoose ? "file" : "archive"] = index_->Where(copy);
    if (source.base == Base::Documents)
    {
        out["in"] = "documents";
    }
    out["size"] = copy.size;
    out["priority"] = copy.priority;
    if (copy.unused != dao::core::install::Unused::No)
    {
        out["unused"] = WhyUnused(copy);
    }
    if (!copy.settled)
    {
        out["settled"] = false;
    }
    if (!Readable(copy))
    {
        out["readable"] = false;
        out["why"] = WhyUnreadable(copy);
    }
    return out;
}

const Copy *GameFiles::Taken(const std::vector<Copy> &copies)
{
    return !copies.empty() && copies.front().unused == dao::core::install::Unused::No ? &copies.front() : nullptr;
}

std::string GameFiles::WhyUnused(const Copy &copy) const
{
    using dao::core::install::Unused;
    const auto &set = index_->Set();
    switch (copy.unused)
    {
    case Unused::No:
        return {};
    case Unused::Campaign:
        return "another campaign's: the game has it while that campaign is loaded (ghilana takes \"" +
               (set.campaign.empty() ? std::string("none, the main menu") : set.campaign) + "\"; --campaign)";
    case Unused::Level:
        return "a level's own: the game has it only while that level is loaded";
    case Unused::Language:
        return "another language's (ghilana takes " + set.language + "; --language)";
    case Unused::Textures:
        return "another texture quality's (ghilana takes " + set.textures + "; --textures)";
    case Unused::Disabled:
        return "its add-in is not enabled in AddIns.xml, or its offer not listed in offers.xml";
    case Unused::Folder:
        return "in no folder the game takes files from";
    }
    return "the game does not have it";
}

std::string GameFiles::NoSuchResource(std::string_view tool, std::string_view name) const
{
    std::string said = std::string(tool) + ": no resource \"" + std::string(name) + "\" in any source.";
    if (Extension(name).empty())
    {
        std::string types;
        const std::string prefix = Lower(name) + ".";
        for (auto it = Resources().lower_bound(prefix); it != Resources().end() && it->first.starts_with(prefix); ++it)
        {
            types += types.empty() ? " " : ", ";
            types += it->first;
        }
        said += types.empty() ? " A name has its extension (\"abi_base.gda\")."
                              : " A name has its extension; there is" + types + ".";
    }
    return said + " `resources` finds names by a word.";
}

bool GameFiles::IsIn(const Copy &copy, std::string_view source, const std::string &archiveKey) const
{
    if (!source.empty() && Sources()[copy.source].id != Lower(source))
    {
        return false;
    }
    return archiveKey.empty() || PathKey(index_->Where(copy)) == archiveKey;
}

std::vector<Copy> GameFiles::Select(std::string_view tool, std::string_view name, std::string_view source,
                                    std::string_view archive) const
{
    const std::vector<Copy> *copies = Find(name);
    if (copies == nullptr)
    {
        throw ToolError(NoSuchResource(tool, name));
    }
    std::vector<Copy> chosen;
    const std::string archiveKey = PathKey(archive);
    for (const Copy &copy : *copies)
    {
        if (IsIn(copy, source, archiveKey))
        {
            chosen.push_back(copy);
        }
    }
    if (chosen.empty())
    {
        Json there = Json::array();
        for (const Copy &copy : *copies)
        {
            there.push_back(Describe(copy));
        }
        throw ToolError(std::string(tool) + ": \"" + std::string(name) + "\" has no copy in that source and archive; " +
                        "its copies are " + there.dump());
    }
    return chosen;
}

} // namespace eluvian::ghilana
