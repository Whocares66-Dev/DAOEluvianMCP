// The game's files as ghilana's answers give them: JSON made from what
// DAOLib's readers read (dao/core/Gff4.h, Gff3.h, GdaFile.h). Each GFF4 field
// is named as Names.h names it, or "#label" where nothing does; a GFF3 file
// names its own; a table's columns are named from their hashes.
//
// What a reader has no read for, or what runs out of the file, is said in
// the field ({"_unread": "..."}), never left out or guessed.
#pragma once

#include "core/Tool.h"

#include <dao/core/GdaFile.h>
#include <dao/core/Gff3.h>
#include <dao/core/Gff4.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eluvian::ghilana
{

// How much of a file goes into its JSON: structs nested deeper than `depth`
// are left as their field count, lists longer than `items` keep their first
// `items` and say how many more there were.
struct Limits
{
    int depth = 32;
    std::size_t items = SIZE_MAX;
};

// A GFF4 file's top struct.
Json Gff4ToJson(const dao::core::gff4::File &file, const Limits &limits = {});

// A GFF3 file's top struct: each field by its label, a struct an object, a
// list an array, a localised string {"strref", and "strings" by language id
// if it holds any}, a vector or orientation an array, void data {"bytes":
// count}. Text is Latin-1 read as UTF-8.
Json Gff3ToJson(const dao::core::gff3::File &file, const Limits &limits = {});

// A GFF4 type's name ("u32", "string", "tlk"), for a column or a field.
std::string_view Gff4TypeName(std::uint16_t type);

// A table's cell: null, a number, or text.
Json CellJson(const dao::core::GdaFile::Cell &cell);
// A table's column's name: as Names.h names its hash, or "#" and the hash in hex.
std::string ColumnName(std::uint32_t hash);
// A table's column's type as the rows hold it, or "" if no row does.
std::string ColumnType(const dao::core::GdaFile::Column &column);

} // namespace eluvian::ghilana
