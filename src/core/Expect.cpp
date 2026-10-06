#include "core/Expect.h"

#include <format>

namespace eluvian::expect
{

std::optional<Json> At(const Json &answer, const std::string &pointer, std::string_view tool)
{
    try
    {
        const Json::json_pointer path(pointer);
        if (!answer.contains(path))
        {
            return std::nullopt;
        }
        return std::make_optional(answer.at(path));
    }
    catch (const Json::exception &)
    {
        throw ToolError(std::format(R"({}: `pointer` "{}" is not a JSON pointer like "/given")", tool, pointer));
    }
}

std::string Check(const Json &expect, const std::optional<Json> &actual)
{
    const auto describe = [&actual] { return actual ? actual->dump() : std::string("nothing"); };
    if (const auto it = expect.find("exists"); it != expect.end())
    {
        if (it->is_boolean() && actual.has_value() != it->get<bool>())
        {
            return std::format("expected it {}to exist; found {}", it->get<bool>() ? "" : "not ", describe());
        }
    }
    if (const auto it = expect.find("equals"); it != expect.end() && (!actual || *actual != *it))
    {
        return std::format("expected {}; found {}", it->dump(), describe());
    }
    if (const auto it = expect.find("notEquals"); it != expect.end() && actual && *actual == *it)
    {
        return std::format("expected anything but {}", it->dump());
    }
    for (const auto *bound : {"atLeast", "atMost"})
    {
        const auto it = expect.find(bound);
        if (it == expect.end())
        {
            continue;
        }
        if (!it->is_number() || !actual || !actual->is_number())
        {
            return std::format("expected a number {} {}; found {}", bound, it->dump(), describe());
        }
        const double have = actual->get<double>();
        const double limit = it->get<double>();
        if (std::string_view(bound) == "atLeast" ? have < limit : have > limit)
        {
            return std::format("expected {} {}; found {}", bound, it->dump(), describe());
        }
    }
    return {};
}

} // namespace eluvian::expect
