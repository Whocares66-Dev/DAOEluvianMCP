// A picture as a PNG file, for `texture`: the agent reads images, and a DDS
// is not one it can. Encoded by libspng (vcpkg's libspng, over zlib), which
// the tests decode it back with.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace eluvian::ghilana
{

// A PNG file of a picture: rows from the top, `channels` bytes a pixel
// (1 grey, 2 grey and alpha, 3 red, green, blue, 4 and alpha), 8 bits each.
// Throws std::invalid_argument for pixels of another size, std::runtime_error
// if libspng refuses.
std::vector<std::byte> EncodePng(std::uint32_t width, std::uint32_t height, int channels,
                                 std::span<const std::uint8_t> pixels);

} // namespace eluvian::ghilana
