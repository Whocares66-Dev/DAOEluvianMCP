// Names for what the game's files hold only as numbers: a GFF4 field's
// label (12002, a conversation's lines) and a GDA column's hash (the CRC32
// of its name). Built into ghilana from DAOLib's dev/gff4_fields.csv
// (xoreos's src/aurora/gff4fields.h, by DAOLib's tools/gff4.py
// --import-names) and dev/gda_tables.csv (every column of the game's 749
// tables that xoreos's src/aurora/gdaheaders.cpp names, by DAOLib's
// tools/gda.py), which CMake turns into ghilana_names.inc at configure.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace eluvian::ghilana
{

// A GFF4 field's name; empty for a label nothing names.
std::string_view Gff4FieldName(std::uint32_t label);

// The label a GFF4 field name stands for, without case; nothing for a name
// not known. "#12002" is read as the label it writes.
std::optional<std::uint32_t> Gff4FieldLabel(std::string_view name);

// A GDA column's name, by its hash; empty for a hash nothing names.
std::string_view GdaColumnName(std::uint32_t hash);

// How many names of each kind are known.
std::size_t Gff4FieldNames();
std::size_t GdaColumnNames();

} // namespace eluvian::ghilana
