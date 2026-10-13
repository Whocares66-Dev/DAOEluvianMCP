#include "ghilana/FileJson.h"

#include "ghilana/Names.h"
#include "ghilana/Text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <variant>

namespace eluvian::ghilana
{

namespace
{

namespace gff4 = dao::core::gff4;

Json Struct(const gff4::Struct &node, const Limits &limits, int depth);

template <class T> Json Scalar(const gff4::Struct &node, std::uint32_t label)
{
    const auto value = node.Get<T>(label);
    return value ? Json(*value) : Json(nullptr);
}

std::string FieldKey(std::uint32_t label)
{
    const auto name = Gff4FieldName(label);
    return name.empty() ? "#" + std::to_string(label) : std::string(name);
}

template <class T> Json Elements(std::span<const std::byte> bytes, std::size_t items)
{
    Json out = Json::array();
    const std::size_t count = bytes.size() / sizeof(T);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i >= items)
        {
            out.push_back("... " + std::to_string(count - i) + " more");
            break;
        }
        T value{};
        std::memcpy(&value, bytes.data() + (i * sizeof(T)), sizeof(T));
        out.push_back(value);
    }
    return out;
}

Json List(const gff4::Struct &node, const gff4::Field &field, const Limits &limits, int depth)
{
    const auto type = static_cast<gff4::Type>(field.type);
    const bool generic = type == gff4::Type::Generic && (field.flags & gff4::kStruct) == 0;
    if ((field.flags & gff4::kStruct) != 0 || generic)
    {
        if (depth >= limits.depth)
        {
            return Json{{"_items", node.Structs(field.label).size()}};
        }
        const auto structs = node.Structs(field.label);
        Json out = Json::array();
        for (std::size_t i = 0; i < structs.size(); ++i)
        {
            if (i >= limits.items)
            {
                out.push_back("... " + std::to_string(structs.size() - i) + " more");
                break;
            }
            Json element = Struct(structs[i], limits, depth + 1);
            if (generic)
            {
                Json labelled = {{"_struct", structs[i].Label()}};
                labelled.update(element);
                element = std::move(labelled);
            }
            out.push_back(std::move(element));
        }
        return out;
    }
    switch (type)
    {
    case gff4::Type::U8:
        return Elements<std::uint8_t>(node.ListBytes(field.label, type), limits.items);
    case gff4::Type::U16:
        return Elements<std::uint16_t>(node.ListBytes(field.label, type), limits.items);
    case gff4::Type::U32:
        return Elements<std::uint32_t>(node.ListBytes(field.label, type), limits.items);
    case gff4::Type::F32:
        return Elements<float>(node.ListBytes(field.label, type), limits.items);
    case gff4::Type::S32: {
        Json out = Json::array();
        const auto numbers = node.Numbers(field.label);
        for (std::size_t i = 0; i < numbers.size(); ++i)
        {
            if (i >= limits.items)
            {
                out.push_back("... " + std::to_string(numbers.size() - i) + " more");
                break;
            }
            out.push_back(static_cast<std::int32_t>(numbers[i]));
        }
        return out;
    }
    default:
        return Json{{"_unread", "a list of " + std::string(Gff4TypeName(field.type)) +
                                    ", which DAOLib's GFF4 reader does not read yet"}};
    }
}

