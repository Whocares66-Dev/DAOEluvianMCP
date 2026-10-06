#include "game/Convert.h"

#include "core/Args.h"

#include "dao/String.h"

#include <Windows.h>

#include <cmath>
#include <format>

namespace eluvian::game
{

using dao::core::ScriptType;
using dao::script::Value;

std::wstring Wide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string Narrow(std::wstring_view utf16)
{
    return dao::Utf8(utf16);
}

Json IdJson(dao::ObjectId id)
{
    return id == dao::kInvalidObject ? Json(nullptr) : Json(FormatId(id));
}

Json VectorJson(const dao::Vector &v)
{
    return Json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

dao::Vector VectorOf(const Json &value, std::string_view what)
{
    const auto number = [&](const Json &n) {
        if (!n.is_number())
        {
            throw ToolError(std::format(R"({} must be a point: {{"x": 1, "y": 2, "z": 0}} or [1, 2, 0])", what));
        }
        return n.get<float>();
    };
    if (value.is_array() && value.size() == 3)
    {
        return dao::Vector{number(value[0]), number(value[1]), number(value[2])};
    }
    if (value.is_object() && value.contains("x") && value.contains("y"))
    {
        return dao::Vector{number(value["x"]), number(value["y"]), number(value.value("z", Json(0)))};
    }
    number(Json()); // throws the explanation
    return {};
}

Value ValueOf(const Json &value, ScriptType type, std::string_view param)
{
    const auto fail = [&](std::string_view expected) -> Value {
        throw ToolError(std::format("script: `{}` must be {}", param, expected));
    };
    switch (type)
    {
    case ScriptType::Int:
        if (value.is_boolean())
        {
            return Value::Int(value.get<bool>() ? 1 : 0);
        }
        if (value.is_number() && value.get<double>() == std::floor(value.get<double>()))
        {
            return Value::Int(value.get<std::int32_t>());
        }
        return fail("a whole number (an int)");
    case ScriptType::Float:
        return value.is_number() ? Value::Float(value.get<float>()) : fail("a number (a float)");
    case ScriptType::String:
        return value.is_string() ? Value::String(Wide(value.get<std::string>())) : fail("a string");
    case ScriptType::Resource:
        return value.is_string() ? Value::Resource(Wide(value.get<std::string>())) : fail("a resource's name");
    case ScriptType::Object:
        if (value.is_null() || value == "invalid")
        {
            return Value::Object(dao::kInvalidObject);
        }
        if (const auto id = ParseId(value))
        {
            return Value::Object(*id);
        }
        return fail("an object id like \"0x7d9\", or null for OBJECT_INVALID");
    case ScriptType::Vector: {
        const auto v = VectorOf(value, std::format("script: `{}`", param));
        return Value::Vector(v.x, v.y, v.z);
    }
    default:
        return fail(std::format("a {}, which a call by name cannot pass", dao::core::ScriptTypeName(type)));
    }
}

Json JsonOf(const Value &value)
{
    switch (value.type)
    {
    case ScriptType::Void:
        return nullptr;
    case ScriptType::Int:
        return value.integer;
    case ScriptType::Float:
        return value.number;
    case ScriptType::String:
    case ScriptType::Resource:
        return Narrow(value.text);
    case ScriptType::Object:
        return IdJson(value.object);
    case ScriptType::Vector:
        return VectorJson(dao::Vector{value.x, value.y, value.z});
    case ScriptType::Array: {
        Json elements = Json::array();
        for (const auto &element : value.elements)
        {
            elements.push_back(JsonOf(element));
        }
        return elements;
    }
    default:
        return Json{{"unreadable", dao::core::ScriptTypeName(value.type)}};
    }
}

} // namespace eluvian::game
