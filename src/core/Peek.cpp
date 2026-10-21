#include "core/Peek.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace eluvian
{

namespace
{

// The engine's std::wstring is MSVC 8's (DAOLib's dao/core/Msvc8.h, read in
// its constructor 0x004C0D90 and destructor 0x004C0DB0): an allocator padded
// to 4 bytes, a 16-byte union of an inline buffer of 8 wide characters and a
// pointer to the heap, the length at +0x14 and the capacity at +0x18; the
// characters are inline while the capacity is 7.
constexpr std::uint32_t kStringData = 0x04;
constexpr std::uint32_t kStringSize = 0x14;
constexpr std::uint32_t kStringCapacity = 0x18;
constexpr std::uint32_t kStringInline = 7;
// A length past this is no string, or one no answer wants whole.
constexpr std::uint32_t kStringMost = 4096;
constexpr std::size_t kMostCount = 1024;

std::string OffsetText(std::int64_t offset)
{
    std::array<char, 24> text{};
    const auto size = offset < 0 ? static_cast<std::uint64_t>(-offset) : static_cast<std::uint64_t>(offset);
    std::snprintf(text.data(), text.size(), "%s0x%llx", offset < 0 ? "-" : "+", static_cast<unsigned long long>(size));
    return text.data();
}

// The address `offset` from `at`, as the game's 32-bit arithmetic has it.
std::uint32_t Moved(std::uint32_t at, std::int64_t offset)
{
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(at) + offset);
}

template <class T> std::optional<T> Read(const PeekMemory &memory, std::uint32_t address)
{
    T value{};
    if (!memory.read(address, &value, sizeof value))
    {
        return std::nullopt;
    }
    return value;
}

Json Pointer(const PeekMemory &memory, std::uint32_t pointer)
{
    Json out = AddressText(pointer);
    if (memory.className)
    {
        if (std::string name = memory.className(pointer); !name.empty())
        {
            return Json{{"pointer", std::move(out)}, {"class", std::move(name)}};
        }
    }
    return Json{{"pointer", std::move(out)}};
}

std::optional<Json> Floats(const PeekMemory &memory, std::uint32_t address, std::size_t size)
{
    std::vector<float> values(size);
    if (!memory.read(address, values.data(), size * sizeof(float)))
    {
        return std::nullopt;
    }
    return Json(values);
}

std::optional<Json> WideString(const PeekMemory &memory, std::uint32_t address)
{
    const auto size = Read<std::uint32_t>(memory, address + kStringSize);
    const auto capacity = Read<std::uint32_t>(memory, address + kStringCapacity);
    if (!size || !capacity)
    {
        return std::nullopt;
    }
    if (*size > kStringMost || *size > *capacity)
    {
        return Json{{"not_a_string", true}, {"length", *size}, {"capacity", *capacity}};
    }
    std::uint32_t characters = address + kStringData;
    if (*capacity > kStringInline)
    {
        const auto pointer = Read<std::uint32_t>(memory, characters);
        if (!pointer)
        {
            return std::nullopt;
        }
        characters = *pointer;
    }
    std::u16string text(*size, u'\0');
    if (!memory.read(characters, text.data(), text.size() * 2))
    {
        return std::nullopt;
    }
    std::string utf8;
    for (const char16_t unit : text)
    {
        // The engine's names are ASCII; anything else is shown as its code.
        if (unit >= 0x20 && unit < 0x7F)
        {
            utf8 += static_cast<char>(unit);
        }
        else
        {
            std::array<char, 12> escaped{};
            std::snprintf(escaped.data(), escaped.size(), "\\u%04x", static_cast<unsigned>(unit));
            utf8 += escaped.data();
        }
    }
    return Json(utf8);
}

std::optional<Json> Bytes(const PeekMemory &memory, std::uint32_t address, std::size_t count)
{
    std::vector<unsigned char> bytes(count);
    if (!memory.read(address, bytes.data(), count))
    {
        return std::nullopt;
    }
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        std::array<char, 4> pair{};
        std::snprintf(pair.data(), pair.size(), "%02x", static_cast<unsigned>(bytes[i]));
        text += (i == 0 ? "" : " ");
        text += pair.data();
    }
    return Json(text);
}

template <class T> std::optional<Json> Numbers(const PeekMemory &memory, std::uint32_t address, std::size_t count)
{
    std::vector<T> values(count);
    if (!memory.read(address, values.data(), count * sizeof(T)))
    {
        return std::nullopt;
    }
    if (count == 1)
    {
        return Json(values[0]);
    }
    return Json(values);
}

// One value of a type that is not a number; a list of `count`, each `size` bytes on.
std::optional<Json> Each(std::size_t count, std::uint32_t address, std::uint32_t size,
                         const std::function<std::optional<Json>(std::uint32_t)> &one)
{
    if (count == 1)
    {
        return one(address);
    }
    Json list = Json::array();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto value = one(address + static_cast<std::uint32_t>(i * size));
        if (!value)
        {
            return std::nullopt;
        }
        list.push_back(std::move(*value));
    }
    return list;
}

