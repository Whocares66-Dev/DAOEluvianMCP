#include "core/Args.h"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

namespace eluvian
{

std::string FormatId(std::uint32_t id)
{
    return std::format("{:#x}", id);
}

std::optional<std::uint32_t> ParseId(const Json &value)
{
    if (value.is_number_unsigned())
    {
        const auto n = value.get<std::uint64_t>();
        if (n <= std::numeric_limits<std::uint32_t>::max())
        {
            return static_cast<std::uint32_t>(n);
        }
        return std::nullopt;
    }
    if (value.is_number_integer()) // JSON parsed from text makes non-negatives unsigned; a C++ int is signed
    {
        const auto n = value.get<std::int64_t>();
        if (n >= 0 && n <= std::int64_t{std::numeric_limits<std::uint32_t>::max()})
        {
            return static_cast<std::uint32_t>(n);
        }
        return std::nullopt;
    }
    if (!value.is_string())
    {
        return std::nullopt;
    }
    std::string_view text = value.get_ref<const std::string &>();
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    std::uint32_t id = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id, base);
    if (error != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return id;
}

Args::Args(const Json &args, std::string_view tool) : args_(args.is_null() ? Json::object() : args), tool_(tool)
{
    if (!args_.is_object())
    {
        throw ToolError(tool_ + ": the arguments must be an object");
    }
}

const Json *Args::Find(std::string_view key) const
{
    const auto it = args_.find(key);
    return it == args_.end() || it->is_null() ? nullptr : &*it;
}

void Args::Fail(std::string_view key, std::string_view expected) const
{
    throw ToolError(std::format("{}: `{}` must be {}", tool_, key, expected));
}

bool Args::Has(std::string_view key) const
{
    return Find(key) != nullptr;
}

std::string Args::String(std::string_view key) const
{
    const Json *value = Find(key);
    if (value == nullptr || !value->is_string())
    {
        Fail(key, "given, as a string");
    }
    return value->get<std::string>();
}

std::string Args::String(std::string_view key, std::string fallback) const
{
    return Has(key) ? String(key) : std::move(fallback);
}

double Args::Number(std::string_view key) const
{
    const Json *value = Find(key);
    if (value == nullptr || !value->is_number())
    {
        Fail(key, "given, as a number");
    }
    return value->get<double>();
}

double Args::Number(std::string_view key, double fallback) const
{
    return Has(key) ? Number(key) : fallback;
}

std::int64_t Args::Integer(std::string_view key, std::int64_t fallback, std::int64_t min, std::int64_t max) const
{
    const Json *value = Find(key);
    if (value == nullptr)
    {
        return fallback;
    }
    const auto range = std::format("a whole number from {} to {}", min, max);
    if (!value->is_number())
    {
        Fail(key, range);
    }
    const double n = value->get<double>();
    if (n != std::floor(n) || n < static_cast<double>(min) || n > static_cast<double>(max))
    {
        Fail(key, range);
    }
    return static_cast<std::int64_t>(n);
}

bool Args::Bool(std::string_view key, bool fallback) const
{
    const Json *value = Find(key);
    if (value == nullptr)
    {
        return fallback;
    }
    if (!value->is_boolean())
    {
        Fail(key, "true or false");
    }
    return value->get<bool>();
}

std::uint32_t Args::Id(std::string_view key, std::string_view what) const
{
    const Json *value = Find(key);
    const auto id = value == nullptr ? std::nullopt : ParseId(*value);
    if (!id)
    {
        Fail(key, std::format("an object id like \"0x7d9\": {}", what));
    }
    return *id;
}

std::optional<std::uint32_t> Args::OptionalId(std::string_view key, std::string_view what) const
{
    return Has(key) ? std::optional{Id(key, what)} : std::nullopt;
}

std::string Args::Choice(std::string_view key, std::initializer_list<std::string_view> values) const
{
    std::string list;
    for (const auto value : values)
    {
        list += list.empty() ? "" : ", ";
        list += value;
    }
    const Json *value = Find(key);
    if (value != nullptr && value->is_string())
    {
        const auto &text = value->get_ref<const std::string &>();
        for (const auto allowed : values)
        {
            if (text == allowed)
            {
                return text;
            }
        }
    }
    Fail(key, "one of: " + list);
}

std::string Args::Choice(std::string_view key, std::initializer_list<std::string_view> values,
                         std::string_view fallback) const
{
    return Has(key) ? Choice(key, values) : std::string(fallback);
}

} // namespace eluvian
