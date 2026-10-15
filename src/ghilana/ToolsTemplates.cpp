// Ghilana's tools for a mod's own creatures and items made from the game's
// (#141): rows of a table the game merges (table_rows), and a template with
// fields changed under a new name (template_edit), through DAOLib's core
// (GdaFile::Write and gff3::Editor, DAOLib #123). With model_copy
// (ToolsModels.cpp) a new creature has its own appearance and model.
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/Text.h"

#include <dao/core/GdaFile.h>
#include <dao/core/Gff3.h>

#include <algorithm>
#include <set>
#include <variant>

namespace eluvian::ghilana::detail
{

namespace
{

namespace gff3 = dao::core::gff3;
using dao::core::GdaFile;

ToolSpec Spec(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    schema["required"] = required;
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Listener, false};
}

// The game's copy of a file, read.
std::vector<std::byte> GameCopy(Ghilana &ghilana, const std::string &name, const std::string &tool)
{
    (void)ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select(tool, name, "", "");
    std::string why;
    auto bytes = files.Read(GameFiles::Choose(copies), &why);
    if (!bytes)
    {
        throw ToolError(tool + ": \"" + name + "\" cannot be read: " + why);
    }
    return std::move(*bytes);
}

// ---------------------------------------------------------------- table_rows

// A cell set from JSON, of the kind its column holds.
GdaFile::Cell CellOf(const Json &value, const GdaFile::Cell &like, const std::string &column)
{
    if (std::holds_alternative<std::wstring>(like))
    {
        if (!value.is_string())
        {
            throw ToolError("table_rows: column " + column + " holds text");
        }
        return Wide(value.get<std::string>());
    }
    if (std::holds_alternative<double>(like))
    {
        if (!value.is_number())
        {
            throw ToolError("table_rows: column " + column + " holds a number");
        }
        return value.get<double>();
    }
    if (!value.is_number_integer())
    {
        throw ToolError("table_rows: column " + column + " holds a whole number");
    }
    return value.get<std::int64_t>();
}

std::int64_t IdOf(const std::vector<GdaFile::Cell> &row, std::size_t idColumn)
{
    const auto *id = std::get_if<std::int64_t>(&row[idColumn]);
    return id != nullptr ? *id : -1;
}

// The IDs a table merged from files of its prefix already has (apr_base,
// apr_*): the game's own, its DLC's, and other mods'.
std::set<std::int64_t> MergedIds(Ghilana &ghilana, const std::string &prefix)
{
    std::set<std::int64_t> ids;
    const GameFiles &files = ghilana.Files();
    for (const auto &[name, copies] : files.Resources())
    {
        if (Extension(name) != "gda" || !name.starts_with(prefix + "_") || copies.empty())
        {
            continue;
        }
        const auto bytes = files.Read(GameFiles::Choose(copies));
        const auto table = bytes ? GdaFile::Parse(*bytes) : std::nullopt;
        const auto idColumn = table ? table->ColumnOf("ID") : std::nullopt;
        if (!idColumn)
        {
            continue;
        }
        for (const auto &row : table->Rows())
        {
            ids.insert(IdOf(row, *idColumn));
        }
    }
    return ids;
}

Json TableRows(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "table_rows");
    const fs::path to = OutputFolder(ghilana, args, "table_rows");
    std::string table = Lower(args.String("table"));
    if (Extension(table).empty())
    {
        table += ".gda";
    }
    std::string name = Lower(args.String("name"));
    if (Extension(name).empty())
    {
        name += ".gda";
    }
    const std::string prefix = Stem(table).substr(0, Stem(table).find('_'));
    if (!Stem(name).starts_with(prefix + "_"))
    {
        throw ToolError("table_rows: the game merges " + prefix + "_*.gda into one table, so name it " + prefix +
                        "_<yours>.gda");
    }
    const std::vector<std::byte> original = GameCopy(ghilana, table, "table_rows");
    const auto read = GdaFile::Parse(original);
    const auto idColumn = read ? read->ColumnOf("ID") : std::nullopt;
    if (!idColumn)
    {
        throw ToolError("table_rows: \"" + table + "\" is no table with an ID column");
    }
    const Json &asked = raw.contains("rows") ? raw["rows"] : Json();
    if (!asked.is_array() || asked.empty())
    {
        throw ToolError("table_rows: `rows` is a list of {copy, set}");
    }
    const std::set<std::int64_t> taken = MergedIds(ghilana, prefix);
    std::vector<std::vector<GdaFile::Cell>> rows;
    Json made = Json::array();
    for (const Json &spec : asked)
    {
        const std::int64_t copy = spec.value("copy", std::int64_t{-1});
        const auto from =
            std::ranges::find_if(read->Rows(), [&](const auto &row) { return IdOf(row, *idColumn) == copy; });
        if (from == read->Rows().end())
        {
            throw ToolError("table_rows: \"" + table + "\" has no row of ID " + std::to_string(copy));
        }
        std::vector<GdaFile::Cell> row = *from;
        // Held: items() of a temporary would outlive it.
        const Json set = spec.value("set", Json::object());
        for (const auto &[column, value] : set.items())
        {
            const auto at = read->ColumnOf(column);
            if (!at)
            {
                std::string message = "table_rows: \"" + table;
                message += "\" has no column ";
                message += column;
                throw ToolError(message);
            }
            row[*at] = CellOf(value, row[*at], column);
        }
        const std::int64_t id = IdOf(row, *idColumn);
        if (taken.contains(id) || std::ranges::any_of(rows, [&](const auto &r) { return IdOf(r, *idColumn) == id; }))
        {
            throw ToolError("table_rows: ID " + std::to_string(id) + " is taken in the " + prefix +
                            " table the game merges; `table` with the table's name lists its rows");
        }
        made.push_back(id);
        rows.push_back(std::move(row));
    }
    const auto bytes = GdaFile::Write(original, rows);
    if (bytes.empty())
    {
        throw ToolError("table_rows: DAOLib wrote no table: a cell is of another kind than its column's");
    }
    const fs::path path = to / fs::path(Wide(name));
    WriteFile(path, bytes, "table_rows");
    return {{"written", PathText(path)},
            {"rows", made},
            {"merged_into", prefix},
            {"into_the_game", "the game merges every " + prefix + "_*.gda it finds into the " + prefix +
                                  " table; put it in an override folder (or pack it with mod_pack) beside what uses "
                                  "the new rows"}};
}