Json Value(const gff4::Struct &node, const gff4::Field &field, const Limits &limits, int depth)
{
    if ((field.flags & gff4::kList) != 0)
    {
        return List(node, field, limits, depth);
    }
    if ((field.flags & gff4::kStruct) != 0)
    {
        const auto child = node.Child(field.label);
        if (!child)
        {
            return nullptr;
        }
        if (depth >= limits.depth)
        {
            return Json{{"_fields", child->Shape().fields.size()}};
        }
        return Struct(*child, limits, depth + 1);
    }
    switch (static_cast<gff4::Type>(field.type))
    {
    case gff4::Type::U8:
        return Scalar<std::uint8_t>(node, field.label);
    case gff4::Type::S8:
        return Scalar<std::int8_t>(node, field.label);
    case gff4::Type::U16:
        return Scalar<std::uint16_t>(node, field.label);
    case gff4::Type::S16:
        return Scalar<std::int16_t>(node, field.label);
    case gff4::Type::U32:
        return Scalar<std::uint32_t>(node, field.label);
    case gff4::Type::S32:
        return Scalar<std::int32_t>(node, field.label);
    case gff4::Type::U64:
        return Scalar<std::uint64_t>(node, field.label);
    case gff4::Type::S64:
        return Scalar<std::int64_t>(node, field.label);
    case gff4::Type::F32:
        return Scalar<float>(node, field.label);
    case gff4::Type::F64:
        return Scalar<double>(node, field.label);
    case gff4::Type::String: {
        const auto text = node.String(field.label);
        return text ? Json(Utf8(*text)) : Json(nullptr);
    }
    case gff4::Type::TlkString: {
        const auto tlk = node.Tlk(field.label);
        if (!tlk)
        {
            return nullptr;
        }
        return Json{{"strref", tlk->strref == 0xFFFFFFFF ? Json(nullptr) : Json(tlk->strref)}};
    }
    case gff4::Type::Vector3:
    case gff4::Type::Vector4:
    case gff4::Type::Quaternion:
    case gff4::Type::Color:
    case gff4::Type::Matrix: {
        Json out = Json::array();
        for (const float f : node.Floats(field.label))
        {
            out.push_back(f);
        }
        return out;
    }
    default:
        return Json{{"_unread", "a field of " + std::string(Gff4TypeName(field.type)) +
                                    ", which DAOLib's GFF4 reader does not read yet"}};
    }
}

Json Struct(const gff4::Struct &node, const Limits &limits, int depth)
{
    Json out = Json::object();
    for (const auto &field : node.Shape().fields)
    {
        out[FieldKey(field.label)] = Value(node, field, limits, depth);
    }
    return out;
}

} // namespace

Json Gff4ToJson(const dao::core::gff4::File &file, const Limits &limits)
{
    const auto root = file.Root();
    if (!root)
    {
        return Json::object();
    }
    return Struct(*root, limits, 0);
}

std::string_view Gff4TypeName(std::uint16_t type)
{
    switch (static_cast<gff4::Type>(type))
    {
    case gff4::Type::U8:
        return "u8";
    case gff4::Type::S8:
        return "s8";
    case gff4::Type::U16:
        return "u16";
    case gff4::Type::S16:
        return "s16";
    case gff4::Type::U32:
        return "u32";
    case gff4::Type::S32:
        return "s32";
    case gff4::Type::U64:
        return "u64";
    case gff4::Type::S64:
        return "s64";
    case gff4::Type::F32:
        return "f32";
    case gff4::Type::F64:
        return "f64";
    case gff4::Type::Vector3:
        return "vector3";
    case gff4::Type::Vector4:
        return "vector4";
    case gff4::Type::Quaternion:
        return "quaternion";
    case gff4::Type::String:
        return "string";
    case gff4::Type::Color:
        return "color";
    case gff4::Type::Matrix:
        return "matrix";
    case gff4::Type::TlkString:
        return "tlk";
    case gff4::Type::Generic:
        return "generic";
    default:
        return "struct";
    }
}

