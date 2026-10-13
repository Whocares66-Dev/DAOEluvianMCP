#include "ghilana/Text.h"

#include <cstdint>

namespace eluvian::ghilana
{

namespace
{

void Append(std::string &out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

char LowerChar(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string Utf8(std::wstring_view text)
{
    constexpr std::uint32_t kReplacement = 0xFFFD;
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto unit = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i]));
        if (unit >= 0xD800 && unit < 0xDC00)
        {
            if (i + 1 < text.size())
            {
                const auto low = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i + 1]));
                if (low >= 0xDC00 && low < 0xE000)
                {
                    Append(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    ++i;
                    continue;
                }
            }
            Append(out, kReplacement);
        }
        else if (unit >= 0xDC00 && unit < 0xE000)
        {
            Append(out, kReplacement);
        }
        else
        {
            Append(out, unit);
        }
    }
    return out;
}

std::wstring Wide(std::string_view text)
{
    std::wstring out;
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (byte < 0x80)
        {
            length = 1;
            code = byte;
        }
        else if ((byte & 0xE0) == 0xC0)
        {
            length = 2;
            code = byte & 0x1F;
        }
        else if ((byte & 0xF0) == 0xE0)
        {
            length = 3;
            code = byte & 0x0F;
        }
        else if ((byte & 0xF8) == 0xF0)
        {
            length = 4;
            code = byte & 0x07;
        }
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(text[i + k]);
            valid = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3F);
        }
        if (!valid)
        {
            out += static_cast<wchar_t>(byte);
            ++i;
            continue;
        }
        if (code >= 0x10000)
        {
            code -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (code >> 10));
            out += static_cast<wchar_t>(0xDC00 + (code & 0x3FF));
        }
        else
        {
            out += static_cast<wchar_t>(code);
        }
        i += length;
    }
    return out;
}

std::string Latin1(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        Append(out, static_cast<unsigned char>(c));
    }
    return out;
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
    {
        c = LowerChar(c);
    }
    return out;
}

bool Glob(std::string_view pattern, std::string_view text)
{
    // The classic walk with one backtrack point: on a mismatch after a `*`,
    // the star takes one more character.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || LowerChar(pattern[p]) == LowerChar(text[t])))
        {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            resume = t;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            t = ++resume;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.size();
}

bool IsPattern(std::string_view text)
{
    return text.find_first_of("*?") != std::string_view::npos;
}

std::string Extension(std::string_view name)
{
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string() : Lower(name.substr(dot + 1));
}

std::string Stem(std::string_view name)
{
    const auto dot = name.rfind('.');
    return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

std::string PathText(const std::filesystem::path &path)
{
    return Utf8(path.generic_wstring());
}

std::string PathKey(std::string_view path)
{
    std::string key = Lower(path);
    for (char &c : key)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    while (key.starts_with("./"))
    {
        key.erase(0, 2);
    }
    while (key.starts_with('/'))
    {
        key.erase(0, 1);
    }
    return key;
}

} // namespace eluvian::ghilana
