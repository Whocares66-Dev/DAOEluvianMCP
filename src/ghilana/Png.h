// A picture as a PNG file, for `texture`: the agent reads images, and a DDS
// is not one it can; and for `texture_export` and `texture_import`, a
// texture's way to an image editor and back (#135). Encoded and decoded by
// libspng (vcpkg's libspng, over zlib).
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

// A PNG file's picture, whatever its colour type and depth, made 8 bits of
// red, green, blue and alpha a pixel, rows from the top.
struct PngImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// Throws std::runtime_error, saying why, for bytes libspng does not read as a PNG.
PngImage DecodePng(std::span<const std::byte> file);

} // namespace eluvian::ghilana