std::optional<Json> Value(const PeekMemory &memory, std::uint32_t address, std::string_view type, std::size_t count)
{
    if (type == "f32")
    {
        return Numbers<float>(memory, address, count);
    }
    if (type == "i32")
    {
        return Numbers<std::int32_t>(memory, address, count);
    }
    if (type == "u32")
    {
        return Numbers<std::uint32_t>(memory, address, count);
    }
    if (type == "u16")
    {
        return Numbers<std::uint16_t>(memory, address, count);
    }
    if (type == "u8")
    {
        return Numbers<std::uint8_t>(memory, address, count);
    }
    if (type == "bytes")
    {
        return Bytes(memory, address, count);
    }
    if (type == "ptr")
    {
        return Each(count, address, 4, [&](std::uint32_t at) -> std::optional<Json> {
            const auto pointer = Read<std::uint32_t>(memory, at);
            return pointer ? std::optional(Pointer(memory, *pointer)) : std::nullopt;
        });
    }
    if (type == "vec3" || type == "quat")
    {
        const std::uint32_t floats = type == "vec3" ? 3 : 4;
        return Each(count, address, floats * 4, [&](std::uint32_t at) { return Floats(memory, at, floats); });
    }
    return Each(count, address, 0x1C, [&](std::uint32_t at) { return WideString(memory, at); });
}

} // namespace

std::string AddressText(std::uint32_t address)
{
    std::array<char, 12> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", static_cast<unsigned>(address));
    return text.data();
}

std::vector<std::int64_t> PeekOffsets(const Json &offsets)
{
    if (offsets.is_null())
    {
        return {};
    }
    if (!offsets.is_array())
    {
        throw ToolError(R"(peek: `offsets` is a list, such as ["0x7B0", "0x64"])");
    }
    std::vector<std::int64_t> out;
    for (const Json &offset : offsets)
    {
        if (offset.is_number_integer())
        {
            out.push_back(offset.get<std::int64_t>());
            continue;
        }
        const std::string text = offset.is_string() ? offset.get<std::string>() : "";
        const bool negative = text.starts_with('-');
        const std::string digits = negative ? text.substr(1) : text;
        const bool hex = digits.starts_with("0x") || digits.starts_with("0X");
        std::size_t used = 0;
        std::int64_t value = 0;
        try
        {
            value = std::stoll(hex ? digits.substr(2) : digits, &used, hex ? 16 : 10);
        }
        catch (const std::exception &)
        {
            used = 0;
        }
        if (digits.empty() || used != digits.size() - (hex ? 2 : 0))
        {
            throw ToolError("peek: offset " + offset.dump() + R"( is no number; give 1968, "0x7B0" or "-0x10")");
        }
        out.push_back(negative ? -value : value);
    }
    return out;
}

Json Peek(const PeekMemory &memory, std::uint32_t base, const std::vector<std::int64_t> &offsets, std::string_view type,
          std::size_t count)
{
    if (std::ranges::find(kPeekTypes, type) == std::end(kPeekTypes))
    {
        throw ToolError("peek: type \"" + std::string(type) +
                        "\" is none of f32, i32, u32, u16, u8, ptr, vec3, quat, wstring, bytes");
    }
    if (count == 0 || count > kMostCount)
    {
        throw ToolError("peek: `count` is 1 to 1024");
    }
    Json out = {{"base", AddressText(base)}};
    if (memory.className)
    {
        if (std::string name = memory.className(base); !name.empty())
        {
            out["class"] = std::move(name);
        }
    }
    Json steps = Json::array();
    std::uint32_t at = base;
    for (std::size_t i = 0; i + 1 < offsets.size(); ++i)
    {
        const std::uint32_t from = Moved(at, offsets[i]);
        const auto pointer = Read<std::uint32_t>(memory, from);
        if (!pointer)
        {
            out["steps"] = std::move(steps);
            out["readable"] = false;
            out["why"] = "the pointer at " + AddressText(from) + " (" + OffsetText(offsets[i]) + ") cannot be read";
            return out;
        }
        Json step = Pointer(memory, *pointer);
        step["offset"] = OffsetText(offsets[i]);
        step["at"] = AddressText(from);
        steps.push_back(std::move(step));
        at = *pointer;
    }
    const std::uint32_t address = offsets.empty() ? at : Moved(at, offsets.back());
    out["steps"] = std::move(steps);
    out["address"] = AddressText(address);
    out["type"] = std::string(type);
    auto value = Value(memory, address, type, count);
    if (!value)
    {
        out["readable"] = false;
        out["why"] = "the value at " + AddressText(address) + " cannot be read";
        return out;
    }
    out["value"] = std::move(*value);
    return out;
}

} // namespace eluvian