// ---------------------------------------------------------------- template_edit

// Sets one field by its path ("Appearance Data/Appearance_Type").
void SetField(gff3::Editor &editor, const gff3::File &file, const std::string &path, const Json &value)
{
    std::optional<gff3::Struct> in = file.Root();
    std::string label = path;
    for (std::size_t slash = label.find('/'); slash != std::string::npos; slash = label.find('/'))
    {
        in = in ? in->Child(label.substr(0, slash)) : std::nullopt;
        label = label.substr(slash + 1);
    }
    const gff3::Field *field = in ? in->Find(label) : nullptr;
    if (field == nullptr)
    {
        throw ToolError("template_edit: no field " + path);
    }
    bool done = false;
    switch (field->type)
    {
    case gff3::Type::Float:
    case gff3::Type::Double:
        done = value.is_number() && editor.SetFloat(*in, label, value.get<double>());
        break;
    case gff3::Type::String:
    case gff3::Type::ResRef:
        done = value.is_string() && editor.SetString(*in, label, value.get<std::string>());
        break;
    case gff3::Type::LocString:
        done = value.is_number_integer() && editor.SetLoc(*in, label, gff3::LocString{value.get<std::uint32_t>(), {}});
        break;
    default:
        done = value.is_number_integer() && editor.SetNumber(*in, label, value.get<std::int64_t>());
        break;
    }
    if (!done)
    {
        throw ToolError("template_edit: field " + path +
                        " takes another kind of value (a number, a text, or a string reference for a name)");
    }
}

Json TemplateEdit(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "template_edit");
    const fs::path to = OutputFolder(ghilana, args, "template_edit");
    const std::string name = Lower(args.String("name"));
    std::string saveAs = Lower(args.String("save_as"));
    if (Extension(saveAs).empty())
    {
        saveAs += "." + Extension(name);
    }
    const std::vector<std::byte> bytes = GameCopy(ghilana, name, "template_edit");
    const auto file = gff3::File::Parse(bytes);
    if (!file)
    {
        throw ToolError("template_edit: \"" + name + "\" is no template (GFF 3.2) DAOLib reads");
    }
    gff3::Editor editor(*file);
    const Json &set = raw.contains("set") ? raw["set"] : Json::object();
    Json changed = Json::array();
    for (const auto &[path, value] : set.items())
    {
        SetField(editor, *file, path, value);
        changed.push_back(path);
    }
    const fs::path path = to / fs::path(Wide(saveAs));
    WriteFile(path, editor.Bytes(), "template_edit");
    return {{"written", PathText(path)},
            {"changed", changed},
            {"note", "a template's TemplateResRef is its own name: set it to the new one, and its Tag to what scripts "
                     "and stage_spawn find it by"}};
}

} // namespace

void RegisterTemplateTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();
    const Json to = {{"type", "string"},
                     {"description", "The folder to write into, a full path; made if missing. Never inside the game's "
                                     "folder."}};
    tools.Add(
        Spec("table_rows",
             "Writes a table of new rows for one the game merges from every file of its prefix (apr_base and "
             "apr_*, the appearances): each row copied from a row of `table` by its ID, with cells changed. A "
             "new ID already in the merged table, the game's, its DLC's or another mod's, is refused. Answers "
             "the file written; put it beside what uses the rows.",
             {{"table",
               {{"type", "string"},
                {"description", "The game's table to copy rows from, \"apr_base\": its columns are the new "
                                "file's."}}},
              {"rows",
               {{"type", "array"},
                {"items", {{"type", "object"}}},
                {"description", "Each {\"copy\": the ID of a row to copy, \"set\": {column: value}}; set ID to "
                                "a new one."}}},
              {"name",
               {{"type", "string"}, {"description", "The file's name, the table's prefix and yours: \"apr_mymod\"."}}},
              {"to", to}},
             {"table", "rows", "name", "to"}),
        [&ghilana](const Json &args) -> ToolResult { return TableRows(ghilana, args); });
    tools.Add(Spec("template_edit",
                   "Writes a template (.utc creature, .uti item, .utp placeable...) under a new name with fields "
                   "changed: a number, a text (a String or ResRef), or a string reference for a name. A field in a "
                   "struct is named by its path, \"Appearance Data/Appearance_Type\". `resource` shows a template's "
                   "fields.",
                   {{"name", {{"type", "string"}, {"description", "The game's template, \"ogre_default.utc\"."}}},
                    {"set",
                     {{"type", "object"},
                      {"description", "Each field's path and its new value: {\"Tag\": \"red_ogre\", \"Appearance "
                                      "Data/Appearance_Type\": 7701}."}}},
                    {"save_as",
                     {{"type", "string"},
                      {"description", "The new template's name, \"red_ogre\" (the extension is the original's)."}}},
                    {"to", to}},
                   {"name", "set", "save_as", "to"}),
              [&ghilana](const Json &args) -> ToolResult { return TemplateEdit(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
