// libspng's encoder into a buffer of its own (SPNG_ENCODE_TO_BUFFER), as
// its documentation's example writes one: the header, the image finalized,
// then the buffer taken, which is then the caller's to free. It was a
// deflate and filters of ghilana's own until the user asked why a library
// did not do it (2026-10-12).
#include "ghilana/Png.h"

#include <spng.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace eluvian::ghilana
{

namespace
{

struct ContextFree
{
    void operator()(spng_ctx *context) const
    {
        spng_ctx_free(context);
    }
};

void Check(int error, const char *what)
{
    if (error != 0)
    {
        throw std::runtime_error(std::string("EncodePng: ") + what + ": " + spng_strerror(error));
    }
}

} // namespace

std::vector<std::byte> EncodePng(std::uint32_t width, std::uint32_t height, int channels,
                                 std::span<const std::uint8_t> pixels)
{
    // PNG's colour types for 1 to 4 channels: grey, grey and alpha, truecolour, and alpha.
    static constexpr std::array<std::uint8_t, 5> kColourType{0, SPNG_COLOR_TYPE_GRAYSCALE,
                                                             SPNG_COLOR_TYPE_GRAYSCALE_ALPHA, SPNG_COLOR_TYPE_TRUECOLOR,
                                                             SPNG_COLOR_TYPE_TRUECOLOR_ALPHA};
    if (channels < 1 || channels > 4 ||
        pixels.size() != std::size_t{width} * height * static_cast<std::size_t>(channels))
    {
        throw std::invalid_argument("EncodePng: the pixels are not width * height * channels bytes");
    }
    const std::unique_ptr<spng_ctx, ContextFree> context(spng_ctx_new(SPNG_CTX_ENCODER));
    if (!context)
    {
        throw std::runtime_error("EncodePng: libspng made no encoder");
    }
    Check(spng_set_option(context.get(), SPNG_ENCODE_TO_BUFFER, 1), "to a buffer");
    spng_ihdr header{};
    header.width = width;
    header.height = height;
    header.bit_depth = 8;
    header.color_type = kColourType[static_cast<std::size_t>(channels)];
    Check(spng_set_ihdr(context.get(), &header), "the header");
    Check(spng_encode_image(context.get(), pixels.data(), pixels.size(), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE),
          "the picture");
    std::size_t size = 0;
    int error = 0;
    void *buffer = spng_get_png_buffer(context.get(), &size, &error);
    Check(error, "the file");
    const std::unique_ptr<void, decltype(&std::free)> owned(buffer, &std::free);
    const auto *bytes = static_cast<const std::byte *>(buffer);
    return {bytes, bytes + size};
}

} // namespace eluvian::ghilana
