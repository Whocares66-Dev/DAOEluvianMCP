#include "core/Origin.h"

#include <algorithm>
#include <array>

namespace eluvian
{

bool IsLocalOrigin(std::string_view origin)
{
    if (origin.empty())
    {
        return true;
    }
    for (const std::string_view scheme : {"http://", "https://"})
    {
        if (!origin.starts_with(scheme))
        {
            continue;
        }
        std::string_view host = origin.substr(scheme.size());
        // An IPv6 host is bracketed, and holds colons of its own.
        const auto hostEnd = host.starts_with('[') ? host.find(']') : 0;
        if (hostEnd == std::string_view::npos)
        {
            return false;
        }
        if (const auto colon = host.find(':', hostEnd); colon != std::string_view::npos)
        {
            const auto port = host.substr(colon + 1);
            if (port.empty() || !std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; }))
            {
                return false;
            }
            host = host.substr(0, colon);
        }
        constexpr std::array<std::string_view, 3> kLocal{"localhost", "127.0.0.1", "[::1]"};
        return std::ranges::find(kLocal, host) != kLocal.end();
    }
    return false;
}

} // namespace eluvian
