#include "core/GfxList.h"

#include <charconv>

namespace eluvian
{

std::vector<std::int64_t> ListNumbers(std::string_view joined)
{
    std::vector<std::int64_t> numbers;
    while (!joined.empty())
    {
        const auto comma = joined.find(',');
        std::string_view element = joined.substr(0, comma);
        joined = comma == std::string_view::npos ? std::string_view() : joined.substr(comma + 1);
        while (!element.empty() && element.front() == ' ')
        {
            element.remove_prefix(1);
        }
        while (!element.empty() && element.back() == ' ')
        {
            element.remove_suffix(1);
        }
        // A whole number, or one written with a fraction of none ("12.0").
        double value = 0;
        const auto [end, error] = std::from_chars(element.data(), element.data() + element.size(), value);
        if (error == std::errc() && end == element.data() + element.size() && !element.empty())
        {
            numbers.push_back(static_cast<std::int64_t>(value));
        }
    }
    return numbers;
}

} // namespace eluvian
