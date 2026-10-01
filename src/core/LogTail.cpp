#include "core/LogTail.h"

#include <fstream>
#include <stdexcept>
#include <string_view>

namespace eluvian
{

LogPage ReadLog(const std::filesystem::path &file, std::uint64_t cursor, const std::optional<std::regex> &filter,
                std::size_t maxLines, std::size_t maxBytes)
{
    // The game holds its logs open for writing; MSVC's ifstream opens with
    // _SH_DENYNO, and spdlog's writer shares too, so the read is allowed.
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot open " + file.string());
    }
    LogPage page;
    in.seekg(0, std::ios::end);
    page.size = static_cast<std::uint64_t>(in.tellg());
    if (cursor > page.size)
    {
        page.restarted = true;
        cursor = 0;
    }
    page.next = cursor;
    in.seekg(static_cast<std::streamoff>(cursor));

    const std::uint64_t available = page.size - cursor;
    const std::size_t want = available < maxBytes ? static_cast<std::size_t>(available) : maxBytes;
    std::string text(want, '\0');
    in.read(text.data(), static_cast<std::streamsize>(want));
    text.resize(static_cast<std::size_t>(in.gcount()));

    std::size_t at = 0;
    while (at < text.size())
    {
        const auto newline = text.find('\n', at);
        if (newline == std::string::npos)
        {
            break; // a line still being written, or cut by maxBytes: the next read has it
        }
        std::string_view line(text.data() + at, newline - at);
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        if (!filter || std::regex_search(line.begin(), line.end(), *filter))
        {
            if (page.lines.size() == maxLines)
            {
                page.truncated = true;
                return page;
            }
            page.lines.emplace_back(line);
        }
        at = newline + 1;
        page.next = cursor + at;
    }
    page.truncated = page.next < page.size && want < available;
    return page;
}

} // namespace eluvian
