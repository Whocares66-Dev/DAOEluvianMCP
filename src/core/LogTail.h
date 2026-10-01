// A log file read from a cursor: the lines written since the agent last
// looked, and the cursor to look from next time.
//
// The play test of 2026-09-30 re-read turnbased.log from a timestamp with awk
// after every order. Here a read hands back a byte offset; the next read
// starts there. A line still being written (no newline yet) is left for the
// next read. A file shorter than the cursor was started afresh (the game
// restarted, and DAOLib truncates its logs at start-up), and the read starts
// from its beginning and says so.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace eluvian
{

struct LogPage
{
    std::vector<std::string> lines;
    std::uint64_t next = 0; // the cursor for the next read
    std::uint64_t size = 0; // the file's size when read
    bool restarted = false; // the file was shorter than the cursor: read from its start
    bool truncated = false; // stopped at the line or byte limit before the end
};

// Reads complete lines from `cursor` on, keeping those `filter` finds a
// match in, until `maxLines` are kept or `maxBytes` read. Throws
// std::runtime_error if the file cannot be opened.
LogPage ReadLog(const std::filesystem::path &file, std::uint64_t cursor, const std::optional<std::regex> &filter,
                std::size_t maxLines, std::size_t maxBytes);

} // namespace eluvian