namespace
{

namespace gff3 = dao::core::gff3;

Json Unread()
{
    return Json{{"_unread", "its value runs out of the file"}};
}

template <class T> Json Number(const gff3::Struct &node, const std::string &label)
{
    const auto value = node.Get<T>(label);
    return value ? Json(*value) : Unread();
}

Json Text(const std::optional<std::string> &text)
{
    return text ? Json(Latin1(*text)) : Unread();
}

Json Gff3Struct(const gff3::Struct &node, const Limits &limits, int depth);

Json Gff3List(const gff3::Struct &node, const std::string &label, const Limits &limits, int depth)
{
    const auto structs = node.List(label);
    Json out = Json::array();
    for (std::size_t i = 0; i < structs.size(); ++i)
    {
        if (i >= limits.items)
        {
            out.push_back("... " + std::to_string(structs.size() - i) + " more");
            break;
        }
        out.push_back(Gff3Struct(structs[i], limits, depth + 1));
    }
    return out;
}

Json Gff3Loc(const gff3::Struct &node, const std::string &label)
{
    const auto loc = node.Loc(label);
    if (!loc)
    {
        return Unread();
    }
    Json out = {{"strref", loc->strref == 0xFFFFFFFF ? Json(nullptr) : Json(loc->strref)}};
    if (!loc->strings.empty())
    {
        Json strings = Json::object();
        for (const auto &[id, text] : loc->strings)
        {
            strings[std::to_string(id)] = Latin1(text);
        }
        out["strings"] = std::move(strings);
    }
    return out;
}

Json Gff3Value(const gff3::Struct &node, const gff3::Field &field, const Limits &limits, int depth)
{
    const std::string &label = field.label;
    switch (field.type)
    {
    case gff3::Type::Byte:
        return Number<std::uint8_t>(node, label);
    case gff3::Type::Char:
        return Number<std::int8_t>(node, label);
    case gff3::Type::Word:
        return Number<std::uint16_t>(node, label);
    case gff3::Type::Short:
        return Number<std::int16_t>(node, label);
    case gff3::Type::Dword:
        return Number<std::uint32_t>(node, label);
    case gff3::Type::Int:
        return Number<std::int32_t>(node, label);
    case gff3::Type::Dword64:
        return Number<std::uint64_t>(node, label);
    case gff3::Type::Int64:
        return Number<std::int64_t>(node, label);
    case gff3::Type::Float:
        return Number<float>(node, label);
    case gff3::Type::Double:
        return Number<double>(node, label);
    case gff3::Type::String:
        return Text(node.String(label));
    case gff3::Type::ResRef:
        return Text(node.ResRef(label));
    case gff3::Type::LocString:
        return Gff3Loc(node, label);
    case gff3::Type::StrRef: {
        const auto strref = node.StrRef(label);
        return strref ? Json(*strref) : Unread();
    }
    case gff3::Type::Vector:
    case gff3::Type::Orientation: {
        const auto floats = node.Floats(label);
        return floats.empty() ? Unread() : Json(floats);
    }
    case gff3::Type::Void: {
        const auto bytes = node.Void(label);
        return bytes ? Json{{"bytes", bytes->size()}} : Unread();
    }
    case gff3::Type::Struct: {
        const auto child = node.Child(label);
        return child ? Gff3Struct(*child, limits, depth + 1) : Unread();
    }
    case gff3::Type::List:
        return Gff3List(node, label, limits, depth);
    default:
        return Json{{"_unread", "a field of type " + std::to_string(static_cast<std::uint32_t>(field.type))}};
    }
}

Json Gff3Struct(const gff3::Struct &node, const Limits &limits, int depth)
{
    if (depth > limits.depth)
    {
        return Json{{"_fields", node.Fields().size()}};
    }
    Json out = Json::object();
    for (std::size_t i = 0; i < node.Fields().size(); ++i)
    {
        const auto &field = node.Fields()[i];
        out[field.label.empty() ? "#" + std::to_string(i) : field.label] = Gff3Value(node, field, limits, depth);
    }
    return out;
}

} // namespace

Json Gff3ToJson(const dao::core::gff3::File &file, const Limits &limits)
{
    const auto root = file.Root();
    return root ? Gff3Struct(*root, limits, 0) : Json::object();
}

Json CellJson(const dao::core::GdaFile::Cell &cell)
{
    if (const auto *integer = std::get_if<std::int64_t>(&cell))
    {
        return *integer;
    }
    if (const auto *real = std::get_if<double>(&cell))
    {
        return *real;
    }
    if (const auto *text = std::get_if<std::wstring>(&cell))
    {
        return Utf8(*text);
    }
    return nullptr;
}

std::string ColumnName(std::uint32_t hash)
{
    const auto name = GdaColumnName(hash);
    if (!name.empty())
    {
        return std::string(name);
    }
    char text[16];
    std::snprintf(text, sizeof text, "#%08x", hash);
    return text;
}

std::string ColumnType(const dao::core::GdaFile::Column &column)
{
    return column.type ? std::string(Gff4TypeName(static_cast<std::uint16_t>(*column.type))) : std::string();
}

} // namespace eluvian::ghilana
