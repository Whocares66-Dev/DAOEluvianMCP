#include "core/Stdio.h"

#include <istream>
#include <ostream>

namespace eluvian
{

std::string OneLine(const Json &value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<std::string> McpLine(McpEndpoint &endpoint, std::string_view line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    {
        line.remove_suffix(1);
    }
    if (line.find_first_not_of(" \t") == std::string_view::npos)
    {
        return std::nullopt;
    }
    Json message;
    try
    {
        message = Json::parse(line);
    }
    catch (const Json::parse_error &error)
    {
        return OneLine(McpEndpoint::ParseError(error.what()));
    }
    auto reply = endpoint.Handle(message);
    if (!reply)
    {
        return std::nullopt;
    }
    return OneLine(*reply);
}

void ServeStdio(McpEndpoint &endpoint, std::istream &in, std::ostream &out)
{
    std::string line;
    while (std::getline(in, line))
    {
        if (const auto reply = McpLine(endpoint, line))
        {
            out << *reply << '\n';
            out.flush();
        }
    }
}

} // namespace eluvian
