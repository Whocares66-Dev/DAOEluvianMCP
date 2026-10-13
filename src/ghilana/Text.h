// Text as ghilana's answers carry it: UTF-8, from the UTF-16 of the game's
// GFF4 files and talk tables and the 8-bit strings of its GFF3 templates;
// names compared without case, as the game's resource names are; and the
// patterns a caller names resources by.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace eluvian::ghilana
{

// UTF-16 (wchar_t on Windows) as UTF-8. A lone surrogate becomes U+FFFD.
std::string Utf8(std::wstring_view text);

// UTF-8 as UTF-16, for a path the caller named. Bytes that are no UTF-8
// are taken as Latin-1.
std::wstring Wide(std::string_view text);

// Eight-bit text as UTF-8, each byte its Latin-1 character: a GFF3 file's
// strings, which xoreos reads as ASCII (src/aurora/gff3file.cpp); a byte
// past ASCII is kept rather than lost.
std::string Latin1(std::string_view text);

// ASCII letters in lower case; the rest as they are.
std::string Lower(std::string_view text);

// Whether `text` matches `pattern`, without case: `*` any run, `?` any one
// character, the rest itself.
bool Glob(std::string_view pattern, std::string_view text);

// Whether a pattern has `*` or `?` in it.
bool IsPattern(std::string_view text);

// A name's extension without its dot, in lower case ("utc"); empty for none.
std::string Extension(std::string_view name);
// A name without its extension ("ogre_default").
std::string Stem(std::string_view name);

// A path as UTF-8 with forward slashes, how answers write paths.
std::string PathText(const std::filesystem::path &path);

// A path a caller gave, compared as the index keys them: lower case,
// forward slashes, no leading "./" or slash.
std::string PathKey(std::string_view path);

} // namespace eluvian::ghilana
