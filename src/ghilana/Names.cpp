#include "ghilana/Names.h"

#include "ghilana/Text.h"

#include <dao/core/GdaHash.h>

#include <charconv>
#include <string>
#include <unordered_map>

namespace eluvian::ghilana
{

namespace
{

struct FieldName
{
    std::uint32_t label;
    const char *name;
};

// kGff4Fields, kGdaColumns: written by cmake/Ghilana.cmake from DAOLib's
// dev/gff4_fields.csv and dev/gda_column_names.txt.
#include "ghilana_names.inc"

// Made once, from the tables; a constructor of its own, which may throw
// (the maps ask for memory), where an implicit one would be taken not to.
struct Tables
{
    Tables()
    {
        for (const auto &[label, name] : kGff4Fields)
        {
            fieldNames.emplace(label, name);
            fieldLabels.emplace(Lower(name), label);
        }
        for (const char *name : kGdaColumns)
        {
            columns.emplace(dao::core::GdaColumnHash(name), name);
        }
    }

    std::unordered_map<std::uint32_t, std::string_view> fieldNames;
    std::unordered_map<std::string, std::uint32_t> fieldLabels; // lower case
    std::unordered_map<std::uint32_t, std::string_view> columns;
};

const Tables &Get()
{
    static const Tables tables;
    return tables;
}

} // namespace

std::string_view Gff4FieldName(std::uint32_t label)
{
    const auto &names = Get().fieldNames;
    const auto it = names.find(label);
    return it == names.end() ? std::string_view() : it->second;
}

std::optional<std::uint32_t> Gff4FieldLabel(std::string_view name)
{
    if (name.starts_with('#'))
    {
        std::uint32_t label = 0;
        const auto *end = name.data() + name.size();
        const auto [at, error] = std::from_chars(name.data() + 1, end, label);
        if (error == std::errc() && at == end && name.size() > 1)
        {
            return label;
        }
        return std::nullopt;
    }
    const auto &labels = Get().fieldLabels;
    const auto it = labels.find(Lower(name));
    if (it == labels.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::string_view GdaColumnName(std::uint32_t hash)
{
    const auto &columns = Get().columns;
    const auto it = columns.find(hash);
    return it == columns.end() ? std::string_view() : it->second;
}

std::size_t Gff4FieldNames()
{
    return Get().fieldNames.size();
}

std::size_t GdaColumnNames()
{
    return Get().columns.size();
}

} // namespace eluvian::ghilana
