// The names in one of the game's resource archives: what `resources`
// lists, so a test can find a creature's template, an
// item's or a plot's by a word ("ogre_default.utc") instead of guessing
// (a tester tried "ogre", "hurlock" and "bandit" for spawn and got none,
// 2026-10-02). The game's own .erf and .rim files are ERF V2.0 (266 .erf
// and nearly 10,000 .rim) and V2.2 (377 .erf, the DLC's), read
// 2026-10-02; both open with the format's name in UTF-16, then a count,
// then a fixed-size entry per resource whose first 64 bytes are its name
// in UTF-16 with its extension. Only the index is read, never a resource.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eluvian
{

// How many bytes from an archive's start hold its index (the header and
// every entry), given at least its first kErfHeadBytes; nothing if the
// bytes are no ERF V2.0 or V2.2.
inline constexpr std::size_t kErfHeadBytes = 56;
std::optional<std::size_t> ErfIndexBytes(std::span<const std::byte> head);

// The resources' names, as the archive holds them ("ogre_default.utc"),
// from the archive's first ErfIndexBytes bytes. Empty for no ERF; an index
// cut short gives the names it has.
std::vector<std::string> ErfNames(std::span<const std::byte> index);

} // namespace eluvian
