#include "core/Base64.h"

#include <cstddef>
#include <cstdint>

namespace eluvian
{

std::string Base64(std::string_view bytes)
{
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t at = 0;
    for (; at + 3 <= bytes.size(); at += 3)
    {
        const std::uint32_t chunk = (static_cast<std::uint8_t>(bytes[at]) << 16) |
                                    (static_cast<std::uint8_t>(bytes[at + 1]) << 8) |
                                    static_cast<std::uint8_t>(bytes[at + 2]);
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += kAlphabet[chunk & 0x3F];
    }
    const std::size_t left = bytes.size() - at;
    if (left > 0)
    {
        std::uint32_t chunk = static_cast<std::uint8_t>(bytes[at]) << 16;
        if (left == 2)
        {
            chunk |= static_cast<std::uint8_t>(bytes[at + 1]) << 8;
        }
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += left == 2 ? kAlphabet[(chunk >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

} // namespace eluvian
