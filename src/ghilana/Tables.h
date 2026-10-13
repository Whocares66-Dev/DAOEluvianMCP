// What `table` adds over DAOLib's merge of the game's data tables
// (dao/core/M2da.h): the worksheets read from the install's own
// m2da_base.gda and 2da_base.gda, each file of a table with the copy it was
// read from, and how a call's value matches a cell.
#pragma once

#include "core/Tool.h"
#include "ghilana/GameFiles.h"

#include <dao/core/GdaFile.h>
#include <dao/core/M2da.h>

#include <string>
#include <vector>

namespace eluvian::ghilana
{

// The tables m2da_base.gda and 2da_base.gda name, read from the copy of each
// GameFiles::Choose takes.
std::vector<dao::core::m2da::Worksheet> ReadWorksheets(const GameFiles &files);

// One file of a table, the copy of it the game takes.
struct TablePart
{
    std::string file; // "abi_base.gda"
    Json from;        // GameFiles::Describe of its copy
    dao::core::GdaFile table;
    bool afterUnreadable = false; // a file the game takes before it in its order could not be read
};

// The parts merged as the game merges them, given in the game's order of its
// files (a row's `part` is its place).
dao::core::m2da::Merged MergeParts(const std::vector<TablePart> &parts);

// Whether a cell holds what a call asked for: a number equal to a number,
// or text equal to text without case; a number asked as text is compared
// as its digits.
bool CellMatches(const Json &cell, const Json &wanted);

} // namespace eluvian::ghilana
