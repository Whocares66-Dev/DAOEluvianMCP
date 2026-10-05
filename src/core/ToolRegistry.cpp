#include "core/ToolRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace eluvian
{

namespace
{

std::string KindList(const auto &kinds)
{
    std::string list;
    for (const auto &[name, kind] : kinds)
    {
        if (!list.empty())
        {
            list += ", ";
        }
        list += name;
    }
    return list.empty() ? "(none registered)" : list;
}

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
    const std::lock_guard lock(mutex_);
    ++revision_;
    // Built where it lives: an Entry is never moved, since MSVC's map
    // allocates when moved and the move would be a noexcept that can throw.
    const bool added = !tools_.contains(spec.name);
    Entry &entry = tools_[spec.name];
    entry.spec = std::move(spec);
    entry.handler = std::move(handler);
    entry.kinds.clear();
    return added;
}

bool ToolRegistry::AddKind(std::string_view tool, KindSpec spec, ToolHandler handler)
{
    const std::lock_guard lock(mutex_);
    const auto it = tools_.find(tool);
    if (it == tools_.end())
    {
        throw std::invalid_argument("no tool " + std::string(tool) + " to add a kind to");
    }
    if (it->second.handler)
    {
        throw std::invalid_argument("tool " + std::string(tool) + " has a handler of its own, not kinds");
    }
    ++revision_;
    const bool added = !it->second.kinds.contains(spec.kind);
    Kind &entry = it->second.kinds[spec.kind];
    entry.spec = std::move(spec);
    entry.handler = std::move(handler);
    return added;
}

std::optional<Call> ToolRegistry::Resolve(std::string_view tool, const Json &args) const
{
    const std::lock_guard lock(mutex_);
    const auto it = tools_.find(tool);
    if (it == tools_.end())
    {
        return std::nullopt;
    }
    const Entry &entry = it->second;
    if (entry.handler)
    {
        return Call{entry.spec.name, {}, entry.spec.thread, entry.handler};
    }
    const std::string &property = entry.spec.kindProperty;
    const auto kindArg = args.is_object() ? args.find(property) : args.end();
    if (kindArg == args.end() || !kindArg->is_string())
    {
        throw ToolError(entry.spec.name + " needs `" + property + "`, one of: " + KindList(entry.kinds));
    }
    const auto &kindName = kindArg->get_ref<const std::string &>();
    const auto kit = entry.kinds.find(kindName);
    if (kit == entry.kinds.end())
    {
        throw ToolError(entry.spec.name + " has no " + property + " \"" + kindName +
                        "\"; the choices are: " + KindList(entry.kinds));
    }
    return Call{entry.spec.name, kindName, kit->second.spec.thread, kit->second.handler};
}

// An argument a tool does not have was ignored without a word: `events`
// with `topics` read every topic, its filter being `topic`
// (turn-based-agent, 2026-10-05, #61). A kind of a base tool shares its
// tool's schema, whose properties are its kinds' together.
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
        if (properties.contains(name) || (!entry.handler && name == entry.spec.kindProperty))
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
        unknown[name] = "not an argument of " + std::string(tool) + "; " +
                        (near ? "did you mean `" + nearest + "`?" : "it takes " + all);
    }
    return unknown;
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
        std::string description = entry.spec.description;
        Json schema = entry.spec.inputSchema;
        if (!entry.handler)
        {
            Json kinds = Json::array();
            const std::string &property = entry.spec.kindProperty;
            description += "\n\n`" + property + "`:";
            for (const auto &[kindName, kind] : entry.kinds)
            {
                kinds.push_back(kindName);
                description += "\n- " + kindName + ": " + kind.spec.description;
            }
            if (entry.kinds.empty())
            {
                description += " none registered yet.";
            }
            schema["type"] = "object";
            schema["properties"][property]["type"] = "string";
            schema["properties"][property]["enum"] = kinds;
            Json required = schema.value("required", Json::array());
            if (std::find(required.begin(), required.end(), property) == required.end())
            {
                required.push_back(property);
            }
            schema["required"] = required;
        }
        Json tool = {{"name", name}, {"description", description}, {"inputSchema", schema}};
        if (entry.spec.readOnly)
        {
            tool["annotations"] = {{"readOnlyHint", true}};
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
