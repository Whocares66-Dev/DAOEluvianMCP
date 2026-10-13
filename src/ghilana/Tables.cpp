#include "ghilana/Tables.h"

#include "ghilana/Text.h"

namespace eluvian::ghilana
{

namespace
{

std::string TextOf(const Json &value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

} // namespace

std::vector<dao::core::m2da::Worksheet> ReadWorksheets(const GameFiles &files)
{
    std::vector<dao::core::m2da::Worksheet> out;
    for (const bool merged : {true, false})
    {
        const auto *copies = files.Find(merged ? "m2da_base.gda" : "2da_base.gda");
        if (copies == nullptr)
        {
            continue;
        }
        const auto bytes = files.Read(GameFiles::Choose(*copies));
        const auto table = bytes ? dao::core::GdaFile::Parse(*bytes) : std::nullopt;
        if (table)
        {
            auto sheets = dao::core::m2da::Worksheets(*table, merged);
            out.insert(out.end(), sheets.begin(), sheets.end());
        }
    }
    return out;
}

dao::core::m2da::Merged MergeParts(const std::vector<TablePart> &parts)
{
    std::vector<const dao::core::GdaFile *> tables;
    tables.reserve(parts.size());
    for (const auto &part : parts)
    {
        tables.push_back(&part.table);
    }
    return dao::core::m2da::Merge(tables);
}

bool CellMatches(const Json &cell, const Json &wanted)
{
    if (cell.is_null())
    {
        return wanted.is_null();
    }
    if (cell.is_number() && wanted.is_number())
    {
        return cell.get<double>() == wanted.get<double>();
    }
    return Lower(TextOf(cell)) == Lower(TextOf(wanted));
}

} // namespace eluvian::ghilana
