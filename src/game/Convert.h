// Between the agent's JSON and the game's values: text (UTF-8 to the
// engine's UTF-16), object ids ("0x7d9", as the logs write them), vectors,
// and DAOLib's run-time script values (dao/ScriptCall.h).
#pragma once

#include "core/Tool.h"

#include "dao/ObjectId.h"
#include "dao/ScriptCall.h"
#include "dao/Structures.h"

#include <string>
#include <string_view>

namespace eluvian::game
{

std::wstring Wide(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);

// "0x7d9"; null for OBJECT_INVALID.
Json IdJson(dao::ObjectId id);

Json VectorJson(const dao::Vector &v);
// {x, y, z} or [x, y, z]; throws ToolError naming `what`.
dao::Vector VectorOf(const Json &value, std::string_view what);

// A JSON argument as a script value of `type`, for parameter `param`;
// throws ToolError saying what was expected.
dao::script::Value ValueOf(const Json &value, dao::core::ScriptType type, std::string_view param);

// A script value as JSON: numbers, text, ids, {x, y, z}, arrays as lists.
Json JsonOf(const dao::script::Value &value);

} // namespace eluvian::game
