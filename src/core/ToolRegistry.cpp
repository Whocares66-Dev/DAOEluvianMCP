#include "core/ToolRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <string>

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
