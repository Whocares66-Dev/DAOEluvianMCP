#include "core/ToolRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace eluvian
{

namespace
{

// How many single-letter edits make one name the other.
std::size_t Distance(std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
    {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

} // namespace

bool ToolRegistry::Add(ToolSpec spec, ToolHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("tool " + spec.name + " has no handler");
    }
    const std::lock_guard lock(mutex_);
    ++revision_;
    const bool added = !tools_.contains(spec.name);
    Entry &entry = tools_[spec.name];
    entry.spec = std::move(spec);
    entry.handler = std::move(handler);
    return added;
}

std::optional<Call> ToolRegistry::Resolve(std::string_view tool) const
{
    const std::lock_guard lock(mutex_);
    const auto it = tools_.find(tool);
    if (it == tools_.end())
    {
        return std::nullopt;
    }
    return Call{it->second.spec.name, it->second.spec.thread, it->second.handler};
}

// An argument a tool does not have was ignored without a word: `events`
// with `topics` read every topic, its filter being `topic`
// (turn-based-agent, 2026-10-05, #61).
Json ToolRegistry::Unknown(std::string_view tool, const Json &args) const
{
    const std::lock_guard lock(mutex_);
    Json unknown = Json::object();
    const auto it = tools_.find(tool);
    if (it == tools_.end() || !args.is_object())
    {
        return unknown;
    }
    const Entry &entry = it->second;
    if (!entry.spec.inputSchema.is_object())
    {
        return unknown;
    }
    const Json properties = entry.spec.inputSchema.value("properties", Json::object());
    if (!properties.is_object() || properties.empty())
    {
        return unknown;
    }
    for (const auto &[name, value] : args.items())
    {
        if (properties.contains(name))
        {
            continue;
        }
        std::string nearest;
        std::size_t best = name.size();
        std::string all;
        for (const auto &[known, schema] : properties.items())
        {
            all += (all.empty() ? "`" : ", `") + known + "`";
            const std::size_t distance = Distance(name, known);
            if (distance < best)
            {
                best = distance;
                nearest = known;
            }
        }
        // Near enough to be the one meant: a letter or two off, or a third
        // of the name.
        const bool near = !nearest.empty() && best <= std::max<std::size_t>(2, name.size() / 3);
        unknown[name] = "not an argument; " + (near ? "did you mean `" + nearest + "`?" : "it takes " + all + ".");
    }
    return unknown;
}

// A schema's `required` was the client's to check: MCP clients may, a
// REST caller does not, and each handler checked what it read, or did
// not (`tactics set` without `command` answered a JSON library's
// exception, eluvian.log, 2026-10-09, #110). The schema says it once.
std::vector<std::string> ToolRegistry::Missing(std::string_view tool, const Json &args) const
{
    const std::lock_guard lock(mutex_);
    std::vector<std::string> missing;
    const auto it = tools_.find(tool);
    if (it == tools_.end() || !it->second.spec.inputSchema.is_object())
    {
        return missing;
    }
    const Json required = it->second.spec.inputSchema.value("required", Json::array());
    if (!required.is_array())
    {
        return missing;
    }
    for (const auto &name : required)
    {
        if (name.is_string() && !(args.is_object() && args.contains(name.get<std::string>())))
        {
            missing.push_back(name.get<std::string>());
        }
    }
    return missing;
}

Json ToolRegistry::Describe() const
{
    const std::lock_guard lock(mutex_);
    Json tools = Json::array();
    for (const auto &[name, entry] : tools_)
    {
        if (!entry.spec.listed)
        {
            continue;
        }
        Json tool = {{"name", name}, {"description", entry.spec.description}, {"inputSchema", entry.spec.inputSchema}};
        if (entry.spec.readOnly)
        {
            tool["annotations"] = {{"readOnlyHint", true}};
        }
        if (entry.spec.budget)
        {
            tool["_meta"] = {
                {"eluvian/budget",
                 {{"argument", entry.spec.budget->argument}, {"defaultMs", entry.spec.budget->defaultMs}}}};
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::vector<std::string> ToolRegistry::Names() const
{
    const std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto &[name, entry] : tools_)
    {
        names.push_back(name);
    }
    return names;
}

std::uint64_t ToolRegistry::Revision() const
{
    const std::lock_guard lock(mutex_);
    return revision_;
}

} // namespace eluvian
