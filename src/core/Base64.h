// Base64, as MCP's image content carries a picture (RFC 4648, with padding).
#pragma once

#include <string>
#include <string_view>

namespace eluvian
{

std::string Base64(std::string_view bytes);

} // namespace eluvian
