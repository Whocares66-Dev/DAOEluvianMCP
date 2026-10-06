#include "core/Erf.h"

#include <cstdint>
#include <string_view>

namespace eluvian
{

namespace
{

struct Layout
{
    std::size_t header; // bytes before the first entry
    std::size_t entry;  // bytes of each entry
};

// V2.0: the name, a count, a year, a day and a word, then name, offset and
// size. V2.2 adds flags, a module id and a password's digest to the header
// and a packed size to each entry.
constexpr Layout kV20{32, 72};
constexpr Layout kV22{56, 76};
constexpr std::size_t kNameBytes = 64;

std::uint32_t Word(std::span<const std::byte> bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// The UTF-16 text at `at`, up to `chars` of it or its first nul. Resource
// names are ASCII; anything else is kept as '?'.
std::string Text(std::span<const std::byte> bytes, std::size_t at, std::size_t chars)
{
    std::string text;
    for (std::size_t i = 0; i < chars && at + (2 * i) + 1 < bytes.size(); ++i)
    {
        const auto unit = static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[at + (2 * i)]) |
                                                     static_cast<std::uint16_t>(bytes[at + (2 * i) + 1]) << 8);
        if (unit == 0)
        {
            break;
        }
        text += unit < 0x80 ? static_cast<char>(unit) : '?';
    }
    return text;
}

std::optional<Layout> LayoutOf(std::span<const std::byte> bytes)
{
    if (bytes.size() < kErfHeadBytes)
    {
        return std::nullopt;
    }
    const std::string magic = Text(bytes, 0, 8);
    if (magic == "ERF V2.0")
    {
        return kV20;
    }
    if (magic == "ERF V2.2")
    {
        return kV22;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::size_t> ErfIndexBytes(std::span<const std::byte> head)
{
    const auto layout = LayoutOf(head);
    if (!layout)
    {
        return std::nullopt;
    }
    return layout->header + (static_cast<std::size_t>(Word(head, 16)) * layout->entry);
}

std::vector<std::string> ErfNames(std::span<const std::byte> index)
{
    const auto layout = LayoutOf(index);
    if (!layout)
    {
        return {};
    }
    const std::size_t count = Word(index, 16);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t at = layout->header + (i * layout->entry);
        if (at + kNameBytes > index.size())
        {
            break;
        }
        if (auto name = Text(index, at, kNameBytes / 2); !name.empty())
        {
            names.push_back(std::move(name));
        }
    }
    return names;
}

} // namespace eluvian
